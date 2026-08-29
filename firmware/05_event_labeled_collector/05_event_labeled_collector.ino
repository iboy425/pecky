/*
 * Pecky Cap — event-labelled USB collector
 *
 * This firmware deliberately does not guess the performed action.  A host
 * program marks PREPARE/ACTION/HOLD/RETURN/REST boundaries over USB while the
 * ESP32-S3 attaches the active label to every 25 Hz sensor frame.
 *
 * Verified hat wiring:
 *   MPU6050/MPU6500-compatible IMU: SDA GPIO16, SCL GPIO15, address 0x68
 *   RFP602 pressure-divider output: GPIO4 (0..3.3 V only)
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr char kFirmwareVersion[] = "PECKY_EVENT_COLLECTOR_V1";
constexpr uint8_t kMpuAddress = 0x68;
constexpr int kSdaPin = 16;
constexpr int kSclPin = 15;
constexpr int kPressurePin = 4;
constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kSamplePeriodUs = 40000;  // 25 Hz
constexpr int kCalibrationSamples = 75;      // 3 seconds at 25 Hz

constexpr uint8_t kRegSampleRateDivider = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegGyroConfig = 0x1B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelXoutHigh = 0x3B;
constexpr uint8_t kRegPowerManagement1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;

struct LabelContext {
  uint16_t trial = 0;
  uint8_t action = 0;
  uint8_t repetition = 0;
  uint8_t stage = 0;
};

bool imuReady = false;
bool calibrated = false;
bool recording = false;
uint8_t sensorWhoAmI = 0;
uint8_t consecutiveReadErrors = 0;
uint32_t sequenceNumber = 0;
uint32_t nextSampleUs = 0;
uint32_t previousSampleMs = 0;
uint32_t droppedFrames = 0;
float gyroBiasX = 0.0f;
float gyroBiasY = 0.0f;
float gyroBiasZ = 0.0f;
uint16_t pressureBaseline = 0;
uint16_t pressureMad = 0;
LabelContext activeLabel;

char commandBuffer[128] = {};
size_t commandLength = 0;

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
  const size_t received = Wire.requestFrom(
      static_cast<int>(kMpuAddress), static_cast<int>(length), true);
  if (received != length) return false;
  for (size_t i = 0; i < length; ++i) buffer[i] = Wire.read();
  return true;
}

bool readRawSample(int16_t& ax, int16_t& ay, int16_t& az,
                   int16_t& gx, int16_t& gy, int16_t& gz) {
  uint8_t data[14];
  if (!readRegisters(kRegAccelXoutHigh, data, sizeof(data))) return false;
  ax = makeInt16(data[0], data[1]);
  ay = makeInt16(data[2], data[3]);
  az = makeInt16(data[4], data[5]);
  gx = makeInt16(data[8], data[9]);
  gy = makeInt16(data[10], data[11]);
  gz = makeInt16(data[12], data[13]);
  return true;
}

uint16_t readPressureRaw() {
  uint32_t sum = 0;
  for (int i = 0; i < 4; ++i) sum += analogRead(kPressurePin);
  return static_cast<uint16_t>(sum / 4U);
}

int compareU16(const void* left, const void* right) {
  const uint16_t a = *static_cast<const uint16_t*>(left);
  const uint16_t b = *static_cast<const uint16_t*>(right);
  return (a > b) - (a < b);
}

uint16_t medianU16(uint16_t* values, size_t count) {
  qsort(values, count, sizeof(values[0]), compareU16);
  return values[count / 2];
}

void clearLabel() {
  activeLabel = LabelContext{};
}

bool initializeImu() {
  uint8_t whoAmI = 0;
  if (!readRegisters(kRegWhoAmI, &whoAmI, 1)) {
    Serial.println("ERROR,WHO_AM_I_READ_FAILED");
    return false;
  }
  if (whoAmI != 0x68 && whoAmI != 0x69 && whoAmI != 0x70) {
    Serial.printf("ERROR,UNEXPECTED_WHO_AM_I,0x%02X\n", whoAmI);
    return false;
  }
  sensorWhoAmI = whoAmI;

  bool ok = true;
  ok &= writeRegister(kRegPowerManagement1, 0x00);
  delay(100);
  ok &= writeRegister(kRegConfig, 0x03);             // 44 Hz DLPF
  ok &= writeRegister(kRegSampleRateDivider, 0x09);  // 100 Hz internal
  ok &= writeRegister(kRegGyroConfig, 0x00);         // +/-250 dps
  ok &= writeRegister(kRegAccelConfig, 0x00);        // +/-2 g
  if (!ok) {
    Serial.println("ERROR,MPU_CONFIG_WRITE_FAILED");
    return false;
  }
  return true;
}

void printHello() {
  Serial.printf(
      "PONG,%s,FS=25,SDA=%d,SCL=%d,PRESSURE=%d,WHO=0x%02X\n",
      kFirmwareVersion, kSdaPin, kSclPin, kPressurePin, sensorWhoAmI);
}

void printStatus() {
  Serial.printf(
      "STATUS,IMU=%s,CAL=%s,REC=%s,SEQ=%lu,DROPS=%lu,P0=%u,PN=%u\n",
      imuReady ? "OK" : "FAIL", calibrated ? "YES" : "NO",
      recording ? "YES" : "NO", static_cast<unsigned long>(sequenceNumber),
      static_cast<unsigned long>(droppedFrames), pressureBaseline, pressureMad);
}

bool parseUnsigned(const char* text, long minimum, long maximum, long& value) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const long parsed = strtol(text, &end, 10);
  if (*end != '\0' || parsed < minimum || parsed > maximum) return false;
  value = parsed;
  return true;
}

void calibrateSensors() {
  if (!imuReady || recording) {
    Serial.println("CAL_FAIL,BAD_STATE");
    return;
  }

  Serial.println("CAL_START,KEEP_NEUTRAL_STILL,RELEASE_PRESSURE");
  double sumAccelNorm = 0.0;
  double sumAccelNormSquared = 0.0;
  double sumGx = 0.0;
  double sumGy = 0.0;
  double sumGz = 0.0;
  double sumGyroSquared = 0.0;
  uint16_t pressures[kCalibrationSamples];
  int valid = 0;
  uint32_t deadline = micros();

  for (int i = 0; i < kCalibrationSamples; ++i) {
    deadline += kSamplePeriodUs;
    while (static_cast<int32_t>(micros() - deadline) < 0) delay(1);

    int16_t ax, ay, az, gx, gy, gz;
    if (!readRawSample(ax, ay, az, gx, gy, gz)) continue;
    const double axG = ax / 16384.0;
    const double ayG = ay / 16384.0;
    const double azG = az / 16384.0;
    const double gxDps = gx / 131.0;
    const double gyDps = gy / 131.0;
    const double gzDps = gz / 131.0;
    const double accelNorm = sqrt(axG * axG + ayG * ayG + azG * azG);
    sumAccelNorm += accelNorm;
    sumAccelNormSquared += accelNorm * accelNorm;
    sumGx += gxDps;
    sumGy += gyDps;
    sumGz += gzDps;
    sumGyroSquared += gxDps * gxDps + gyDps * gyDps + gzDps * gzDps;
    pressures[valid] = readPressureRaw();
    ++valid;
  }

  if (valid < 70) {
    calibrated = false;
    Serial.printf("CAL_FAIL,I2C_SAMPLES,%d\n", valid);
    return;
  }

  const double accelMean = sumAccelNorm / valid;
  const double accelVariance =
      fmax(0.0, sumAccelNormSquared / valid - accelMean * accelMean);
  const double accelStd = sqrt(accelVariance);
  gyroBiasX = static_cast<float>(sumGx / valid);
  gyroBiasY = static_cast<float>(sumGy / valid);
  gyroBiasZ = static_cast<float>(sumGz / valid);
  const double meanGyroEnergy = sumGyroSquared / valid;
  const double biasEnergy = gyroBiasX * gyroBiasX + gyroBiasY * gyroBiasY +
                            gyroBiasZ * gyroBiasZ;
  const double gyroRms = sqrt(fmax(0.0, meanGyroEnergy - biasEnergy));

  pressureBaseline = medianU16(pressures, valid);
  uint16_t deviations[kCalibrationSamples];
  for (int i = 0; i < valid; ++i) {
    deviations[i] = pressures[i] > pressureBaseline
                        ? pressures[i] - pressureBaseline
                        : pressureBaseline - pressures[i];
  }
  pressureMad = medianU16(deviations, valid);

  if (accelMean < 0.90 || accelMean > 1.10) {
    calibrated = false;
    Serial.printf("CAL_FAIL,ACC_NORM,MEAN=%.4f,STD=%.4f\n", accelMean, accelStd);
    return;
  }
  if (accelStd > 0.05 || gyroRms > 10.0) {
    calibrated = false;
    Serial.printf("CAL_FAIL,MOVED,ASTD=%.4f,GRMS=%.3f\n", accelStd, gyroRms);
    return;
  }
  if (pressureBaseline >= 4085) {
    calibrated = false;
    Serial.printf("CAL_FAIL,PRESSURE_SATURATED,P0=%u\n", pressureBaseline);
    return;
  }

  calibrated = true;
  consecutiveReadErrors = 0;
  Serial.printf(
      "CAL_OK,N=%d,AMEAN=%.5f,ASTD=%.5f,GRMS=%.4f,GBX=%.4f,GBY=%.4f,GBZ=%.4f,P0=%u,PMAD=%u\n",
      valid, accelMean, accelStd, gyroRms, gyroBiasX, gyroBiasY, gyroBiasZ,
      pressureBaseline, pressureMad);
}

void handleCommand(char* command) {
  char* save = nullptr;
  char* verb = strtok_r(command, ",", &save);
  if (verb == nullptr) return;

  if (strcmp(verb, "HELLO") == 0 || strcmp(verb, "PING") == 0) {
    printHello();
    return;
  }
  if (strcmp(verb, "STATUS") == 0) {
    printStatus();
    return;
  }
  if (strcmp(verb, "CAL") == 0) {
    calibrateSensors();
    return;
  }
  if (strcmp(verb, "BEGIN") == 0) {
    if (!imuReady || !calibrated || recording) {
      Serial.println("NACK,BEGIN,BAD_STATE");
      return;
    }
    recording = true;
    clearLabel();
    sequenceNumber = 0;
    droppedFrames = 0;
    previousSampleMs = millis();
    nextSampleUs = micros() + kSamplePeriodUs;
    Serial.printf("ACK,BEGIN,%lu,%lu\n", static_cast<unsigned long>(millis()),
                  static_cast<unsigned long>(sequenceNumber));
    return;
  }
  if (strcmp(verb, "LABEL") == 0) {
    char* commandIdText = strtok_r(nullptr, ",", &save);
    char* trialText = strtok_r(nullptr, ",", &save);
    char* actionText = strtok_r(nullptr, ",", &save);
    char* repetitionText = strtok_r(nullptr, ",", &save);
    char* stageText = strtok_r(nullptr, ",", &save);
    long commandId, trial, action, repetition, stage;
    const bool valid = recording &&
        parseUnsigned(commandIdText, 0, 999999, commandId) &&
        parseUnsigned(trialText, 0, 65535, trial) &&
        parseUnsigned(actionText, 0, 255, action) &&
        parseUnsigned(repetitionText, 0, 255, repetition) &&
        parseUnsigned(stageText, 0, 5, stage) &&
        strtok_r(nullptr, ",", &save) == nullptr;
    if (!valid) {
      Serial.println("NACK,LABEL,BAD_FIELDS");
      return;
    }
    activeLabel.trial = static_cast<uint16_t>(trial);
    activeLabel.action = static_cast<uint8_t>(action);
    activeLabel.repetition = static_cast<uint8_t>(repetition);
    activeLabel.stage = static_cast<uint8_t>(stage);
    Serial.printf("ACK,LABEL,%ld,%lu,%lu\n", commandId,
                  static_cast<unsigned long>(millis()),
                  static_cast<unsigned long>(sequenceNumber));
    return;
  }
  if (strcmp(verb, "ABORT") == 0) {
    char* commandIdText = strtok_r(nullptr, ",", &save);
    char* trialText = strtok_r(nullptr, ",", &save);
    long commandId, trial;
    if (!recording || !parseUnsigned(commandIdText, 0, 999999, commandId) ||
        !parseUnsigned(trialText, 0, 65535, trial)) {
      Serial.println("NACK,ABORT,BAD_FIELDS");
      return;
    }
    clearLabel();
    Serial.printf("ACK,ABORT,%ld,%ld,%lu,%lu\n", commandId, trial,
                  static_cast<unsigned long>(millis()),
                  static_cast<unsigned long>(sequenceNumber));
    return;
  }
  if (strcmp(verb, "END") == 0) {
    char* commandIdText = strtok_r(nullptr, ",", &save);
    long commandId;
    if (!recording || !parseUnsigned(commandIdText, 0, 999999, commandId)) {
      Serial.println("NACK,END,BAD_STATE");
      return;
    }
    recording = false;
    clearLabel();
    Serial.printf("ACK,END,%ld,%lu,%lu,DROPS=%lu\n", commandId,
                  static_cast<unsigned long>(millis()),
                  static_cast<unsigned long>(sequenceNumber),
                  static_cast<unsigned long>(droppedFrames));
    return;
  }

  Serial.println("NACK,UNKNOWN_COMMAND");
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\r') continue;
    if (incoming == '\n') {
      commandBuffer[commandLength] = '\0';
      if (commandLength > 0) handleCommand(commandBuffer);
      commandLength = 0;
      continue;
    }
    if (commandLength + 1 >= sizeof(commandBuffer)) {
      commandLength = 0;
      Serial.println("NACK,COMMAND_TOO_LONG");
      continue;
    }
    commandBuffer[commandLength++] = incoming;
  }
}

void sampleIfDue() {
  if (!recording) return;
  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - nextSampleUs) < 0) return;

  if (static_cast<int32_t>(nowUs - nextSampleUs) >=
      static_cast<int32_t>(kSamplePeriodUs)) {
    const uint32_t latePeriods = (nowUs - nextSampleUs) / kSamplePeriodUs;
    droppedFrames += latePeriods;
    nextSampleUs = nowUs;
  }
  nextSampleUs += kSamplePeriodUs;

  int16_t ax, ay, az, gx, gy, gz;
  if (!readRawSample(ax, ay, az, gx, gy, gz)) {
    ++consecutiveReadErrors;
    Serial.printf("WARN,I2C_READ_FAILED,%u\n", consecutiveReadErrors);
    if (consecutiveReadErrors >= 3) {
      recording = false;
      calibrated = false;
      clearLabel();
      Serial.println("ERROR,I2C_ABORTED_SESSION");
    }
    return;
  }
  consecutiveReadErrors = 0;

  const uint32_t nowMs = millis();
  const uint32_t dtMs = nowMs - previousSampleMs;
  previousSampleMs = nowMs;
  const uint16_t pressure = readPressureRaw();
  Serial.printf(
      "RAW,%lu,%lu,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%u,%lu\n",
      static_cast<unsigned long>(nowMs),
      static_cast<unsigned long>(sequenceNumber), activeLabel.trial,
      activeLabel.action, activeLabel.repetition, activeLabel.stage, ax, ay, az,
      gx, gy, gz, pressure, static_cast<unsigned long>(dtMs));
  ++sequenceNumber;
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(1200);
  analogReadResolution(12);
  analogSetPinAttenuation(kPressurePin, ADC_11db);
  pinMode(kPressurePin, INPUT);
  Wire.begin(kSdaPin, kSclPin, 400000);
  Wire.setTimeOut(50);
  imuReady = initializeImu();

  Serial.printf("READY,%s,IMU=%s,WHO=0x%02X\n", kFirmwareVersion,
                imuReady ? "OK" : "FAIL", sensorWhoAmI);
  printHello();
  if (!imuReady) Serial.println("ERROR,CHECK_IMU_WIRING,SDA16,SCL15,VCC3V3,GND");
}

void loop() {
  readSerialCommands();
  sampleIfDue();
  delay(1);
}
