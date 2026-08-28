/*
 * Pecky Cap — hat-only offline data logger
 *
 * Hardware already on the cap:
 *   ESP32-S3 (built with the safe 8 MB flash profile)
 *   MPU6500/MPU6050-compatible IMU at I2C address 0x68
 *   optional existing RFP602 pressure divider at GPIO4
 *
 * Behaviour (V2 — RST toggle collection):
 *   - normal power-on / USB connection stays idle and never creates a session;
 *   - press RST once while idle -> keep still for 3 seconds -> start a new CSV;
 *   - press RST again while recording -> stop that session. The next RST starts
 *     the next participant's file;
 *   - samples are flushed every 250 ms, so an abrupt reset may lose at most the
 *     final quarter-second rather than mixing walking / setup data into a file;
 *   - connect USB later and use LIST / DUMP /hat_XXXX.csv over Serial to read
 *     saved sessions without adding an SD card.
 */

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_system.h>
#include <math.h>

namespace {

// Keep these aligned with the cap's current wiring.
constexpr uint8_t kMpuAddress = 0x68;
constexpr int kSdaPin = 16;
constexpr int kSclPin = 15;
constexpr int kPressurePin = 4;

constexpr uint32_t kSerialBaud = 115200;
// 25 Hz comfortably captures slow neck exercises and lets ten one-minute
// participants fit in the 3 MB local filesystem as raw CSV.
constexpr uint32_t kSamplePeriodUs = 40000;
constexpr uint32_t kFlushPeriodMs = 250;
constexpr uint32_t kStatusPeriodMs = 1000;
constexpr size_t kMaxLogBytes = 2800000;  // Safety limit inside the 3 MB LittleFS layout.

constexpr uint8_t kRegSampleRateDivider = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegGyroConfig = 0x1B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelXoutHigh = 0x3B;
constexpr uint8_t kRegPowerManagement1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;

File logFile;
Preferences preferences;

bool imuReady = false;
bool logging = false;
bool captureRequested = false;
bool pausedForSerialTransfer = false;
bool attitudeInitialized = false;
uint8_t sensorWhoAmI = 0;
uint32_t nextSampleUs = 0;
uint32_t sessionStartMs = 0;
uint32_t lastFlushMs = 0;
uint32_t lastStatusMs = 0;
uint32_t sampleSequence = 0;
char activeFilename[32] = "";

float gyroOffsetX = 0.0f;
float gyroOffsetY = 0.0f;
float gyroOffsetZ = 0.0f;
float neutralPitchDeg = 0.0f;
float neutralRollDeg = 0.0f;
float pitchDeg = 0.0f;
float rollDeg = 0.0f;
float pressureBaseline = 0.0f;

char commandBuffer[96] = {};
size_t commandLength = 0;

// The RST button resets the CPU before firmware can run. Persisting this bit
// lets the *next boot* interpret an external reset as the opposite collection
// state: idle -> start, active -> stop. Power-on / USB boots deliberately
// clear an unfinished active bit and stay idle, so simply plugging in USB never
// creates a spurious session.
bool readCaptureState(bool& configured, bool& active) {
  configured = false;
  active = false;
  if (!preferences.begin("peckylog", false)) {
    Serial.println("ERROR,PREFERENCES_OPEN_FAILED");
    return false;
  }
  // ESP32 NVS keys are limited to 15 characters.
  configured = preferences.isKey("cap_cfg");
  active = configured && preferences.getBool("cap_active", false);
  preferences.end();
  return true;
}

bool persistCaptureState(bool active) {
  if (!preferences.begin("peckylog", false)) {
    Serial.println("ERROR,PREFERENCES_OPEN_FAILED");
    return false;
  }
  const bool ok = preferences.putBool("cap_cfg", true) > 0 &&
                  preferences.putBool("cap_active", active) > 0;
  preferences.end();
  if (!ok) Serial.println("ERROR,CAPTURE_STATE_SAVE_FAILED");
  return ok;
}

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
  ok &= writeRegister(kRegPowerManagement1, 0x00);  // Wake.
  delay(100);
  ok &= writeRegister(kRegConfig, 0x03);             // DLPF about 44 Hz.
  ok &= writeRegister(kRegSampleRateDivider, 0x27);  // 1 kHz / 40 = 25 Hz.
  ok &= writeRegister(kRegGyroConfig, 0x00);         // +/-250 dps.
  ok &= writeRegister(kRegAccelConfig, 0x00);        // +/-2 g.
  if (!ok) Serial.println("ERROR,MPU_CONFIG_WRITE_FAILED");
  return ok;
}

