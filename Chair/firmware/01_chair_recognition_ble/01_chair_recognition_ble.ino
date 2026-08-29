/*
 * Qingxian Chair — local recognizer + BLE action events.
 *
 * The recognizer calibrates its five range channels while the user remains in
 * neutral posture, then looks for sustained inward movement at the left,
 * right, or centre channel group. This is a real, sensor-driven classifier:
 * no serial command or timer can generate an action event. Tune the thresholds
 * below only after collecting representative data for the installed chair.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <math.h>

namespace {
constexpr uint32_t kBaud = 115200;
constexpr uint8_t kTrig[] = {4, 6, 10, 15, 17};
constexpr uint8_t kEcho[] = {5, 7, 11, 16, 18};
constexpr uint8_t kMpuSda = 8, kMpuScl = 9;
constexpr uint32_t kFrameMs = 180, kCalibrationMs = 3500, kCooldownMs = 1800;
constexpr float kMovementCm = 11.0f;
constexpr char kServiceUuid[] = "2f6f2000-8d0a-4e3d-bbc6-9f536a6ed001";
constexpr char kEventUuid[] = "2f6f2001-8d0a-4e3d-bbc6-9f536a6ed001";

NimBLECharacteristic* eventCharacteristic = nullptr;
float baseline[5] = {NAN, NAN, NAN, NAN, NAN};
uint16_t baselineCount[5] = {};
uint32_t calibrationStarted = 0, nextFrame = 0, lastEvent = 0, sequence = 0;
uint8_t candidate = 0, candidateFrames = 0;
bool recognitionEnabled = false;

float readDistance(uint8_t index) {
  digitalWrite(kTrig[index], LOW); delayMicroseconds(3);
  digitalWrite(kTrig[index], HIGH); delayMicroseconds(10); digitalWrite(kTrig[index], LOW);
  const uint32_t duration = pulseIn(kEcho[index], HIGH, 9000);
  if (!duration) return NAN;
  const float cm = duration * 0.01715f;
  return cm >= 2 && cm <= 150 ? cm : NAN;
}

void publish(uint8_t code) {
  ++sequence;
  char message[48];
  snprintf(message, sizeof(message), "{\"v\":1,\"t\":\"a\",\"q\":%lu,\"c\":%u}", static_cast<unsigned long>(sequence), code);
  eventCharacteristic->setValue(message);
  eventCharacteristic->notify();
  Serial.printf("ACTION,%lu,%u\n", static_cast<unsigned long>(sequence), code);
}

void calibrate(const float* ranges) {
  for (uint8_t i = 0; i < 5; ++i) {
    if (!isfinite(ranges[i])) continue;
    ++baselineCount[i];
    baseline[i] = !isfinite(baseline[i]) ? ranges[i] : baseline[i] + (ranges[i] - baseline[i]) / baselineCount[i];
  }
}

uint8_t classify(const float* ranges) {
  // A closer torso/arm produces a positive inward movement value.
  const float left = (isfinite(ranges[0]) && isfinite(baseline[0]) ? baseline[0] - ranges[0] : 0) +
                     (isfinite(ranges[1]) && isfinite(baseline[1]) ? baseline[1] - ranges[1] : 0);
  const float right = (isfinite(ranges[3]) && isfinite(baseline[3]) ? baseline[3] - ranges[3] : 0) +
                      (isfinite(ranges[4]) && isfinite(baseline[4]) ? baseline[4] - ranges[4] : 0);
  const float centre = isfinite(ranges[2]) && isfinite(baseline[2]) ? baseline[2] - ranges[2] : 0;
  if (centre >= kMovementCm && centre > left * .62f && centre > right * .62f) return 3;
  if (left >= kMovementCm && left > right * 1.20f) return 1;
  if (right >= kMovementCm && right > left * 1.20f) return 2;
  return 0;
}

void setupBle() {
  NimBLEDevice::init("Qingxian-Chair");
  NimBLEDevice::setPower(-12);
  NimBLEServer* server = NimBLEDevice::createServer();
  // Keep the chair discoverable after every app disconnect; recognition pause
  // affects only action events, never the availability of the BLE service.
  server->advertiseOnDisconnect(true);
  NimBLEService* service = server->createService(kServiceUuid);
  eventCharacteristic = service->createCharacteristic(kEventUuid, NIMBLE_PROPERTY::NOTIFY);
  service->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid); advertising->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
}

void serviceSerialCommands() {
  static String command;
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value != '\n' && value != '\r') { if (command.length() < 32) command += value; continue; }
    command.trim(); command.toUpperCase();
    if (command == "START") { recognitionEnabled = true; candidate = candidateFrames = 0; Serial.println("CONTROL,STARTED"); }
    else if (command == "PAUSE") { recognitionEnabled = false; candidate = candidateFrames = 0; Serial.println("CONTROL,PAUSED"); }
    else if (command == "STATUS") Serial.printf("STATUS,RECOGNITION,%s,CALIBRATED=%u\n", recognitionEnabled ? "RUNNING" : "PAUSED", millis() - calibrationStarted >= kCalibrationMs ? 1U : 0U);
    else if (command == "CALIBRATE") { calibrationStarted = millis(); for (uint8_t i = 0; i < 5; ++i) { baseline[i] = NAN; baselineCount[i] = 0; } recognitionEnabled = false; Serial.println("CONTROL,CALIBRATING"); }
    else if (command.length() > 0) Serial.println("ERROR,CONTROL,USE_START_PAUSE_STATUS_OR_CALIBRATE");
    command = "";
  }
}
}  // namespace

void setup() {
  Serial.begin(kBaud); delay(500);
  for (uint8_t i = 0; i < 5; ++i) { pinMode(kTrig[i], OUTPUT); digitalWrite(kTrig[i], LOW); pinMode(kEcho[i], INPUT); }
  Wire.begin(kMpuSda, kMpuScl, 400000);  // Reserved for the next fused model; range recognition works without it.
  setupBle(); calibrationStarted = millis();
  Serial.println("READY,CHAIR_RECOGNITION_BLE_V1,KEEP_NEUTRAL_FOR_3_5_SECONDS");
  Serial.println("CONTROL,PAUSED,SEND_START_WHEN_READY");
}

void loop() {
  serviceSerialCommands();
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextFrame) < 0) return;
  nextFrame = now + kFrameMs;
  float ranges[5];
  for (uint8_t i = 0; i < 5; ++i) { ranges[i] = readDistance(i); delay(30); }
  if (now - calibrationStarted < kCalibrationMs) { calibrate(ranges); return; }
  if (!recognitionEnabled) return;
  const uint8_t detected = classify(ranges);
  if (detected == candidate) ++candidateFrames; else { candidate = detected; candidateFrames = detected ? 1 : 0; }
  if (candidate && candidateFrames >= 2 && now - lastEvent >= kCooldownMs) { publish(candidate); lastEvent = now; candidateFrames = 0; }
  // Adapt very slowly only while neutral, compensating for sensor drift without
  // absorbing a held exercise posture into the baseline.
  if (!detected) for (uint8_t i = 0; i < 5; ++i) if (isfinite(ranges[i]) && isfinite(baseline[i])) baseline[i] = baseline[i] * .995f + ranges[i] * .005f;
}
