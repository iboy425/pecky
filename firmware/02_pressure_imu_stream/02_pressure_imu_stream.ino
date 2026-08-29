#include <Arduino.h>
#include <Wire.h>
#include <math.h>

namespace {
constexpr uint8_t kMpuAddress = 0x68;
constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;
constexpr int kPressurePin = 4;
constexpr uint32_t kSerialBaud = 460800;
constexpr uint32_t kSamplePeriodUs = 10000;  // 100 Hz

constexpr uint8_t kRegSampleRateDivider = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegGyroConfig = 0x1B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelXoutHigh = 0x3B;
constexpr uint8_t kRegPowerManagement1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;

float gyroOffsetX = 0.0f;
float gyroOffsetY = 0.0f;
float gyroOffsetZ = 0.0f;
float pitchDeg = 0.0f;
float rollDeg = 0.0f;
float pressureBaseline = 0.0f;
float pressureFiltered = 0.0f;
float pressureOnDelta = 80.0f;
uint32_t nextSampleUs = 0;
uint32_t sampleSequence = 0;
bool sensorReady = false;
bool attitudeInitialized = false;
uint8_t sensorWhoAmI = 0;

int16_t makeInt16(uint8_t highByte, uint8_t lowByte) {
  return static_cast<int16_t>((static_cast<uint16_t>(highByte) << 8) | lowByte);
}

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kMpuAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool readRegisters(uint8_t startReg, uint8_t* buffer, size_t length) {
  Wire.beginTransmission(kMpuAddress);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return false;

  const size_t received =
      Wire.requestFrom(static_cast<int>(kMpuAddress), static_cast<int>(length), true);
  if (received != length) return false;
  for (size_t i = 0; i < length; ++i) buffer[i] = Wire.read();
  return true;
}

bool readRegister(uint8_t reg, uint8_t& value) {
  return readRegisters(reg, &value, 1);
}

void scanI2cBus() {
  Serial.println("INFO,I2C_SCAN_START");
  int found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission(true) == 0) {
      Serial.printf("INFO,I2C_DEVICE,0x%02X\n", address);
      ++found;
    }
  }
  Serial.printf("INFO,I2C_SCAN_DONE,%d\n", found);
}

bool readRawSample(int16_t& ax, int16_t& ay, int16_t& az, int16_t& temp,
                   int16_t& gx, int16_t& gy, int16_t& gz) {
  uint8_t data[14];
  if (!readRegisters(kRegAccelXoutHigh, data, sizeof(data))) return false;
  ax = makeInt16(data[0], data[1]);
  ay = makeInt16(data[2], data[3]);
  az = makeInt16(data[4], data[5]);
  temp = makeInt16(data[6], data[7]);
  gx = makeInt16(data[8], data[9]);
  gy = makeInt16(data[10], data[11]);
  gz = makeInt16(data[12], data[13]);
  return true;
}

uint16_t readPressureRaw() {
  uint32_t sum = 0;
  constexpr int kOversamples = 4;
  for (int i = 0; i < kOversamples; ++i) sum += analogRead(kPressurePin);
  return static_cast<uint16_t>(sum / kOversamples);
}

bool initializeImu() {
  uint8_t whoAmI = 0;
  if (!readRegister(kRegWhoAmI, whoAmI)) {
    Serial.println("ERROR,WHO_AM_I_READ_FAILED");
    return false;
  }
  Serial.printf("INFO,WHO_AM_I,0x%02X\n", whoAmI);
  if (whoAmI != 0x68 && whoAmI != 0x69 && whoAmI != 0x70) {
    Serial.println("ERROR,UNEXPECTED_WHO_AM_I");
    return false;
  }
  sensorWhoAmI = whoAmI;
  Serial.println(whoAmI == 0x70
                     ? "INFO,SENSOR_MODEL,MPU6500_COMPATIBLE"
                     : "INFO,SENSOR_MODEL,MPU6050_COMPATIBLE");

  bool ok = true;
  ok &= writeRegister(kRegPowerManagement1, 0x00);
  delay(100);
  ok &= writeRegister(kRegConfig, 0x03);
  ok &= writeRegister(kRegSampleRateDivider, 0x09);
  ok &= writeRegister(kRegGyroConfig, 0x00);
  ok &= writeRegister(kRegAccelConfig, 0x00);
  if (!ok) {
    Serial.println("ERROR,MPU_CONFIG_WRITE_FAILED");
    return false;
  }
  Serial.println("INFO,IMU_READY");
  return true;
}

bool calibrateGyroscope() {
  constexpr int kCalibrationSamples = 300;
  int64_t sumX = 0, sumY = 0, sumZ = 0;
  int validSamples = 0;
  Serial.println("INFO,GYRO_CALIBRATION_START,KEEP_SENSOR_STILL");
  for (int i = 0; i < kCalibrationSamples; ++i) {
    int16_t ax, ay, az, temp, gx, gy, gz;
    if (readRawSample(ax, ay, az, temp, gx, gy, gz)) {
      sumX += gx;
      sumY += gy;
      sumZ += gz;
      ++validSamples;
    }
    delay(5);
  }
  if (validSamples < kCalibrationSamples * 9 / 10) {
    Serial.printf("ERROR,GYRO_CALIBRATION_TOO_FEW_SAMPLES,%d\n", validSamples);
    return false;
  }
  gyroOffsetX = static_cast<float>(sumX) / validSamples / 131.0f;
  gyroOffsetY = static_cast<float>(sumY) / validSamples / 131.0f;
  gyroOffsetZ = static_cast<float>(sumZ) / validSamples / 131.0f;
  Serial.printf("INFO,GYRO_CALIBRATION_DONE,%.4f,%.4f,%.4f\n",
                gyroOffsetX, gyroOffsetY, gyroOffsetZ);
  return true;
}

