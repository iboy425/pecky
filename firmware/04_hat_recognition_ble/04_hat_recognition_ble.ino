/*
 * Pecky Cap — three independent recognizers + unified BLE output
 *
 * Current verified hardware:
 *   ESP32-S3, MPU6500-compatible sensor at I2C 0x68
 *   SDA GPIO16, SCL GPIO15, RFP-602 pressure divider ADC GPIO4
 *
 * Recognition flow:
 *   neutral calibration -> shared features -> three recognizers -> arbiter
 *   -> durable rep sequence -> BLE EVENT + SNAPSHOT
 *
 * This sketch does not use the logger's phase/timer as an input.  It can run
 * completely without a phone; the phone receives semantic results, never raw
 * IMU frames.  Keep firmware/03_hat_flash_logger for future data collection.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>

#include "recognition_engine.h"

namespace {

constexpr uint8_t kMpuAddress = 0x68;
constexpr int kSdaPin = 16;
constexpr int kSclPin = 15;
constexpr int kPressurePin = 4;
constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kSamplePeriodUs = 40000;
constexpr uint32_t kProgressPeriodMs = 200;
constexpr uint8_t kGroupTarget = 3;
// The assembled wearable currently brownouts at BLE radio start. Keep the
// local recognizer and USB EVENT stream alive for the live demo.
constexpr bool kEnableBleRadio = false;

constexpr uint8_t kRegSampleRateDivider = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegGyroConfig = 0x1B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelXoutHigh = 0x3B;
constexpr uint8_t kRegPowerManagement1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;

constexpr char kServiceUuid[] = "2f6f1000-8d0a-4e3d-bbc6-9f536a6ed001";
constexpr char kEventUuid[] = "2f6f1001-8d0a-4e3d-bbc6-9f536a6ed001";
constexpr char kSnapshotUuid[] = "2f6f1002-8d0a-4e3d-bbc6-9f536a6ed001";
constexpr char kCommandUuid[] = "2f6f1003-8d0a-4e3d-bbc6-9f536a6ed001";

enum class PendingCommand : uint8_t {
  kNone,
  kSync,
  kCalibrate,
  kResetSession,
  kPressureTest,
};

Preferences preferences;
pecky::RecognitionEngine recognitionEngine;
pecky::CalibrationProfile calibrationProfile;

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* eventCharacteristic = nullptr;
NimBLECharacteristic* snapshotCharacteristic = nullptr;
NimBLECharacteristic* commandCharacteristic = nullptr;
volatile bool bleConnected = false;
volatile bool connectionChanged = false;
volatile PendingCommand pendingCommand = PendingCommand::kNone;

uint32_t eventSequence = 0;
uint32_t lifetimeReps = 0;
uint32_t sessionId = 0;
uint32_t sessionReps = 0;
uint32_t nextSampleUs = 0;
uint32_t lastProgressMs = 0;
bool imuReady = false;
bool calibrated = false;
bool recognitionEnabled = false;

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
  for (size_t index = 0; index < length; ++index) buffer[index] = Wire.read();
  return true;
}

bool initializeImu() {
  uint8_t whoAmI = 0;
  if (!readRegisters(kRegWhoAmI, &whoAmI, 1)) {
    Serial.println("ERROR,WHO_AM_I_READ_FAILED");
    return false;
  }
  Serial.printf("INFO,WHO_AM_I,0x%02X\n", whoAmI);
  if (whoAmI != 0x68 && whoAmI != 0x69 && whoAmI != 0x70) {
    Serial.println("ERROR,UNEXPECTED_WHO_AM_I");
    return false;
  }
  bool ok = true;
  ok &= writeRegister(kRegPowerManagement1, 0x00);
  delay(100);
  ok &= writeRegister(kRegConfig, 0x03);             // DLPF about 44 Hz.
  ok &= writeRegister(kRegSampleRateDivider, 0x27);  // 1 kHz / 40 = 25 Hz.
  ok &= writeRegister(kRegGyroConfig, 0x00);         // +/-250 dps.
  ok &= writeRegister(kRegAccelConfig, 0x00);        // +/-2 g.
  if (!ok) Serial.println("ERROR,MPU_CONFIG_WRITE_FAILED");
  return ok;
}

uint16_t readPressureRaw() {
  uint32_t sum = 0;
  for (int index = 0; index < 4; ++index) sum += analogRead(kPressurePin);
  return static_cast<uint16_t>(sum / 4);
}

bool readSensorFrame(pecky::SensorFrame& frame) {
  uint8_t data[14];
  if (!readRegisters(kRegAccelXoutHigh, data, sizeof(data))) return false;
  const int16_t ax = makeInt16(data[0], data[1]);
  const int16_t ay = makeInt16(data[2], data[3]);
  const int16_t az = makeInt16(data[4], data[5]);
  const int16_t gx = makeInt16(data[8], data[9]);
  const int16_t gy = makeInt16(data[10], data[11]);
  const int16_t gz = makeInt16(data[12], data[13]);
  frame.timeMs = millis();
  frame.axG = ax / 16384.0f;
  frame.ayG = ay / 16384.0f;
  frame.azG = az / 16384.0f;
  frame.gxDps = gx / 131.0f;
  frame.gyDps = gy / 131.0f;
  frame.gzDps = gz / 131.0f;
  frame.pressureAdc = readPressureRaw();
  return true;
}

void sortFloats(float* values, size_t count) {
  for (size_t index = 1; index < count; ++index) {
    const float value = values[index];
    size_t position = index;
    while (position > 0 && values[position - 1] > value) {
      values[position] = values[position - 1];
      --position;
    }
    values[position] = value;
  }
}

float medianOf(float* values, size_t count) {
  sortFloats(values, count);
  return count % 2 == 0 ? 0.5f * (values[count / 2 - 1] + values[count / 2])
                        : values[count / 2];
}

bool calibrateAtNeutral() {
  constexpr size_t kCalibrationSamples = 75;
  pecky::SensorFrame samples[kCalibrationSamples];
  size_t valid = 0;
  Serial.println("INFO,CALIBRATION_START,KEEP_HEAD_NEUTRAL_STILL_AND_DO_NOT_PRESS_PAD");
  while (valid < kCalibrationSamples) {
    if (readSensorFrame(samples[valid])) ++valid;
    delay(kSamplePeriodUs / 1000);
  }

  pecky::CalibrationProfile profile;
  float pressureValues[kCalibrationSamples];
  float accelerationMagnitudeMean = 0.0f;
  for (size_t index = 0; index < kCalibrationSamples; ++index) {
    const auto& sample = samples[index];
    profile.neutralAxG += sample.axG;
    profile.neutralAyG += sample.ayG;
    profile.neutralAzG += sample.azG;
    profile.gyroBiasXDps += sample.gxDps;
    profile.gyroBiasYDps += sample.gyDps;
    profile.gyroBiasZDps += sample.gzDps;
    pressureValues[index] = sample.pressureAdc;
    accelerationMagnitudeMean +=
        pecky::vectorNorm(sample.axG, sample.ayG, sample.azG);
  }
  const float inverseCount = 1.0f / kCalibrationSamples;
  profile.neutralAxG *= inverseCount;
  profile.neutralAyG *= inverseCount;
  profile.neutralAzG *= inverseCount;
  profile.gyroBiasXDps *= inverseCount;
  profile.gyroBiasYDps *= inverseCount;
  profile.gyroBiasZDps *= inverseCount;
  accelerationMagnitudeMean *= inverseCount;
  profile.pressureBaseline = medianOf(pressureValues, kCalibrationSamples);

  float pressureDeviations[kCalibrationSamples];
  float accelerationVariance = 0.0f;
  float gyroMotionEnergy = 0.0f;
  for (size_t index = 0; index < kCalibrationSamples; ++index) {
    const auto& sample = samples[index];
    pressureDeviations[index] = fabsf(sample.pressureAdc - profile.pressureBaseline);
    const float accelerationMagnitude =
        pecky::vectorNorm(sample.axG, sample.ayG, sample.azG);
    accelerationVariance += pecky::square(accelerationMagnitude - accelerationMagnitudeMean);
    gyroMotionEnergy +=
        pecky::square(sample.gxDps - profile.gyroBiasXDps) +
        pecky::square(sample.gyDps - profile.gyroBiasYDps) +
        pecky::square(sample.gzDps - profile.gyroBiasZDps);
  }
  profile.pressureNoise =
      fmaxf(1.0f, 1.4826f * medianOf(pressureDeviations, kCalibrationSamples));
  const float accelerationStd = sqrtf(accelerationVariance * inverseCount);
  const float gyroMotionRms = sqrtf(gyroMotionEnergy * inverseCount);
  // The installed MPU6500 has a stable resting gyro RMS around 12 dps. Keep
  // enough margin for that measured hardware noise while still rejecting a
  // genuinely moving calibration pose.
  if (accelerationStd > 0.05f || gyroMotionRms > 18.0f) {
    Serial.printf("ERROR,CALIBRATION_MOVED,ACC_STD=%.3f,GYRO_RMS=%.1f,RETRY\n",
                  accelerationStd, gyroMotionRms);
    return false;
  }

  // Fixed by the current physical mounting. If the final enclosure flips the
  // board, change this once and keep it identical for every wearer.
  profile.extensionSign = -1.0f;
  calibrationProfile = profile;
  recognitionEngine.begin(calibrationProfile);
  calibrated = true;
  Serial.printf(
      "INFO,CALIBRATION_DONE,ACC=%.3f/%.3f/%.3f,GYRO=%.2f/%.2f/%.2f,P0=%.1f,PN=%.1f\n",
      profile.neutralAxG, profile.neutralAyG, profile.neutralAzG,
      profile.gyroBiasXDps, profile.gyroBiasYDps, profile.gyroBiasZDps,
      profile.pressureBaseline, profile.pressureNoise);
  return true;
}

uint8_t currentGroupReps() {
  if (sessionReps == 0) return 0;
  return static_cast<uint8_t>((sessionReps - 1) % kGroupTarget + 1);
}

bool isBleConnected() {
  return bleConnected && bleServer != nullptr && bleServer->getConnectedCount() > 0;
}

void notifyValue(NimBLECharacteristic* characteristic, const char* payload) {
  if (characteristic == nullptr) return;
  characteristic->setValue(payload);
  if (isBleConnected()) characteristic->notify();
}

void publishSnapshot(bool notify) {
  if (snapshotCharacteristic == nullptr) return;
  const auto& status = recognitionEngine.status();
  char payload[180];
  snprintf(payload, sizeof(payload),
           "{\"v\":1,\"t\":\"z\",\"s\":%lu,\"q\":%lu,\"r\":%u,\"g\":%u,"
           "\"tr\":%lu,\"a\":%u,\"ph\":%u,\"pt\":%u,\"m\":%lu}",
           static_cast<unsigned long>(sessionId),
           static_cast<unsigned long>(eventSequence), currentGroupReps(),
           kGroupTarget, static_cast<unsigned long>(lifetimeReps), status.state,
           status.pressureHealthy ? 1 : 0,
           status.pressureTestActive ? 1 : 0,
           static_cast<unsigned long>(millis()));
  snapshotCharacteristic->setValue(payload);
  if (notify && isBleConnected()) snapshotCharacteristic->notify();
}

void publishProgress() {
  const auto& status = recognitionEngine.status();
  // The terminal bridge is the production path for this prototype. Emit the
  // recognizer state over USB when radio advertising is intentionally off.
  if (!kEnableBleRadio) {
    Serial.printf("PROGRESS,candidate=%u,hold=%lu,state=%u,neutral=%u,pressure=%u\n",
                  static_cast<unsigned>(status.candidate),
                  static_cast<unsigned long>(status.candidateDurationMs),
                  static_cast<unsigned>(status.state),
                  status.neutralStable ? 1U : 0U,
                  status.pressureHealthy ? 1U : 0U);
    return;
  }
  if (!isBleConnected()) return;
  char payload[180];
  snprintf(payload, sizeof(payload),
           "{\"v\":1,\"t\":\"p\",\"s\":%lu,\"q\":%lu,\"r\":%u,\"g\":%u,"
           "\"c\":%u,\"h\":%lu,\"a\":%u,\"pt\":%u,\"m\":%lu}",
           static_cast<unsigned long>(sessionId),
           static_cast<unsigned long>(eventSequence), currentGroupReps(),
           kGroupTarget, static_cast<unsigned>(status.candidate),
           static_cast<unsigned long>(status.candidateDurationMs), status.state,
           status.pressureTestActive ? 1 : 0,
           static_cast<unsigned long>(millis()));
  notifyValue(eventCharacteristic, payload);
}

void persistCounters() {
  preferences.putUInt("event_seq", eventSequence);
  preferences.putUInt("total_reps", lifetimeReps);
  preferences.putUInt("session_id", sessionId);
  preferences.putUInt("session_reps", sessionReps);
}

void publishRecognition(const pecky::RecognitionEvent& event) {
  ++eventSequence;
  ++lifetimeReps;
  ++sessionReps;
  persistCounters();  // Durable truth is saved before any best-effort Notify.

  const uint8_t groupReps = currentGroupReps();
  char payload[180];
  snprintf(payload, sizeof(payload),
           "{\"v\":1,\"t\":\"r\",\"s\":%lu,\"q\":%lu,\"r\":%u,\"g\":%u,"
           "\"c\":%u,\"h\":%lu,\"cf\":%u,\"m\":%lu}",
           static_cast<unsigned long>(sessionId),
           static_cast<unsigned long>(eventSequence), groupReps, kGroupTarget,
           static_cast<unsigned>(event.action),
           static_cast<unsigned long>(event.durationMs),
           static_cast<unsigned>(event.confidence * 100.0f),
           static_cast<unsigned long>(millis()));
  const char* actionZh = "未知动作";
  switch (static_cast<unsigned>(event.action)) {
    case 1: actionZh = "后仰脖子"; break;
    case 2: actionZh = "收下巴"; break;
    case 3: actionZh = "抱头抗阻"; break;
  }
  Serial.printf("识别成功：%s｜第%lu次｜置信度%u%%｜持续%lums\n",
                actionZh, static_cast<unsigned long>(lifetimeReps),
                static_cast<unsigned>(event.confidence * 100.0f),
                static_cast<unsigned long>(event.durationMs));
  Serial.printf("EVENT,%s,%s\n", pecky::actionName(event.action), payload);
  notifyValue(eventCharacteristic, payload);

  if (groupReps == kGroupTarget) {
    ++eventSequence;
    preferences.putUInt("event_seq", eventSequence);
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"t\":\"x\",\"s\":%lu,\"q\":%lu,\"r\":%u,\"g\":%u,"
             "\"m\":%lu}",
             static_cast<unsigned long>(sessionId),
             static_cast<unsigned long>(eventSequence), groupReps, kGroupTarget,
             static_cast<unsigned long>(millis()));
    notifyValue(eventCharacteristic, payload);
  }
  publishSnapshot(true);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    bleConnected = true;
    connectionChanged = true;
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    bleConnected = false;
    connectionChanged = true;
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const String value(characteristic->getValue().c_str());
    if (value.length() == 0 || value.length() > 120) return;
    if (value.indexOf("\"cmd\":\"sync\"") >= 0) {
      pendingCommand = PendingCommand::kSync;
    } else if (value.indexOf("\"cmd\":\"calibrate\"") >= 0) {
      pendingCommand = PendingCommand::kCalibrate;
    } else if (value.indexOf("\"cmd\":\"reset_session\"") >= 0) {
      pendingCommand = PendingCommand::kResetSession;
    } else if (value.indexOf("\"cmd\":\"pressure_test\"") >= 0) {
      pendingCommand = PendingCommand::kPressureTest;
    }
  }
};

void initializeBle() {
  Serial.println("INFO,BLE_INIT_START");
  char deviceName[24];
  snprintf(deviceName, sizeof(deviceName), "Pecky-%04X",
           static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFF));
  NimBLEDevice::init(deviceName);
  Serial.println("INFO,BLE_STACK_READY");
  // The current wearable prototype only needs a sub-metre cap-to-phone link.
  // Keep advertising/connection TX power low to reduce radio load after the
  // controller has initialized. A brownout inside NimBLEDevice::init still
  // requires a stable 5 V/USB power path; software must not mask it.
  NimBLEDevice::setPower(-12);
  NimBLEDevice::setMTU(185);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  bleServer->advertiseOnDisconnect(true);

  NimBLEService* service = bleServer->createService(kServiceUuid);
  eventCharacteristic = service->createCharacteristic(
      kEventUuid, NIMBLE_PROPERTY::NOTIFY);
  snapshotCharacteristic = service->createCharacteristic(
      kSnapshotUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  commandCharacteristic = service->createCharacteristic(
      kCommandUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  commandCharacteristic->setCallbacks(new CommandCallbacks());
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  Serial.println("INFO,BLE_ADVERTISING_STARTED");
  publishSnapshot(false);
  Serial.printf("INFO,BLE_READY,%s\n", deviceName);
}

void handlePendingCommand() {
  const PendingCommand command = pendingCommand;
  pendingCommand = PendingCommand::kNone;
  switch (command) {
    case PendingCommand::kSync:
      publishSnapshot(true);
      break;
    case PendingCommand::kCalibrate:
      calibrated = false;
      if (!calibrateAtNeutral()) {
        Serial.println("ERROR,CALIBRATION_FAILED,KEEP_STILL_AND_SEND_CALIBRATE_AGAIN");
      }
      publishSnapshot(true);
      break;
    case PendingCommand::kResetSession:
      ++sessionId;
      sessionReps = 0;
      recognitionEngine.resetSessionRecognition();
      persistCounters();
      publishSnapshot(true);
      break;
    case PendingCommand::kPressureTest:
      recognitionEngine.startPressureSelfTest();
      Serial.println("INFO,PRESSURE_TEST,RELEASE_0_5S_THEN_PRESS_PAD_0_5S_WITHIN_8S");
      publishSnapshot(true);
      break;
    default:
      break;
  }
}

void printControlStatus() {
  Serial.printf("STATUS,RECOGNITION,%s,CALIBRATED=%u,BLE=%u\n",
                recognitionEnabled ? "RUNNING" : "PAUSED", calibrated ? 1U : 0U,
                bleConnected ? 1U : 0U);
}

void handleSerialCommand(String command) {
  command.trim();
  command.toUpperCase();
  if (command == "START") {
    recognitionEnabled = true;
    recognitionEngine.resetSessionRecognition();
    Serial.println("CONTROL,STARTED");
  } else if (command == "PAUSE") {
    recognitionEnabled = false;
    recognitionEngine.resetSessionRecognition();
    Serial.println("CONTROL,PAUSED");
  } else if (command == "STATUS") {
    printControlStatus();
  } else if (command == "CALIBRATE") {
    recognitionEnabled = false;
    calibrated = false;
    const bool ok = calibrateAtNeutral();
    Serial.printf("CONTROL,CALIBRATE,%s\n", ok ? "DONE" : "FAILED");
  } else if (command.length() > 0) {
    Serial.println("ERROR,CONTROL,USE_START_PAUSE_STATUS_OR_CALIBRATE");
  }
}

void serviceSerialCommands() {
  static String command;
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\n' || value == '\r') {
      if (command.length() > 0) handleSerialCommand(command);
      command = "";
    } else if (command.length() < 32) {
      command += value;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(700);
  // 80 MHz is ample for a 25 Hz state machine and leaves more supply margin
  // for the BLE radio on the current prototype board.
  if (!setCpuFrequencyMhz(80)) Serial.println("WARN,CPU_FREQUENCY_NOT_CHANGED");
  Serial.println();
  Serial.println("INFO,PECKY_THREE_RECOGNIZERS_BLE_V1");
  Serial.printf("INFO,PINS,SDA=%d,SCL=%d,PRESSURE_ADC=%d\n", kSdaPin, kSclPin,
                kPressurePin);

  analogReadResolution(12);
  analogSetPinAttenuation(kPressurePin, ADC_11db);
  pinMode(kPressurePin, INPUT);
  Wire.begin(kSdaPin, kSclPin, 400000);
  Wire.setTimeOut(50);

  imuReady = initializeImu();
  if (!imuReady) {
    Serial.println("ERROR,IMU_STARTUP_FAILED");
    return;
  }
  while (!calibrateAtNeutral()) delay(1000);

  Serial.println("INFO,NVS_INIT_START");
  preferences.begin("peckyrec", false);
  eventSequence = preferences.getUInt("event_seq", 0);
  lifetimeReps = preferences.getUInt("total_reps", 0);
  sessionId = preferences.getUInt("session_id", 0) + 1;
  sessionReps = 0;
  persistCounters();
  Serial.println("INFO,NVS_INIT_DONE");
  if (kEnableBleRadio) initializeBle();
  else Serial.println("INFO,USB_EVENT_MODE_READY");
  Serial.println("CONTROL,PAUSED,SEND_START_WHEN_READY");
  nextSampleUs = micros() + kSamplePeriodUs;
}

void loop() {
  // Process BLE commands even after a failed re-calibration, otherwise the
  // documented "send calibrate again" recovery path would be unreachable.
  handlePendingCommand();
  serviceSerialCommands();
  if (!recognitionEnabled) {
    delay(10);
    return;
  }
  if (!imuReady || !calibrated) {
    delay(20);
    return;
  }
  if (connectionChanged) {
    connectionChanged = false;
    if (bleConnected) publishSnapshot(true);
  }

  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - nextSampleUs) < 0) return;
  if (static_cast<int32_t>(nowUs - nextSampleUs) >=
      static_cast<int32_t>(kSamplePeriodUs)) {
    nextSampleUs = nowUs;
  }
  nextSampleUs += kSamplePeriodUs;

  pecky::SensorFrame frame;
  if (!readSensorFrame(frame)) {
    Serial.println("ERROR,MPU_READ_FAILED");
    return;
  }
  const pecky::RecognitionEvent event = recognitionEngine.update(frame);
  if (event.valid) publishRecognition(event);
  if (frame.timeMs - lastProgressMs >= kProgressPeriodMs) {
    publishProgress();
    lastProgressMs = frame.timeMs;
  }
}
