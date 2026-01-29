#include <Wire.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h> // Necesario para evitar el error 0x03

// -----------------------------------------------------------------------------
// 1. HARDWARE Y COMUNICACIÓN
// -----------------------------------------------------------------------------
#define MIC_PIN 36        
#define I2C_SDA 21        
#define I2C_SCL 22
#define COMM_TX_PIN 17    
#define COMM_RX_PIN 16    
#define BAUD_RATE 115200 

// CONFIGURACIÓN BLE
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define TELEMETRY_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define HEARTBEAT_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a9"

// MODOS DE TELEMETRÍA
// 0: IDLE (Apagado), 2: STREAM (Continuo, cuando el Usuario pide ver telemetría en tiempo real)
volatile uint8_t telemetryMode = 0; 

// Un servicio --> dos características: Telemetría + Heartbeat (indica que el dispositivo está vivo periódicamente)
BLEServer* pServer = NULL;
BLECharacteristic* pTelemetryCharacteristic = NULL; // Telemetría completa
BLECharacteristic* pHeartbeatCharacteristic = NULL; // Heartbeat periódico
bool deviceConnected = false;


// This callback is used so the Watchdog can know when extactly the Edge asked for telemetry. It is associated with the Telemtry characteristic.
class MyTelemetryCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String value = pCharacteristic->getValue();
        if (value.length() > 0) {
            telemetryMode = (uint8_t)value[0];
            Serial.print("📡 Cambio de modo Telemetría: ");
            Serial.println(telemetryMode);
        }
    }
};

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
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

// Timer Heartbeat
unsigned long lastHeartbeatTime = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000; // 30 segundos

// Timer Cooldown Alarma (No bloqueante)
unsigned long lastAlarmTime = 0;
const unsigned long ALARM_COOLDOWN = 5000; // 5 segundos de espera tras alarma

// Variable global para compartir el audio con el BLE
volatile int liveAudioLevel = 0; 

// -----------------------------------------------------------------------------
// 3. DEFINICIONES IMU
// -----------------------------------------------------------------------------
#define BNO055_ADDR 0x29 
#define BNO055_CHIP_ID_REG     0x00 
#define BNO055_ACC_DATA_X_LSB  0x08 
#define BNO055_EUL_HEADING_LSB 0x1A 
#define BNO055_OPR_MODE_REG    0x3D 
#define BNO055_PWR_MODE_REG    0x3E 
#define BNO055_SYS_TRIGGER_REG 0x3F 
#define BNO055_OPR_MODE_NDOF   0x0C 

const float GRAVITY = 1.0f;
const float ACCELERATION_THRESHOLD = 2.0f * GRAVITY; 
const float ORIENTATION_THRESHOLD = 30.0f;           

static float prev_heading = 0.0f;
static float prev_roll = 0.0f;
static float prev_pitch = 0.0f;

unsigned long previousMillisIMU = 0;
const long intervalIMU = 200; 

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
// 5. LÓGICA MATEMÁTICA IMU + TELEMETRÍA
// -----------------------------------------------------------------------------
bool initialize_bno055() {
    uint8_t chip_id;
    int attempts = 0;
    Serial.println("Initializing BNO055...");
    while (attempts < 10) {
        if (i2c_reg_read_byte(BNO055_CHIP_ID_REG, &chip_id) == 0 && chip_id == 0xA0) break;
        attempts++; delay(100);
    }
    if (attempts >= 10) return false;
    i2c_reg_write_byte(BNO055_SYS_TRIGGER_REG, 0x20); delay(700); 
    i2c_reg_write_byte(BNO055_PWR_MODE_REG, 0x00); delay(50);
    i2c_reg_write_byte(BNO055_OPR_MODE_REG, BNO055_OPR_MODE_NDOF); delay(50);
    Serial.println("✅ BNO055 initialized in NDOF mode");
    return true;
}

float angular_diff(float a, float b) {
    float diff = fmod(fabs(a - b), 360.0);
    if (diff > 180.0) diff = 360.0 - diff;
    return diff;
}

void triggerAlarm(String source, int value);