bool calibrateAtNeutral() {
  constexpr int kSamples = 150;  // 3 seconds; calibration remains independent of log rate.
  int64_t sumGx = 0;
  int64_t sumGy = 0;
  int64_t sumGz = 0;
  int64_t sumAx = 0;
  int64_t sumAy = 0;
  int64_t sumAz = 0;
  uint32_t sumPressure = 0;
  uint16_t pressureMin = 4095;
  uint16_t pressureMax = 0;
  int valid = 0;

  Serial.println("INFO,CALIBRATION_START,KEEP_HEAD_NEUTRAL_AND_STILL_FOR_3_SECONDS");
  for (int i = 0; i < kSamples; ++i) {
    int16_t ax, ay, az, temp, gx, gy, gz;
    if (readRawSample(ax, ay, az, temp, gx, gy, gz)) {
      sumAx += ax;
      sumAy += ay;
      sumAz += az;
      sumGx += gx;
      sumGy += gy;
      sumGz += gz;
      const uint16_t pressure = readPressureRaw();
      sumPressure += pressure;
      pressureMin = min(pressureMin, pressure);
      pressureMax = max(pressureMax, pressure);
      ++valid;
    }
    delay(20);
  }

  if (valid < kSamples * 9 / 10) {
    Serial.printf("ERROR,CALIBRATION_TOO_FEW_SAMPLES,%d\n", valid);
    return false;
  }

  gyroOffsetX = static_cast<float>(sumGx) / valid / 131.0f;
  gyroOffsetY = static_cast<float>(sumGy) / valid / 131.0f;
  gyroOffsetZ = static_cast<float>(sumGz) / valid / 131.0f;
  const float ax = static_cast<float>(sumAx) / valid / 16384.0f;
  const float ay = static_cast<float>(sumAy) / valid / 16384.0f;
  const float az = static_cast<float>(sumAz) / valid / 16384.0f;
  neutralPitchDeg = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;
  neutralRollDeg = atan2f(ay, az) * 180.0f / PI;
  pitchDeg = neutralPitchDeg;
  rollDeg = neutralRollDeg;
  attitudeInitialized = true;
  pressureBaseline = static_cast<float>(sumPressure) / valid;

  Serial.printf("INFO,CALIBRATION_DONE,PITCH=%.2f,ROLL=%.2f,PRESSURE=%.1f\n",
                neutralPitchDeg, neutralRollDeg, pressureBaseline);
  if (pressureMin > 4080 || pressureMax < 15) {
    Serial.println("WARN,PRESSURE_CHANNEL_SATURATED_OR_DISCONNECTED,IMU_LOGGING_CONTINUES");
  }
  return true;
}

uint8_t phaseFor(uint32_t elapsedMs) {
  // Follow the one-minute stopwatch protocol in docs/hat_data_collection.md.
  if (elapsedMs < 5000) return 0;   // neutral_start
  if (elapsedMs < 17000) return 1;  // neck_extension
  if (elapsedMs < 22000) return 2;  // neutral_1
  if (elapsedMs < 34000) return 3;  // chin_tuck
  if (elapsedMs < 39000) return 4;  // neutral_2
  if (elapsedMs < 54000) return 5;  // head_resistance
  if (elapsedMs < 60000) return 6;  // normal_motion
  return 7;                          // free
}

const char* phaseName(uint8_t phase) {
  static const char* const kNames[] = {
      "neutral_start", "neck_extension", "neutral_1", "chin_tuck",
      "neutral_2", "head_resistance", "normal_motion", "free"};
  return phase < sizeof(kNames) / sizeof(kNames[0]) ? kNames[phase] : "unknown";
}

