#include <Wire.h>
#include <math.h>

// -----------------------------------------------------------------------------
// 1. HARDWARE Y COMUNICACIÓN
// -----------------------------------------------------------------------------
#define MIC_PIN 36        
#define I2C_SDA 21        
#define I2C_SCL 22
#define COMM_TX_PIN 17    
#define COMM_RX_PIN 16    
#define BAUD_RATE 115200 

// -----------------------------------------------------------------------------
// 2. VARIABLES AUDIO (Mantenemos tu lógica de picos)
// -----------------------------------------------------------------------------
unsigned long lastMicTime = 0;
const int MIC_DELAY_MICROS = 125;  
int micZero = 0;
int micThresholdVal = 0; 
bool systemArmed = false;

// -----------------------------------------------------------------------------
// 3. DEFINICIONES IMU (ADAPTADAS DE TU EJEMPLO)
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

// Umbrales (Copiados del ejemplo)
const float GRAVITY = 1.0f;
const float ACCELERATION_THRESHOLD = 2.0f * GRAVITY; // 2.0 Gs
const float ORIENTATION_THRESHOLD = 30.0f;           // 30 Grados

// Variables de estado previo (Heading, Roll, Pitch)
static float prev_heading = 0.0f;
static float prev_roll = 0.0f;
static float prev_pitch = 0.0f;

unsigned long previousMillisIMU = 0;
const long intervalIMU = 500; // Revisar cada 500ms (como en el ejemplo: k_msleep(500))

