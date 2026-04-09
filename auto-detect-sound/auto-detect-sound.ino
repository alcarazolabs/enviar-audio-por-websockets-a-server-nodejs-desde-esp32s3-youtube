/*
* Probado con ESP32 Boards lib versión: 3.0.6
* Auto-recording con detección de sonido por RMS

* Valores a tener en cuenta para ajustar la variable THRESHOLD_RMS:

- 100-200: Muy sensible (capta susurros)
- 300-500: Sensibilidad media (voz normal) ← Actual (300)
- 600-1000: Menos sensible (solo voces fuertes)

*/
#include <driver/i2s.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h> // By Gil Maimon v.0.5.4
#include <Adafruit_NeoPixel.h>

//Nota: Aqui se usan pines compartidos entre el microfono INMP441 y el MAX98357A (ver readme.md para conexiones)

// Shared clock pins
#define I2S_BCK         (GPIO_NUM_5)
#define I2S_WS          (GPIO_NUM_6)

// Separate data pins
#define I2S_SPK_DATA    (GPIO_NUM_7)
#define I2S_MIC_DATA    (GPIO_NUM_17) // SD

#define RGB_BUILTIN 48
#define NUM_PIXELS 1

#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE     44100
#define bufferLen       1024
#define bufferCnt       10

// Configuración de detección de audio
#define THRESHOLD_RMS       300      // Umbral de detección de sonido
#define SILENCE_TIMEOUT     3000     // 3 segundos de silencio en milisegundos
#define RMS_SAMPLE_SIZE     512      // Tamaño de muestra para calcular RMS

//Amplifacion de Sonido:
#define GAIN 7  // Ajusta este valor para mayor/menor volumen (1-10 recomendado). Nota 10 la mayoria de las veces crashea la reproduccion.

// Global variables
int16_t sBuffer[bufferLen];
bool isRecording = false;
bool isPlaying = false;
bool isWebSocketConnected = false;

// Variables para detección automática de audio
unsigned long lastSoundDetectedTime = 0;
bool soundDetected = false;

Adafruit_NeoPixel pixels(NUM_PIXELS, RGB_BUILTIN, NEO_GRB + NEO_KHZ800);

const char* ssid = "ROSA";
const char* password = "942900777";
const char* websocket_server_host = "192.168.18.80";
const uint16_t websocket_server_port = 8888;

using namespace websockets;
WebsocketsClient client;

// Función para calcular RMS | Calculate Root Mean Square (RMS) value from audio samples (used to detect loudness)
int calculateRMS(int16_t* samples, int count) {
    long sum = 0;
    for (int i = 0; i < count; i++) {
        sum += samples[i] * samples[i];
    }
    return sqrt((float)sum / count);
}

void updateLED() {
    if (isRecording) {
        pixels.setPixelColor(0, pixels.Color(255, 0, 0));  // Red when recording
    } else if (isPlaying) {
        pixels.setPixelColor(0, pixels.Color(0, 255, 0));  // Green when playing
    } else {
        pixels.setPixelColor(0, pixels.Color(0, 0, 255));  // Blue when idle
    }
    pixels.show();
}

void i2s_install() {
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 10,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    
    esp_err_t result = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (result != ESP_OK) {
        Serial.printf("Error installing I2S driver: %d\n", result);
        while(1);
    }
}

void i2s_setpin() {
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK,         // GPIO 5
        .ws_io_num = I2S_WS,           // GPIO 6
        .data_out_num = I2S_SPK_DATA,  // GPIO 7 (Speaker output)
        .data_in_num = I2S_MIC_DATA    // GPIO 17 (Microphone input)
    };
    
    esp_err_t result = i2s_set_pin(I2S_PORT, &pin_config);
    if (result != ESP_OK) {
        Serial.printf("Error setting I2S pins: %d\n", result);
        while(1);
    }
}

void onEventsCallback(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connection Opened");
        isWebSocketConnected = true;
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection Closed");
        isWebSocketConnected = false;
    }
}
/*
void onAudioData(WebsocketsMessage message) {
    if (message.isBinary()) {
        Serial.println("Received audio data chunk");
        isPlaying = true;
        updateLED();
        
        const uint8_t* audio_data = (const uint8_t*)message.c_str();
        size_t data_length = message.length();
        
        Serial.printf("Audio chunk size: %d bytes\n", data_length);
        
        // Write data in smaller chunks
        size_t bytes_written = 0;
        size_t offset = 0;
        while (offset < data_length) {
            size_t chunk_size = std::min(size_t(64), data_length - offset);
            uint8_t* chunk = (uint8_t*)audio_data + offset;
            
            esp_err_t result = i2s_write(I2S_PORT, chunk, chunk_size, &bytes_written, portMAX_DELAY);
            if (result != ESP_OK) {
                Serial.printf("Error during I2S write: %d\n", result);
                break;
            }
            offset += bytes_written;
        }
        
        Serial.printf("Successfully wrote %d bytes to I2S\n", offset);
        isPlaying = false;
        updateLED();
    }
}

*/

