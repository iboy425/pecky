/*
 * Qingxian Chair — calibrated local recognizer + BLE action events.
 *
 * The installed chair has one consistently useful range channel (HC4) plus a
 * working MPU-6050. The recognizer waits for a deliberate movement, records
 * its HC4 distance peak, then emits exactly one action after the movement
 * settles. It is re-armed only after returning to neutral.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <math.h>

namespace {
constexpr uint32_t kBaud = 115200;
constexpr uint8_t kTrig[] = {4, 6, 10, 15, 17};
constexpr uint8_t kEcho[] = {5, 7, 11, 16, 18};
constexpr uint8_t kMpuSda = 8, kMpuScl = 9, kMpuAddress = 0x68;
constexpr uint32_t kFrameMs = 180, kCalibrationMs = 3500;
constexpr uint32_t kMinimumActionMs = 450, kMaximumActionMs = 2600, kNeutralRearmMs = 850;
// Derived from LATEST_20260830: HC4 neutral≈9 cm, left≈15–20, right≈25–33.
constexpr float kStartMovementDps = 1.6f, kSettleMovementDps = 0.9f;
constexpr float kNeutralDistanceCm = 2.5f, kLeftDistanceCm = 4.0f, kRightDistanceCm = 13.0f;
constexpr char kServiceUuid[] = "2f6f2000-8d0a-4e3d-bbc6-9f536a6ed001";
constexpr char kEventUuid[] = "2f6f2001-8d0a-4e3d-bbc6-9f536a6ed001";

enum class RecognitionPhase : uint8_t { kWaiting, kTracking, kNeedsNeutral };
NimBLECharacteristic* eventCharacteristic = nullptr;
float baselineRange[5] = {NAN, NAN, NAN, NAN, NAN};
float baselineGyro[3] = {0, 0, 0};
uint16_t baselineRangeCount[5] = {}, baselineGyroCount = 0;
uint32_t calibrationStarted = 0, nextFrame = 0, sequence = 0, actionStarted = 0, neutralStarted = 0;
float peakHc4Delta = 0;
bool recognitionEnabled = false;
RecognitionPhase phase = RecognitionPhase::kWaiting;

float readDistance(uint8_t index) {
  digitalWrite(kTrig[index], LOW); delayMicroseconds(3);
  digitalWrite(kTrig[index], HIGH); delayMicroseconds(10); digitalWrite(kTrig[index], LOW);
  const uint32_t duration = pulseIn(kEcho[index], HIGH, 9000);
  if (!duration) return NAN;
  const float cm = duration * 0.01715f;
  return cm >= 2 && cm <= 150 ? cm : NAN;
}

bool readGyro(float* gyro) {
  Wire.beginTransmission(kMpuAddress); Wire.write(0x43);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(kMpuAddress, static_cast<uint8_t>(6), true) != 6) return false;
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(Wire.read()) << 8) | Wire.read());
    gyro[i] = raw / 131.0f;
  }
  return true;
}

void publish(uint8_t code) {
  ++sequence;
  char message[48];
  snprintf(message, sizeof(message), "{\"v\":1,\"t\":\"a\",\"q\":%lu,\"c\":%u}", static_cast<unsigned long>(sequence), code);
  eventCharacteristic->setValue(message); eventCharacteristic->notify();
  Serial.printf("ACTION,%lu,%u\n", static_cast<unsigned long>(sequence), code);
}

void resetRecognition() { phase = RecognitionPhase::kWaiting; actionStarted = neutralStarted = 0; peakHc4Delta = 0; }

void calibrate(const float* ranges, const float* gyro, bool gyroOk) {
  for (uint8_t i = 0; i < 5; ++i) if (isfinite(ranges[i])) {
    ++baselineRangeCount[i];
    baselineRange[i] = !isfinite(baselineRange[i]) ? ranges[i] : baselineRange[i] + (ranges[i] - baselineRange[i]) / baselineRangeCount[i];
  }
  if (gyroOk) { ++baselineGyroCount; for (uint8_t i = 0; i < 3; ++i) baselineGyro[i] += (gyro[i] - baselineGyro[i]) / baselineGyroCount; }
}

float gyroDeltaDps(const float* gyro, bool gyroOk) {
  if (!gyroOk || baselineGyroCount == 0) return 0;
  float sum = 0; for (uint8_t i = 0; i < 3; ++i) sum += sq(gyro[i] - baselineGyro[i]);
  return sqrtf(sum);
}

float hc4DeltaCm(const float* ranges) { return isfinite(ranges[3]) && isfinite(baselineRange[3]) ? ranges[3] - baselineRange[3] : 0; }
uint8_t actionForPeak(float peakDelta) { return peakDelta >= kRightDistanceCm ? 2 : peakDelta >= kLeftDistanceCm ? 1 : 3; }

void updateRecognition(uint32_t now, const float* ranges, const float* gyro, bool gyroOk) {
  const float movement = gyroDeltaDps(gyro, gyroOk), hc4Delta = hc4DeltaCm(ranges);
  const bool neutral = fabsf(hc4Delta) < kNeutralDistanceCm && movement <= kSettleMovementDps;
  if (phase == RecognitionPhase::kNeedsNeutral) {
    if (!neutral) { neutralStarted = 0; return; }
    if (!neutralStarted) neutralStarted = now;
    if (now - neutralStarted >= kNeutralRearmMs) resetRecognition();
    return;
  }
  if (phase == RecognitionPhase::kWaiting) {
    if (movement >= kStartMovementDps || hc4Delta >= kLeftDistanceCm) {
      phase = RecognitionPhase::kTracking; actionStarted = now; peakHc4Delta = max(0.0f, hc4Delta);
      Serial.printf("PROGRESS,MOTION_STARTED,HC4_DELTA=%.2f,GYRO_DELTA=%.2f\n", hc4Delta, movement);
    }
    return;
  }
  peakHc4Delta = max(peakHc4Delta, hc4Delta);
  const uint32_t elapsed = now - actionStarted;
  if ((elapsed >= kMinimumActionMs && movement <= kSettleMovementDps) || elapsed >= kMaximumActionMs) {
    const uint8_t action = actionForPeak(peakHc4Delta);
    publish(action);
    Serial.printf("PROGRESS,ACTION_DONE,CODE=%u,HC4_PEAK=%.2f,ELAPSED_MS=%lu\n", action, peakHc4Delta, static_cast<unsigned long>(elapsed));
    phase = RecognitionPhase::kNeedsNeutral; neutralStarted = 0;
  }
}

void setupBle() {
  NimBLEDevice::init("Qingxian-Chair"); NimBLEDevice::setPower(-12);
  NimBLEServer* server = NimBLEDevice::createServer(); server->advertiseOnDisconnect(true);
  NimBLEService* service = server->createService(kServiceUuid);
  eventCharacteristic = service->createCharacteristic(kEventUuid, NIMBLE_PROPERTY::NOTIFY); service->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid); advertising->enableScanResponse(true); NimBLEDevice::startAdvertising();
}

void beginCalibration() {
  calibrationStarted = millis(); baselineGyroCount = 0;
  for (uint8_t i = 0; i < 5; ++i) { baselineRange[i] = NAN; baselineRangeCount[i] = 0; }
  for (uint8_t i = 0; i < 3; ++i) baselineGyro[i] = 0;
  resetRecognition(); recognitionEnabled = false;
  Serial.println("CONTROL,CALIBRATING,KEEP_NEUTRAL_FOR_3_5_SECONDS");
}

void serviceSerialCommands() {
  static String command;
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value != '\n' && value != '\r') { if (command.length() < 32) command += value; continue; }
    command.trim(); command.toUpperCase();
    if (command == "START") { recognitionEnabled = true; resetRecognition(); Serial.println("CONTROL,STARTED"); }
    else if (command == "PAUSE") { recognitionEnabled = false; resetRecognition(); Serial.println("CONTROL,PAUSED"); }
    else if (command == "STATUS") Serial.printf("STATUS,RECOGNITION,%s,CALIBRATED=%u,MODEL=HC4_MPU_ACTION_COMPLETE\n", recognitionEnabled ? "RUNNING" : "PAUSED", millis() - calibrationStarted >= kCalibrationMs ? 1U : 0U);
    else if (command == "CALIBRATE") beginCalibration();
    else if (command.length() > 0) Serial.println("ERROR,CONTROL,USE_START_PAUSE_STATUS_OR_CALIBRATE");
    command = "";
  }
}
}  // namespace

void setup() {
  Serial.begin(kBaud); delay(500);
  for (uint8_t i = 0; i < 5; ++i) { pinMode(kTrig[i], OUTPUT); digitalWrite(kTrig[i], LOW); pinMode(kEcho[i], INPUT); }
  Wire.begin(kMpuSda, kMpuScl, 400000);
  Wire.beginTransmission(kMpuAddress); Wire.write(0x6B); Wire.write(0); Wire.endTransmission(true);
  setupBle(); beginCalibration();
  Serial.println("READY,CHAIR_RECOGNITION_BLE_V2,MODEL=HC4_MPU_ACTION_COMPLETE");
}

void loop() {
  serviceSerialCommands();
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextFrame) < 0) return;
  nextFrame = now + kFrameMs;
  float ranges[5], gyro[3];
  for (uint8_t i = 0; i < 5; ++i) { ranges[i] = readDistance(i); delay(30); }
  const bool gyroOk = readGyro(gyro);
  if (now - calibrationStarted < kCalibrationMs) { calibrate(ranges, gyro, gyroOk); return; }
  if (recognitionEnabled) updateRecognition(now, ranges, gyro, gyroOk);
}