void closeLogFile() {
  if (logFile) {
    logFile.flush();
    logFile.close();
  }
  logging = false;
}

void stopCapture(const char* reason) {
  closeLogFile();
  captureRequested = false;
  persistCaptureState(false);
  Serial.printf("INFO,CAPTURE_STOPPED,%s\n", reason);
}

bool openNewLogFile() {
  if (logging) return true;
  if (!preferences.begin("peckylog", false)) {
    Serial.println("ERROR,PREFERENCES_OPEN_FAILED");
    return false;
  }
  const uint32_t sessionId = preferences.getULong("session", 0) + 1;
  preferences.putULong("session", sessionId);
  preferences.end();

  snprintf(activeFilename, sizeof(activeFilename), "/hat_%04lu.csv",
           static_cast<unsigned long>(sessionId));
  logFile = LittleFS.open(activeFilename, FILE_WRITE);
  if (!logFile) {
    Serial.printf("ERROR,LOG_OPEN_FAILED,%s\n", activeFilename);
    return false;
  }

  // Compact raw CSV: approximately 100 KB per participant-minute, so ten
  // participants remain well within the local 3 MB filesystem.
  logFile.println("seq,t_ms,phase,ax_raw,ay_raw,az_raw,gx_raw,gy_raw,gz_raw,pressure_raw");
  logFile.flush();
  sessionStartMs = millis();
  lastFlushMs = sessionStartMs;
  lastStatusMs = sessionStartMs;
  sampleSequence = 0;
  logging = true;
  if (!persistCaptureState(true)) {
    closeLogFile();
    return false;
  }
  Serial.printf("INFO,LOGGING_STARTED,%s\n", activeFilename);
  Serial.println("INFO,RST_TOGGLE,RECORDING_PRESS_RST_AGAIN_TO_STOP");
  Serial.println("INFO,SERIAL_COMMANDS,LIST | DUMP /hat_XXXX.csv | STATUS | ERASE_ALL");
  return true;
}

// Never silently format a filesystem that was already used for participant
// data. A fresh board is formatted once; later mount failures are reported so
// the old sessions can be recovered instead of overwritten.
bool mountLittleFsSafely() {
  bool filesystemWasInitialized = false;
  if (preferences.begin("peckylog", false)) {
    filesystemWasInitialized = preferences.getBool("fs_ready", false);
    preferences.end();
  }

  if (LittleFS.begin(false)) return true;
  if (filesystemWasInitialized) {
    Serial.println("ERROR,LITTLEFS_MOUNT_FAILED,REFUSING_TO_FORMAT_EXISTING_DATA");
    return false;
  }

  Serial.println("INFO,LITTLEFS_FIRST_USE_FORMATTING");
  if (!LittleFS.format() || !LittleFS.begin(false)) {
    Serial.println("ERROR,LITTLEFS_INITIAL_FORMAT_FAILED");
    return false;
  }
  if (preferences.begin("peckylog", false)) {
    preferences.putBool("fs_ready", true);
    preferences.end();
  }
  return true;
}

void listFiles() {
  if (logFile) logFile.flush();
  Serial.printf("FILES_BEGIN,TOTAL=%u,USED=%u\n",
                static_cast<unsigned>(LittleFS.totalBytes()),
                static_cast<unsigned>(LittleFS.usedBytes()));
  File root = LittleFS.open("/");
  File entry = root.openNextFile();
  while (entry) {
    Serial.printf("FILE,%s,%u\n", entry.name(), static_cast<unsigned>(entry.size()));
    entry = root.openNextFile();
  }
  Serial.println("FILES_END");
}

bool safeFilename(const String& filename) {
  return filename.startsWith("/hat_") && filename.endsWith(".csv") &&
         filename.indexOf("..") < 0;
}

