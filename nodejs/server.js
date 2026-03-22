/*
* Instalar librerias:
$ npm install ws wav
* Ejecución:
$ node server.js
*/

const WebSocket = require('ws');
const fs = require('fs');
const wav = require('wav');
const path = require('path');

const WS_PORT = 8888;
const UPLOADS_DIR = 'uploads';

const WAV_HEADER_SIZE = 44;
const CHUNK_SIZE = 1024;

// Crear la carpeta 'uploads' si no existe
if (!fs.existsSync(UPLOADS_DIR)) {
    fs.mkdirSync(UPLOADS_DIR);
    console.log(`Directorio '${UPLOADS_DIR}' creado.`);
}

const wsServer = new WebSocket.Server({ port: WS_PORT });

let fileWriter = null;
let isFirstChunk = true;
let currentFilePath = null; // Para mantener la ruta del archivo actual

wsServer.on('connection', function connection(ws) {
    console.log('Cliente (ESP32-S3) conectado.');

    // Reiniciar el estado para una nueva conexión/grabación
    isFirstChunk = true;
    if (fileWriter) {
        fileWriter.end();
        fileWriter = null;
    }

    ws.on('message', (data) => {
        // Se asume que 'data' es un chunk de audio binario
        console.log(`Recibido chunk de audio, tamaño: ${data.length}`);

        if (isFirstChunk) {
            // Generar un nuevo nombre de archivo para la grabación
            const fileName = `recording-${Date.now()}.wav`;
            currentFilePath = path.join(UPLOADS_DIR, fileName);

            console.log(`Creando nueva grabación: ${currentFilePath}`);
            fileWriter = new wav.FileWriter(currentFilePath, {
                channels: 1,       // Mono
                sampleRate: 44100, // Frecuencia de muestreo
                bitDepth: 16       // Profundidad de bits
            });
            isFirstChunk = false;
        }

        // Escribir el chunk de audio en el archivo .wav
        if (fileWriter) {
            fileWriter.write(data);
        }
    });

    let silenceTimeout = null;

    // Usamos un segundo listener de 'message' para manejar la detección de silencio
    ws.on('message', () => {
        // Reiniciar el temporizador cada vez que llega un chunk
        clearTimeout(silenceTimeout);
        silenceTimeout = setTimeout(() => {
            console.log('Silencio detectado, finalizando la grabación.');
            
            if (fileWriter) {
                fileWriter.end(() => {
                    console.log(`Grabación guardada correctamente en: ${currentFilePath}`);
                    // Stramear el audio grabado al esp32s3
                    streamResponseAudio(ws, currentFilePath);

                    currentFilePath = null; // Limpiar la ruta
                });
                fileWriter = null;
            }
            
            // Restablecer para la siguiente grabación
            isFirstChunk = true;

        }, 5000); // Actualizado a 5 segundos de silencio para finalizar la grabación (ajustar este valor).  Tenia 1000 pero probando detenia la recepción del audio, con 5000 va fino!.
    });

    ws.on('close', () => {
        console.log('Cliente desconectado.');
        // Asegurarse de que el archivo se cierra si la conexión se interrumpe
        if (fileWriter) {
            fileWriter.end(() => {
                console.log(`Grabación finalizada por desconexión y guardada en: ${currentFilePath}`);
            });
            fileWriter = null;
        }
    });

    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
        if (fileWriter) {
            fileWriter.end();
            fileWriter = null;
        }
    });
});

// Metodo usado para enviar audio en stream al esp32
function streamResponseAudio(ws, filename) {
    console.log(`Opening ${filename} for streaming`);
    const audioStream = fs.createReadStream(filename, {
        highWaterMark: CHUNK_SIZE,
        start: WAV_HEADER_SIZE
    });
    
    let chunkCount = 0;
    audioStream.on('data', (chunk) => {
        if (ws.readyState === WebSocket.OPEN) {
            chunkCount++;
            console.log(`Sending chunk ${chunkCount}, size: ${chunk.length}`);
            ws.send(chunk, { binary: true });
        }
    });

    audioStream.on('end', () => {
        console.log(`Finished streaming audio file. Total chunks sent: ${chunkCount}`);
    });

    audioStream.on('error', (error) => {
        console.error('Error reading audio file:', error);
    });
}


console.log(`Servidor WebSocket iniciado en el puerto ${WS_PORT}`);

