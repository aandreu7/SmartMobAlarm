#include <Wire.h>
#include <math.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// -----------------------------------------------------------------------------
// 1. HARDWARE Y COMUNICACIÓN
// -----------------------------------------------------------------------------
#define MIC_PIN 36        
#define I2C_SDA 21        
#define I2C_SCL 22
#define COMM_TX_PIN 17    
#define COMM_RX_PIN 16    
#define BAUD_RATE 115200 

// BLE Configuration
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;

// Callback to handle Bluetooth Client connections
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      // Reboots publicity (in case of broken connection) in order to allow clients to connect again
      pServer->getAdvertising()->start();
    }
};

// -----------------------------------------------------------------------------
// 2. VARIABLES AUDIO
// -----------------------------------------------------------------------------
unsigned long lastMicTime = 0;
const int MIC_DELAY_MICROS = 125;  
int micZero = 0;
int micThresholdVal = 0; 
bool systemArmed = false;

// -----------------------------------------------------------------------------
// 3. DEFINICIONES IMU
// -----------------------------------------------------------------------------
#define BNO055_ADDR 0x29 

// Registros BNO055
#define BNO055_CHIP_ID_REG     0x00 
#define BNO055_ACC_DATA_X_LSB  0x08 
#define BNO055_EUL_HEADING_LSB 0x1A 
#define BNO055_OPR_MODE_REG    0x3D 
#define BNO055_PWR_MODE_REG    0x3E 
#define BNO055_SYS_TRIGGER_REG 0x3F 

// Modos de Operación
#define BNO055_OPR_MODE_CONFIG 0x00 
#define BNO055_OPR_MODE_NDOF   0x0C 

// Umbrales
const float GRAVITY = 1.0f;
const float ACCELERATION_THRESHOLD = 2.0f * GRAVITY; 
const float ORIENTATION_THRESHOLD = 30.0f;           

// Variables de estado previo
static float prev_heading = 0.0f;
static float prev_roll = 0.0f;
static float prev_pitch = 0.0f;

unsigned long previousMillisIMU = 0;
const long intervalIMU = 300; 