void calibratePressure() {
  constexpr int kCalibrationSamples = 300;
  uint32_t sum = 0;
  uint16_t minimum = 4095;
  uint16_t maximum = 0;
  Serial.println("INFO,PRESSURE_CALIBRATION_START,RELEASE_SENSOR");
  for (int i = 0; i < kCalibrationSamples; ++i) {
    const uint16_t raw = readPressureRaw();
    sum += raw;
    minimum = min(minimum, raw);
    maximum = max(maximum, raw);
    delay(10);
  }
  pressureBaseline = static_cast<float>(sum) / kCalibrationSamples;
  pressureFiltered = pressureBaseline;
  const float noisePeakToPeak = static_cast<float>(maximum - minimum);
  pressureOnDelta = max(80.0f, noisePeakToPeak * 8.0f);
  Serial.printf("INFO,PRESSURE_CALIBRATION_DONE,BASELINE=%.1f,MIN=%u,MAX=%u,ON_DELTA=%.1f\n",
                pressureBaseline, minimum, maximum, pressureOnDelta);
  if (pressureBaseline > 3800.0f) {
    Serial.println("ERROR,PRESSURE_BASELINE_SATURATED,CHECK_DIVIDER");
  }
}

void initializeSensors() {
  scanI2cBus();
  sensorReady = initializeImu();
  if (sensorReady) sensorReady = calibrateGyroscope();
  if (!sensorReady) {
    Serial.println("ERROR,SENSOR_NOT_READY,CHECK_WIRING");
    return;
  }
  calibratePressure();
  Serial.println(
      "HEADER,seq,t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,temp_c,pitch_deg,roll_deg,acc_mag_g,pressure_raw,pressure_filtered,pressure_baseline,pressure_delta,pressure_state");
  sampleSequence = 0;
  nextSampleUs = micros() + kSamplePeriodUs;
}
}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(1500);
  Serial.println();
  Serial.println("INFO,PECKY_PRESSURE_IMU_STREAM_V1");
  Serial.printf("INFO,PINS,SDA=%d,SCL=%d,PRESSURE_ADC=%d,ADDRESS=0x%02X\n",
                kSdaPin, kSclPin, kPressurePin, kMpuAddress);

  analogReadResolution(12);
  analogSetPinAttenuation(kPressurePin, ADC_11db);
  pinMode(kPressurePin, INPUT);
  Wire.begin(kSdaPin, kSclPin, 400000);
  Wire.setTimeOut(50);
  initializeSensors();
}

void loop() {
  if (!sensorReady) {
    delay(2000);
    initializeSensors();
    return;
  }

  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - nextSampleUs) < 0) return;
  if (static_cast<int32_t>(nowUs - nextSampleUs) >=
      static_cast<int32_t>(kSamplePeriodUs)) {
    Serial.printf("WARN,SAMPLE_LATE_US,%ld\n",
                  static_cast<long>(nowUs - nextSampleUs));
    nextSampleUs = nowUs;
  }
  nextSampleUs += kSamplePeriodUs;

  int16_t rawAx, rawAy, rawAz, rawTemp, rawGx, rawGy, rawGz;
  if (!readRawSample(rawAx, rawAy, rawAz, rawTemp, rawGx, rawGy, rawGz)) {
    Serial.println("ERROR,MPU_READ_FAILED");
    sensorReady = false;
    return;
  }

  const uint16_t pressureRaw = readPressureRaw();
  constexpr float kPressureEmaAlpha = 0.20f;
  pressureFiltered += kPressureEmaAlpha *
                      (static_cast<float>(pressureRaw) - pressureFiltered);
  const float pressureDelta = max(0.0f, pressureFiltered - pressureBaseline);
  const bool pressureActive = pressureDelta >= pressureOnDelta;

  const float ax = rawAx / 16384.0f;
  const float ay = rawAy / 16384.0f;
  const float az = rawAz / 16384.0f;
  const float gx = rawGx / 131.0f - gyroOffsetX;
  const float gy = rawGy / 131.0f - gyroOffsetY;
  const float gz = rawGz / 131.0f - gyroOffsetZ;
  const float temperature = sensorWhoAmI == 0x70
                                ? rawTemp / 333.87f + 21.0f
                                : rawTemp / 340.0f + 36.53f;
  const float accelerationMagnitude = sqrtf(ax * ax + ay * ay + az * az);
  const float accelerometerPitch =
      atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;
  const float accelerometerRoll = atan2f(ay, az) * 180.0f / PI;
  constexpr float kDtSeconds = kSamplePeriodUs / 1000000.0f;
  constexpr float kGyroWeight = 0.98f;
  if (!attitudeInitialized) {
    pitchDeg = accelerometerPitch;
    rollDeg = accelerometerRoll;
    attitudeInitialized = true;
  } else {
    pitchDeg = kGyroWeight * (pitchDeg + gy * kDtSeconds) +
               (1.0f - kGyroWeight) * accelerometerPitch;
    rollDeg = kGyroWeight * (rollDeg + gx * kDtSeconds) +
              (1.0f - kGyroWeight) * accelerometerRoll;
  }

  Serial.printf(
      "DATA,%lu,%lu,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.2f,%.3f,%.3f,%.5f,%u,%.1f,%.1f,%.1f,%s\n",
      sampleSequence++, millis(), ax, ay, az, gx, gy, gz, temperature,
      pitchDeg, rollDeg, accelerationMagnitude, pressureRaw,
      pressureFiltered, pressureBaseline, pressureDelta,
      pressureActive ? "PRESSED" : "RELEASED");
}
