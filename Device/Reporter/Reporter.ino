#include "esp_camera.h"
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

// --- CONFIGURACIÓN ---
#define BT_NAME "ESP32_BLACKBOX"
#define RX_PIN 14       
#define TX_PIN 0        
#define BAUD_RATE 115200 

// PINES CÁMARA (Freenove Wrover)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

bool refPhotoSent = false; 

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- INICIANDO REPORTERO SIN LAG ---");

  // 1. CÁMARA
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA; 
  config.jpeg_quality = 12; 
  config.fb_count = 1; // Aunque sea 1, a veces guarda "basura" vieja
  
  if(psramFound()){
    config.fb_location = CAMERA_FB_IN_PSRAM;
  }
  
  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Error Cámara"); while(1);
  }

  // 2. COMUNICACIÓN
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);

  // 3. BLUETOOTH
  if (SerialBT.begin(BT_NAME)) {
    Serial.println("✅ Bluetooth Listo.");
  } else {
    Serial.println("❌ Fallo Bluetooth."); while(1);
  }
}

// Función auxiliar para captura y envío
void sendPhotoBT(String typeLabel) {
  
  // --- TRUCO ANTI-LAG: VACIAR BUFFER ---
  // Tomamos una foto "dummy" y la liberamos al instante.
  // Esto obliga al sensor a capturar un frame nuevo real.
  camera_fb_t * fb = esp_camera_fb_get();
  esp_camera_fb_return(fb); // Saca del búffer el frame de la recámara
  // -------------------------------------

  // AHORA SÍ: FOTO REAL
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("❌ Fallo captura foto");
    return;
  }
  
  Serial.println("📸 Enviando foto REAL: " + typeLabel);
  
  if (SerialBT.hasClient()) {
     SerialBT.println("=== INCIDENT REPORT ===");
     SerialBT.println("TYPE: " + typeLabel);
     SerialBT.println("--- FOTO START ---");
     SerialBT.write(fb->buf, fb->len);
     SerialBT.println("\n--- FOTO END ---");
     SerialBT.println("=== END REPORT ===");
  }
  
  esp_camera_fb_return(fb);
}

void loop() {
  // A. FOTO INICIAL
  if (SerialBT.hasClient() && !refPhotoSent) {
      delay(1000); 
      sendPhotoBT("INITIAL_REFERENCE");
      refPhotoSent = true;
      Serial.println("✅ Referencia enviada.");
  }
  if (!SerialBT.hasClient()) {
      refPhotoSent = false;
  }

  // B. ALARMAS
  if (Serial2.available()) {
    String cmd = Serial2.readStringUntil('\n');
    cmd.trim();
    
    if (cmd.startsWith("ALARM")) {
      Serial.println("\n🚨 ALARMA: " + cmd);
      
      if (SerialBT.hasClient()) {
          sendPhotoBT(cmd); 
      } else {
          Serial.println("⚠️ PC No conectado.");
      }
      
      while(Serial2.available()) Serial2.read(); 
    }
  }
}