// -----------------------------------------------------------------------------
// 4. FUNCIONES DE BAJO NIVEL (I2C)
// -----------------------------------------------------------------------------
void i2c_reg_write_byte(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(BNO055_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

int i2c_reg_read_byte(uint8_t reg, uint8_t *value) {
  Wire.beginTransmission(BNO055_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  
  if (Wire.requestFrom(BNO055_ADDR, 1) == 1) {
    *value = Wire.read();
    return 0; 
  }
  return -1; 
}

int16_t read_sensor_16bit(uint8_t reg_lsb) {
  uint8_t lsb, msb;
  if (i2c_reg_read_byte(reg_lsb, &lsb) == 0 &&
      i2c_reg_read_byte(reg_lsb + 1, &msb) == 0) {
      return (int16_t)((msb << 8) | lsb);
  }
  return 0;
}

// -----------------------------------------------------------------------------
// 5. LÓGICA MATEMÁTICA IMU
// -----------------------------------------------------------------------------

bool initialize_bno055() {
    uint8_t chip_id;
    int attempts = 0;
    
    Serial.println("Initializing BNO055...");
    while (attempts < 10) {
        if (i2c_reg_read_byte(BNO055_CHIP_ID_REG, &chip_id) == 0 && chip_id == 0xA0) {
            Serial.printf("✅ BNO055 ready after %d attempts\n", attempts);
            break;
        }
        attempts++;
        delay(100);
    }
    
    if (attempts >= 10) {
        Serial.println("❌ BNO055 did not respond");
        return false;
    }
    
    Serial.println("Resetting BNO055...");
    i2c_reg_write_byte(BNO055_SYS_TRIGGER_REG, 0x20); 
    delay(700); 
    i2c_reg_write_byte(BNO055_PWR_MODE_REG, 0x00);
    delay(50);
    i2c_reg_write_byte(BNO055_OPR_MODE_REG, BNO055_OPR_MODE_NDOF);
    delay(50);
    
    Serial.println("✅ BNO055 initialized in NDOF mode");
    return true;
}

float angular_diff(float a, float b) {
    float diff = fmod(fabs(a - b), 360.0);
    if (diff > 180.0)
        diff = 360.0 - diff;
    return diff;
}

// Forward declaration
void triggerAlarm(String source, int value);

void check_imu_events() {
  if (millis() - previousMillisIMU >= intervalIMU) {
    previousMillisIMU = millis();
    
    // --- LECTURA DE SENSORES ---
    int16_t accel_x = read_sensor_16bit(BNO055_ACC_DATA_X_LSB);
    int16_t accel_y = read_sensor_16bit(BNO055_ACC_DATA_X_LSB + 2);
    int16_t accel_z = read_sensor_16bit(BNO055_ACC_DATA_X_LSB + 4);
    
    int16_t heading = read_sensor_16bit(BNO055_EUL_HEADING_LSB);
    int16_t roll    = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 2);
    int16_t pitch   = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 4);
    
    // Conversiones 
    float accel_x_g = accel_x / 100.0f; 
    float accel_y_g = accel_y / 100.0f;
    float accel_z_g = accel_z / 100.0f;
    
    float heading_deg = heading / 16.0f;
    float roll_deg    = roll    / 16.0f;
    float pitch_deg   = pitch   / 16.0f;
    
    // Cálculo magnitud
    float total_accel_val = sqrt(pow(accel_x/981.0f, 2) + pow(accel_y/981.0f, 2) + pow(accel_z/981.0f, 2));

    // If there is a connected Bluetooth Client device, data is sent in real time
    if (deviceConnected) {
        char statusStr[50];
        // Format: "G: 1.05 | H: 120.5" (Gravity & Heading)
        snprintf(statusStr, sizeof(statusStr), "G: %.2f | H: %.1f", total_accel_val, heading_deg);
        
        pCharacteristic->setValue(statusStr);
        pCharacteristic->notify();
    }
    // ------------------------------

    // Diferencias angulares
    float heading_diff = angular_diff(heading_deg, prev_heading);
    float roll_diff    = angular_diff(roll_deg, prev_roll);
    float pitch_diff   = angular_diff(pitch_deg, prev_pitch);
    
    // --- LOGICA DE DISPARO ---
    if (fabs(total_accel_val) > ACCELERATION_THRESHOLD) {
        triggerAlarm("IMU_ACCEL", (int)(total_accel_val * 100));
        return; 
    } 
    else if (heading_diff > ORIENTATION_THRESHOLD ||
             roll_diff    > ORIENTATION_THRESHOLD ||
             pitch_diff   > ORIENTATION_THRESHOLD) {
        
        float max_diff = fmax(heading_diff, fmax(roll_diff, pitch_diff));
        triggerAlarm("IMU_ROTATION", (int)max_diff);
        return; 
    }

    // Actualizar referencias
    prev_heading = heading_deg;
    prev_roll    = roll_deg;
    prev_pitch   = pitch_deg;
  }
}

// -----------------------------------------------------------------------------
// 6. SETUP PRINCIPAL
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(BAUD_RATE); 
  Serial2.begin(BAUD_RATE, SERIAL_8N1, COMM_RX_PIN, COMM_TX_PIN);
  
  Serial.println("\n=== INICIANDO VIGILANTE + BLE ===");

  // Initiates BLE Server
  BLEDevice::init("ESP32_WATCHDOG");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  
  pCharacteristic->setValue("Initializing...");
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  
  BLEDevice::startAdvertising();
  Serial.println("✅ BLE Active. Waiting for BLE Client connection...");


  // --- AUDIO SETUP ---
  pinMode(MIC_PIN, INPUT);
  Serial.println("⏳ Calibrando micro...");
  long sum = 0; 
  for(int i=0; i<5000; i++) { sum += analogRead(MIC_PIN); delayMicroseconds(MIC_DELAY_MICROS); }
  micZero = sum / 5000;
  if (micZero < 100) micZero = 100;
  micThresholdVal = (int)(micZero * 0.6); 
  Serial.printf("✅ MicZero: %d | Threshold: %d\n", micZero, micThresholdVal);
  
  // --- IMU SETUP (I2C) ---
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!initialize_bno055()) {
      Serial.println("⚠️ Fallo crítico en IMU.");
  } else {
      prev_heading = read_sensor_16bit(BNO055_EUL_HEADING_LSB) / 16.0f;
      prev_roll    = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 2) / 16.0f;
      prev_pitch   = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 4) / 16.0f;
      delay(100);
  }
  
  systemArmed = true;
  Serial.println("🛡️ SISTEMA ARMADO.");
}

// -----------------------------------------------------------------------------
// 7. LOOP PRINCIPAL
// -----------------------------------------------------------------------------
void loop() {
  // A. AUDIO CHECK
  if (micros() - lastMicTime >= MIC_DELAY_MICROS) {
    lastMicTime = micros();
    int raw = analogRead(MIC_PIN);
    int wave = raw - micZero; 
    
    if (wave > micThresholdVal) {
       triggerAlarm("AUDIO", raw);
    }
  }

  // B. IMU CHECK (+ BLE Update)
  check_imu_events();
}

// -----------------------------------------------------------------------------
// 8. DISPARADOR ALARMA
// -----------------------------------------------------------------------------
void triggerAlarm(String source, int value) {
  systemArmed = false;
  
  // Notifies alarm to BLE connected devices
  if (deviceConnected) {
      String bleMsg = "ALERTA: " + source;
      pCharacteristic->setValue(bleMsg.c_str());
      pCharacteristic->notify();
  }

  String msg = "ALARM:" + source + ":" + String(value);
  Serial.println("\n🚨 " + msg);
  Serial2.println(msg); // Enviar a Wrover
  
  Serial.println("⏳ Cooldown 5s...");
  delay(5000);
  
  // Recalibrar referencias
  prev_heading = read_sensor_16bit(BNO055_EUL_HEADING_LSB) / 16.0f;
  prev_roll    = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 2) / 16.0f;
  prev_pitch   = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 4) / 16.0f;
  
  // Notifies system rearming to BLE connected devices
  if (deviceConnected) {
      pCharacteristic->setValue("SISTEMA REARMADO");
      pCharacteristic->notify();
  }

  Serial.println("🛡️ Rearmado.");
  systemArmed = true;
}