void onAudioData(WebsocketsMessage message) {
    if (message.isBinary()) {
        Serial.println("Received audio data chunk");
        isPlaying = true;
        updateLED();
        
        const uint8_t* audio_data = (const uint8_t*)message.c_str();
        size_t data_length = message.length();
        
        Serial.printf("Audio chunk size: %d bytes\n", data_length);
        
        // Crear buffer temporal para aplicar ganancia
        size_t num_samples = data_length / 2; // Cada muestra es int16_t (2 bytes)
        int16_t* amplified_buffer = (int16_t*)malloc(data_length);
        
        if (amplified_buffer == NULL) {
            Serial.println("Error: No se pudo asignar memoria para el buffer");
            isPlaying = false;
            updateLED();
            return;
        }
        
        // Copiar datos originales al buffer
        memcpy(amplified_buffer, audio_data, data_length);
        
        // Aplicar ganancia con clamp para evitar overflow
        for (size_t i = 0; i < num_samples; i++) {
            int32_t amplified = (int32_t)amplified_buffer[i] * GAIN;
            // Clamp al rango de int16_t
            if (amplified > 32767)  amplified = 32767;
            if (amplified < -32768) amplified = -32768;
            amplified_buffer[i] = (int16_t)amplified;
        }
        
        // Escribir buffer amplificado en chunks
        size_t bytes_written = 0;
        size_t offset = 0;
        uint8_t* output = (uint8_t*)amplified_buffer;
        
        while (offset < data_length) {
            size_t chunk_size = std::min(size_t(64), data_length - offset);
            
            esp_err_t result = i2s_write(I2S_PORT, output + offset, chunk_size, &bytes_written, portMAX_DELAY);
            if (result != ESP_OK) {
                Serial.printf("Error durante I2S write: %d\n", result);
                break;
            }
            offset += bytes_written;
        }
        
        free(amplified_buffer); // Liberar memoria
        
        Serial.printf("Successfully wrote %d bytes to I2S\n", offset);
        isPlaying = false;
        updateLED();

    }
}


void setup() {
    Serial.begin(115200);
    
    // Initialize LED
    pixels.begin();
    pixels.setBrightness(50);
    
    // Connect WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    
    // Setup I2S
    i2s_install();
    i2s_setpin();
    i2s_start(I2S_PORT);
    
    // Setup WebSocket
    client.onEvent(onEventsCallback);
    client.onMessage(onAudioData);
    
    // Connect WebSocket
    while (!client.connect(websocket_server_host, websocket_server_port, "/")) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("Websocket Connected!");
    
    // Create microphone task
    xTaskCreatePinnedToCore(micTask, "micTask", 10000, NULL, 1, NULL, 1);
    
    updateLED();
    Serial.println("Sistema listo. Esperando detección de audio...");
}

void loop() {
    if (client.available()) {
        client.poll();
    } else if (!isWebSocketConnected) {
        Serial.println("Reconnecting to WebSocket...");
        client.connect(websocket_server_host, websocket_server_port, "/");
        delay(1000);
    }
    
    // Detección automática de audio mediante timeout de silencio
    if (isRecording && soundDetected) {
        unsigned long currentTime = millis();
        
        // Verificar si ha pasado el tiempo de silencio
        if (currentTime - lastSoundDetectedTime >= SILENCE_TIMEOUT) {
            isRecording = false;
            soundDetected = false;
            updateLED();
            Serial.println("Silencio detectado. Deteniendo grabación.");
        }
    }
    
    delay(10);
}

void micTask(void* parameter) {
    size_t bytesIn = 0;
    int16_t samples[1024];
    
    while (1) {
        if (isWebSocketConnected) {
            // Siempre leer muestras del micrófono para detectar audio
            esp_err_t result = i2s_read(I2S_PORT, &samples, sizeof(samples), &bytesIn, portMAX_DELAY);
            
            if (result == ESP_OK && bytesIn > 0) {
                int samplesRead = bytesIn / sizeof(int16_t);
                
                // Calcular RMS para detectar nivel de sonido
                int rmsValue = calculateRMS(samples, samplesRead);
                
                // Detección de sonido
                if (rmsValue > THRESHOLD_RMS) {
                    lastSoundDetectedTime = millis();
                    
                    if (!isRecording) {
                        // Iniciar grabación
                        isRecording = true;
                        soundDetected = true;
                        updateLED();
                        Serial.printf("Sonido detectado (RMS: %d). Iniciando grabación...\n", rmsValue);
                    }
                }
                
                // Si estamos grabando, enviar el audio
                if (isRecording) {
                    client.sendBinary((const char*)samples, bytesIn);
                }
            } else {
                Serial.printf("Error reading from I2S: %d\n", result);
            }
        }
        vTaskDelay(1);
    }
}