void dumpFile(const String& filename) {
  if (!safeFilename(filename)) {
    Serial.println("ERROR,INVALID_FILENAME");
    return;
  }
  if (logFile) logFile.flush();
  File source = LittleFS.open(filename, FILE_READ);
  if (!source) {
    Serial.printf("ERROR,FILE_NOT_FOUND,%s\n", filename.c_str());
    return;
  }

  pausedForSerialTransfer = true;
  Serial.printf("FILE_BEGIN,%s,%u\n", filename.c_str(),
                static_cast<unsigned>(source.size()));
  uint8_t buffer[192];
  while (source.available()) {
    const size_t bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead == 0) break;
    Serial.write(buffer, bytesRead);
  }
  source.close();
  Serial.printf("\nFILE_END,%s\n", filename.c_str());
  pausedForSerialTransfer = false;
  nextSampleUs = micros() + kSamplePeriodUs;
}

void printStatus() {
  Serial.printf("STATUS,IMU=%s,MODE=%s,LOG=%s,FILE=%s,USED=%u,TOTAL=%u\n",
                imuReady ? "READY" : "NOT_READY",
                captureRequested ? "CAPTURE" : "IDLE",
                logging ? "ON" : "OFF",
                activeFilename[0] ? activeFilename : "NONE",
                static_cast<unsigned>(LittleFS.usedBytes()),
                static_cast<unsigned>(LittleFS.totalBytes()));
}

void eraseAllLogs() {
  // Intentionally only reachable through the exact ERASE_ALL command. It is
  // useful after a successfully downloaded session, not during normal use.
  closeLogFile();
  if (!LittleFS.format() || !LittleFS.begin(false)) {
    Serial.println("ERROR,ERASE_ALL_FAILED");
    return;
  }
  activeFilename[0] = '\0';
  captureRequested = false;
  persistCaptureState(false);
  Serial.println("INFO,ERASE_ALL_COMPLETE");
  Serial.println("INFO,CAPTURE_IDLE,PRESS_RST_ONCE_TO_START");
}

void executeCommand(char* rawCommand) {
  String command(rawCommand);
  command.trim();
  if (command.length() == 0) return;
  String upper = command;
  upper.toUpperCase();
  if (upper == "LIST") {
    listFiles();
  } else if (upper == "STATUS") {
    printStatus();
  } else if (upper.startsWith("DUMP ")) {
    String filename = command.substring(5);
    filename.trim();
    dumpFile(filename);
  } else if (upper == "HELP") {
    Serial.println("INFO,SERIAL_COMMANDS,LIST | DUMP /hat_XXXX.csv | STATUS | ERASE_ALL");
  } else if (upper == "ERASE_ALL") {
    eraseAllLogs();
  } else {
    Serial.println("ERROR,UNKNOWN_COMMAND,USE_HELP");
  }
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\r') continue;
    if (incoming == '\n') {
      commandBuffer[commandLength] = '\0';
      executeCommand(commandBuffer);
      commandLength = 0;
      continue;
    }
    if (commandLength + 1 < sizeof(commandBuffer)) {
      commandBuffer[commandLength++] = incoming;
    } else {
      commandLength = 0;
      Serial.println("ERROR,COMMAND_TOO_LONG");
    }
  }
}

void logSample() {
  int16_t rawAx, rawAy, rawAz, rawTemp, rawGx, rawGy, rawGz;
  if (!readRawSample(rawAx, rawAy, rawAz, rawTemp, rawGx, rawGy, rawGz)) {
    Serial.println("ERROR,MPU_READ_FAILED");
    imuReady = false;
    return;
  }

  const float ax = rawAx / 16384.0f;
  const float ay = rawAy / 16384.0f;
  const float az = rawAz / 16384.0f;
  const float gx = rawGx / 131.0f - gyroOffsetX;
  const float gy = rawGy / 131.0f - gyroOffsetY;
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

  const uint16_t pressureRaw = readPressureRaw();
  const float pressureDelta = static_cast<float>(pressureRaw) - pressureBaseline;
  const uint32_t elapsedMs = millis() - sessionStartMs;
  const uint8_t phase = phaseFor(elapsedMs);

  char row[128];
  const int rowLength = snprintf(
      row, sizeof(row), "%lu,%lu,%u,%d,%d,%d,%d,%d,%d,%u\n",
      static_cast<unsigned long>(sampleSequence++),
      static_cast<unsigned long>(elapsedMs), phase, rawAx, rawAy, rawAz, rawGx,
      rawGy, rawGz, pressureRaw);
  if (rowLength <= 0 || static_cast<size_t>(rowLength) >= sizeof(row) ||
      logFile.print(row) == 0) {
    Serial.println("ERROR,LOG_WRITE_FAILED");
    stopCapture("LOG_WRITE_FAILED");
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs - lastFlushMs >= kFlushPeriodMs) {
    logFile.flush();
    lastFlushMs = nowMs;
  }
  if (nowMs - lastStatusMs >= kStatusPeriodMs) {
    Serial.printf("LIVE,t=%lu,phase=%s,pitch=%.1f,roll=%.1f,pressure=%u\n",
                  static_cast<unsigned long>(elapsedMs), phaseName(phase),
                  pitchDeg - neutralPitchDeg, rollDeg - neutralRollDeg, pressureRaw);
    lastStatusMs = nowMs;
  }
  if (logFile.size() >= kMaxLogBytes) {
    Serial.println("WARN,LOG_SIZE_LIMIT_REACHED,STOPPING_TO_PRESERVE_OLD_SESSIONS");
    stopCapture("LOG_SIZE_LIMIT_REACHED");
  }
}