// -----------------------------------------------------------------------------
// 4. FUNCIONES DE BAJO NIVEL (I2C)
// -----------------------------------------------------------------------------
// Escritura de un byte en registro
void i2c_reg_write_byte(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(BNO055_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// Lectura de un byte de registro
int i2c_reg_read_byte(uint8_t reg, uint8_t *value) {
  Wire.beginTransmission(BNO055_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  
  if (Wire.requestFrom(BNO055_ADDR, 1) == 1) {
    *value = Wire.read();
    return 0; // Success
  }
  return -1; // Error
}

// Lectura de 16 bits (LSB + MSB)
int16_t read_sensor_16bit(uint8_t reg_lsb) {
  uint8_t lsb, msb;
  if (i2c_reg_read_byte(reg_lsb, &lsb) == 0 &&
      i2c_reg_read_byte(reg_lsb + 1, &msb) == 0) {
      return (int16_t)((msb << 8) | lsb);
  }
  return 0;
}

// -----------------------------------------------------------------------------
// 5. LÓGICA MATEMÁTICA IMU (DEL EJEMPLO)
// -----------------------------------------------------------------------------

// Función para inicializar correctamente el BNO055
bool initialize_bno055() {
    uint8_t chip_id;
    int attempts = 0;
    
    Serial.println("Initializing BNO055...");
    
    // 1. Verificar Chip ID
    while (attempts < 10) {
        if (i2c_reg_read_byte(BNO055_CHIP_ID_REG, &chip_id) == 0 && chip_id == 0xA0) {
            Serial.printf("✅ BNO055 ready after %d attempts\n", attempts);
            break;
        }
        attempts++;
        delay(100);
    }
    
    if (attempts >= 10) {
        Serial.println("❌ BNO055 did not respond (Check wiring/ADDR)");
        return false;
    }
    
    // 2. Reset del sensor
    Serial.println("Resetting BNO055...");
    i2c_reg_write_byte(BNO055_SYS_TRIGGER_REG, 0x20); // Bit 5 = RST_SYS
    delay(700); // Necesita ~650ms tras reset
    
    // 3. Set Power Mode Normal
    i2c_reg_write_byte(BNO055_PWR_MODE_REG, 0x00);
    delay(50);
    
    // 4. Set Operation Mode NDOF
    i2c_reg_write_byte(BNO055_OPR_MODE_REG, BNO055_OPR_MODE_NDOF);
    delay(50);
    
    Serial.println("✅ BNO055 initialized in NDOF mode");
    return true;
}

// Función matemática para diferencia angular (0-360)
float angular_diff(float a, float b) {
    float diff = fmod(fabs(a - b), 360.0);
    if (diff > 180.0)
        diff = 360.0 - diff;
    return diff;
}

// Lógica principal de detección de eventos IMU
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
    float accel_x_g = accel_x / 100.0f; // Ajustado a 100.0f según tus pruebas (o 981.0f si usas el driver original)
    float accel_y_g = accel_y / 100.0f;
    float accel_z_g = accel_z / 100.0f;
    
    float heading_deg = heading / 16.0f;
    float roll_deg    = roll    / 16.0f;
    float pitch_deg   = pitch   / 16.0f;
    
    // --- DETECCIÓN DE EVENTOS ---
    
    // 1. Magnitud total de aceleración (restamos gravedad si es necesario, pero para golpes brutos sqrt vale)
    // Nota: Si usas el divisor 100.0f, asegúrate de que la magnitud en reposo sea ~10 (1G) o ~1 (si usas 981).
    // Asumiré divisor 100.0f -> 1G = 9.8 aprox. Ajusta ACCELERATION_THRESHOLD acorde.
    // Si usas el código anterior divisor 981.0f -> 1G = 1.0.
    // Volviendo a tu estándar anterior seguro:
    float total_accel_val = sqrt(pow(accel_x/981.0f, 2) + pow(accel_y/981.0f, 2) + pow(accel_z/981.0f, 2));

    // 2. Diferencias angulares
    float heading_diff = angular_diff(heading_deg, prev_heading);
    float roll_diff    = angular_diff(roll_deg, prev_roll);
    float pitch_diff   = angular_diff(pitch_deg, prev_pitch);
    
    // --- LOGICA DE DISPARO ---
    
    if (fabs(total_accel_val) > ACCELERATION_THRESHOLD) {
        triggerAlarm("IMU_ACCEL", (int)(total_accel_val * 100));
        return; // <--- ¡VITAL! Salimos aquí para no sobreescribir la recalibración
    } 
    else if (heading_diff > ORIENTATION_THRESHOLD ||
             roll_diff    > ORIENTATION_THRESHOLD ||
             pitch_diff   > ORIENTATION_THRESHOLD) {
        
        float max_diff = fmax(heading_diff, fmax(roll_diff, pitch_diff));
        triggerAlarm("IMU_ROTATION", (int)max_diff);
        return; // <--- ¡VITAL! Salimos aquí para no sobreescribir la recalibración
    }

    // Actualizar referencias NORMALES (solo si no hubo alarma)
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
  
  Serial.println("\n=== INICIANDO VIGILANTE (ZEPHYR LOGIC PORT) ===");

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
      Serial.println("⚠️ Fallo crítico en IMU. Revisa cables.");
  } else {
      // Lectura inicial para establecer línea base (igual que tu ejemplo)
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

  // B. IMU CHECK (Lógica portada)
  check_imu_events();
}

// -----------------------------------------------------------------------------
// 8. DISPARADOR ALARMA
// -----------------------------------------------------------------------------
void triggerAlarm(String source, int value) {
  systemArmed = false;
  
  String msg = "ALARM:" + source + ":" + String(value);
  Serial.println("\n🚨 " + msg);
  Serial2.println(msg); // Enviar a Wrover
  
  Serial.println("⏳ Cooldown 5s...");
  delay(5000);
  
  // Recalibrar referencias de IMU tras el evento (para que no salte inmediatamente)
  prev_heading = read_sensor_16bit(BNO055_EUL_HEADING_LSB) / 16.0f;
  prev_roll    = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 2) / 16.0f;
  prev_pitch   = read_sensor_16bit(BNO055_EUL_HEADING_LSB + 4) / 16.0f;
  
  Serial.println("🛡️ Rearmado.");
  systemArmed = true;
}