// Función auxiliar para enviar datos BLE (Reutilizable)
void sendBLETelemetry(float g_val, float h, float p, float r) {
    if (deviceConnected) {
        char statusStr[100]; 
        snprintf(statusStr, sizeof(statusStr), 
                 "Au:%d | G:%.2f | H:%.0f P:%.0f R:%.0f", 
                 liveAudioLevel, g_val, h, p, r);
        
        pTelemetryCharacteristic->setValue(statusStr);
        pTelemetryCharacteristic->notify();
    }
}

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
    
    float total_accel_val = sqrt(pow(accel_x/981.0f, 2) + pow(accel_y/981.0f, 2) + pow(accel_z/981.0f, 2));

    // --- TELEMETRÍA BLE CONTINUA (STREAM - Modo 2) ---
    if (telemetryMode == 2) {
        sendBLETelemetry(total_accel_val, heading_deg, pitch_deg, roll_deg);
    }
    // ------------------------------

    // Lógica Alarmas (SOLO SI NO ESTAMOS EN COOLDOWN)
    if (millis() - lastAlarmTime > ALARM_COOLDOWN) {
        float heading_diff = angular_diff(heading_deg, prev_heading);
        float roll_diff    = angular_diff(roll_deg, prev_roll);
        float pitch_diff   = angular_diff(pitch_deg, prev_pitch);
        
        if (fabs(total_accel_val) > ACCELERATION_THRESHOLD) {
            triggerAlarm("IMU_ACCEL", (int)(total_accel_val * 100));
            // [PROACTIVO] Enviamos telemetría BLE justo en el momento del impacto
            sendBLETelemetry(total_accel_val, heading_deg, pitch_deg, roll_deg); 

            // Actualizamos referencias
            prev_heading = heading_deg; prev_roll = roll_deg; prev_pitch = pitch_deg;
            return; 
        } 
        else if (heading_diff > ORIENTATION_THRESHOLD ||
                 roll_diff    > ORIENTATION_THRESHOLD ||
                 pitch_diff   > ORIENTATION_THRESHOLD) {
            float max_diff = fmax(heading_diff, fmax(roll_diff, pitch_diff));
            triggerAlarm("IMU_ROTATION", (int)max_diff);
            // [PROACTIVO] Enviamos telemetría BLE justo en el momento del giro brusco
            sendBLETelemetry(total_accel_val, heading_deg, pitch_deg, roll_deg);

            prev_heading = heading_deg; prev_roll = roll_deg; prev_pitch = pitch_deg;
            return; 
        }
    }

    prev_heading = heading_deg; prev_roll = roll_deg; prev_pitch = pitch_deg;
  }
}

// -----------------------------------------------------------------------------
// 6. SETUP PRINCIPAL
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(BAUD_RATE); 
  Serial2.begin(BAUD_RATE, SERIAL_8N1, COMM_RX_PIN, COMM_TX_PIN);
  
  Serial.println("\n=== INICIANDO VIGILANTE + BLE FULL DATA ===");

  // BLE SERVER
  BLEDevice::init("WATCHDOG");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // 1. TELEMETRÍA (Datos rápidos)
  pTelemetryCharacteristic = pService->createCharacteristic(
                      TELEMETRY_CHAR_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_NOTIFY |
                      BLECharacteristic::PROPERTY_WRITE  // El Edge podrá escribir en el canal BLE para solicitar telemetría
                    );
  pTelemetryCharacteristic->setCallbacks(new MyTelemetryCallbacks());
  pTelemetryCharacteristic->addDescriptor(new BLE2902()); 
  pTelemetryCharacteristic->setValue("Esperando comandos...");

  // 2. HEARTBEAT (Estado cada X segundos)
  pHeartbeatCharacteristic = pService->createCharacteristic(
                      HEARTBEAT_CHAR_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pHeartbeatCharacteristic->addDescriptor(new BLE2902());
  pHeartbeatCharacteristic->setValue("Iniciando heartbeat...");

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  
  BLEDevice::startAdvertising();

  // AUDIO
  pinMode(MIC_PIN, INPUT);
  Serial.println("⏳ Calibrando micro...");
  long sum = 0; 
  for(int i=0; i<5000; i++) { sum += analogRead(MIC_PIN); delayMicroseconds(MIC_DELAY_MICROS); }
  micZero = sum / 5000;
  if (micZero < 100) micZero = 100;
  micThresholdVal = (int)(micZero * 0.6); 
  
  // IMU
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
    
    // Guardamos el valor actual en la variable global para el BLE
    liveAudioLevel = raw; 

    // Solo verificamos umbrales si NO estamos en cooldown
    if (millis() - lastAlarmTime > ALARM_COOLDOWN) {
        int wave = raw - micZero; 
        if (wave > micThresholdVal) {
           triggerAlarm("AUDIO", raw);
        }
    }
  }

  // B. IMU + BLE UPDATE
  check_imu_events();

  // C. HEARTBEAT (30s)
  if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = millis();
    if (deviceConnected) {
        pHeartbeatCharacteristic->setValue("OK");
        pHeartbeatCharacteristic->notify();
    }
  }
}

// -----------------------------------------------------------------------------
// 8. DISPARADOR ALARMA (NO BLOQUEANTE)
// -----------------------------------------------------------------------------
void triggerAlarm(String source, int value) {
  // Marcamos el tiempo actual como inicio del cooldown
  lastAlarmTime = millis();

  String msg = "ALARM:" + source + ":" + String(value);
  Serial.println("\n🚨 " + msg);
  Serial2.println(msg); 
}