bool initializeRun() {
  imuReady = initializeImu();
  if (!imuReady) return false;
  imuReady = calibrateAtNeutral();
  if (!imuReady) return false;
  return openNewLogFile();
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(700);
  Serial.println();
  Serial.println("INFO,PECKY_HAT_FLASH_LOGGER_V2_RST_TOGGLE");
  Serial.printf("INFO,PINS,SDA=%d,SCL=%d,PRESSURE_ADC=%d\n", kSdaPin, kSclPin,
                kPressurePin);

  analogReadResolution(12);
  analogSetPinAttenuation(kPressurePin, ADC_11db);
  pinMode(kPressurePin, INPUT);
  Wire.begin(kSdaPin, kSclPin, 400000);
  Wire.setTimeOut(50);

  if (!mountLittleFsSafely()) {
    return;
  }
  Serial.printf("INFO,LITTLEFS_READY,TOTAL=%u,USED=%u\n",
                static_cast<unsigned>(LittleFS.totalBytes()),
                static_cast<unsigned>(LittleFS.usedBytes()));

  bool captureConfigured = false;
  bool captureWasActive = false;
  if (!readCaptureState(captureConfigured, captureWasActive)) return;

  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("INFO,RESET_REASON,%d\n", static_cast<int>(resetReason));

  // USB insertion, battery power-on, upload and brownout resets never start a
  // session. If power was removed while recording, that old file is already
  // complete enough to download (we flush every 250 ms), so mark it closed.
  if (resetReason != ESP_RST_EXT) {
    if (!captureConfigured || captureWasActive) persistCaptureState(false);
    Serial.println("INFO,CAPTURE_IDLE,PRESS_RST_ONCE_TO_START");
    return;
  }

  // An external RST is the intended one-button toggle. Its prior state decides
  // whether this boot starts the next participant or closes the previous one.
  if (captureWasActive) {
    persistCaptureState(false);
    Serial.println("INFO,RST_TOGGLE,STOPPED");
    Serial.println("INFO,CAPTURE_IDLE,PRESS_RST_ONCE_TO_START");
    return;
  }

  captureRequested = true;
  Serial.println("INFO,RST_TOGGLE,STARTING_KEEP_STILL_FOR_CALIBRATION");
  if (!initializeRun()) {
    Serial.println("ERROR,STARTUP_FAILED,RETRYING_EVERY_2_SECONDS");
  }
}

void loop() {
  handleSerialCommands();
  if (!captureRequested) {
    delay(5);
    return;
  }
  if (!imuReady) {
    delay(2000);
    if (initializeRun()) Serial.println("INFO,RECOVERED_AND_LOGGING");
    return;
  }
  if (!logging || pausedForSerialTransfer) {
    delay(5);
    return;
  }

  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - nextSampleUs) < 0) return;
  if (static_cast<int32_t>(nowUs - nextSampleUs) >=
      static_cast<int32_t>(kSamplePeriodUs)) {
    nextSampleUs = nowUs;
  }
  nextSampleUs += kSamplePeriodUs;
  logSample();
}
