/*
 * Qingxian Chair — calibrated local recognizer + BLE action events.
 *
 * The installed chair has one consistently useful range channel (HC4) plus a
 * working MPU-6050. The recognizer waits for a deliberate movement, records
 * its HC4 distance peak, then emits exactly one action after the movement
 * returns and settles. It then re-arms from the sitter's current neutral pose.
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
constexpr uint32_t kMinimumActionMs = 650, kMaximumActionMs = 5000;
constexpr uint32_t kSettleConfirmMs = 320, kNeutralRearmMs = 700;
// Derived from LATEST_20260830: seated neutral≈9 cm, left≈15–20,
// right≈25–33. START recalibrates this baseline for the current sitter.
constexpr float kStartMovementDps = 1.8f, kSettleMovementDps = 1.4f;
constexpr float kStartTiltG = 0.025f;
constexpr float kStrongMovementDps = 3.0f, kStrongTiltG = 0.022f;
constexpr float kRangeMovementDps = 0.8f, kRangeTiltG = 0.012f;
constexpr float kLeftDistanceCm = 3.0f;
// Both side directions are intentionally one "stretch" action. The installed
// chair trace shows side movement on accel-Y, supported by gyro-Z or HC4;
// symmetric chest extension keeps accel-Y much closer to zero.
constexpr float kSideAccelY = 0.018f, kSideGyroZ = 3.2f;
constexpr float kRangeBackedSideAccelY = 0.015f, kRangeBackedSideCm = 5.0f;
constexpr uint8_t kMaxCalibrationSamples = 24;
constexpr char kServiceUuid[] = "2f6f2000-8d0a-4e3d-bbc6-9f536a6ed001";
constexpr char kEventUuid[] = "2f6f2001-8d0a-4e3d-bbc6-9f536a6ed001";

enum class RecognitionPhase : uint8_t { kWaiting, kTracking, kNeedsNeutral };
NimBLECharacteristic* eventCharacteristic = nullptr;
float baselineRange[5] = {NAN, NAN, NAN, NAN, NAN};
float calibrationRangeSamples[5][kMaxCalibrationSamples] = {};
float baselineAccel[3] = {0, 0, 0};
float baselineGyro[3] = {0, 0, 0};
uint16_t baselineRangeCount[5] = {}, baselineGyroCount = 0;
uint32_t calibrationStarted = 0, nextFrame = 0, sequence = 0, actionStarted = 0;
uint32_t neutralStarted = 0, settleStarted = 0, lastSensorLog = 0;
float peakHc4Delta = 0, peakTiltDelta = 0;
float minAccelDelta[3] = {}, maxAccelDelta[3] = {}, minGyroDelta[3] = {}, maxGyroDelta[3] = {};
bool recognitionEnabled = false, calibrationFinalized = false;
bool outwardSettled = false, returnMovementSeen = false, hc2Appeared = false;
uint8_t startEvidenceFrames = 0;
RecognitionPhase phase = RecognitionPhase::kWaiting;

float readDistance(uint8_t index) {
  digitalWrite(kTrig[index], LOW); delayMicroseconds(3);
  digitalWrite(kTrig[index], HIGH); delayMicroseconds(10); digitalWrite(kTrig[index], LOW);
  const uint32_t duration = pulseIn(kEcho[index], HIGH, 9000);
  if (!duration) return NAN;
  const float cm = duration * 0.01715f;
  return cm >= 2 && cm <= 150 ? cm : NAN;
}

float readStableDistance(uint8_t index) {
  if (index != 3) return readDistance(index);
  float samples[3];
  uint8_t valid = 0;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    const float sample = readDistance(index);
    if (isfinite(sample)) samples[valid++] = sample;
    if (attempt < 2) delay(12);
  }
  if (!valid) return NAN;
  for (uint8_t i = 1; i < valid; ++i) {
    const float value = samples[i];
    uint8_t position = i;
    while (position && samples[position - 1] > value) {
      samples[position] = samples[position - 1];
      --position;
    }
    samples[position] = value;
  }
  return samples[valid / 2];
}

bool readMotion(float* accel, float* gyro) {
  Wire.beginTransmission(kMpuAddress); Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(kMpuAddress, static_cast<uint8_t>(14), true) != 14) return false;
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(Wire.read()) << 8) | Wire.read());
    accel[i] = raw / 16384.0f;
  }
  Wire.read(); Wire.read();  // temperature
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

void resetRecognition() {
  phase = RecognitionPhase::kWaiting;
  actionStarted = neutralStarted = settleStarted = 0;
  peakHc4Delta = peakTiltDelta = 0;
  outwardSettled = returnMovementSeen = hc2Appeared = false;
  startEvidenceFrames = 0;
}

void calibrate(const float* ranges, const float* accel, const float* gyro, bool motionOk) {
  for (uint8_t i = 0; i < 5; ++i) if (isfinite(ranges[i])) {
    if (baselineRangeCount[i] < kMaxCalibrationSamples) {
      calibrationRangeSamples[i][baselineRangeCount[i]++] = ranges[i];
    }
  }
  if (motionOk) {
    ++baselineGyroCount;
    for (uint8_t i = 0; i < 3; ++i) {
      baselineAccel[i] += (accel[i] - baselineAccel[i]) / baselineGyroCount;
      baselineGyro[i] += (gyro[i] - baselineGyro[i]) / baselineGyroCount;
    }
  }
}

void finishCalibration() {
  for (uint8_t channel = 0; channel < 5; ++channel) {
    const uint8_t count = baselineRangeCount[channel] > kMaxCalibrationSamples
                              ? kMaxCalibrationSamples
                              : static_cast<uint8_t>(baselineRangeCount[channel]);
    for (uint8_t i = 1; i < count; ++i) {
      const float value = calibrationRangeSamples[channel][i];
      uint8_t position = i;
      while (position && calibrationRangeSamples[channel][position - 1] > value) {
        calibrationRangeSamples[channel][position] = calibrationRangeSamples[channel][position - 1];
        --position;
      }
      calibrationRangeSamples[channel][position] = value;
    }
    baselineRange[channel] = count ? calibrationRangeSamples[channel][count / 2] : NAN;
  }
  calibrationFinalized = true;
  Serial.printf("PROGRESS,CALIBRATION_DONE,HC_BASELINE=%.1f/%.1f/%.1f/%.1f/%.1f\n",
                baselineRange[0], baselineRange[1], baselineRange[2], baselineRange[3], baselineRange[4]);
}

float gyroDeltaDps(const float* gyro, bool gyroOk) {
  if (!gyroOk || baselineGyroCount == 0) return 0;
  float sum = 0; for (uint8_t i = 0; i < 3; ++i) sum += sq(gyro[i] - baselineGyro[i]);
  return sqrtf(sum);
}

float accelDeltaG(const float* accel, bool motionOk) {
  if (!motionOk || baselineGyroCount == 0) return 0;
  float sum = 0;
  for (uint8_t i = 0; i < 3; ++i) sum += sq(accel[i] - baselineAccel[i]);
  return sqrtf(sum);
}

float hc4DeltaCm(const float* ranges) { return isfinite(ranges[3]) && isfinite(baselineRange[3]) ? ranges[3] - baselineRange[3] : 0; }

void beginMotionTrace(const float* ranges, const float* accel, const float* gyro, bool motionOk) {
  peakHc4Delta = max(0.0f, hc4DeltaCm(ranges));
  peakTiltDelta = accelDeltaG(accel, motionOk);
  hc2Appeared = !isfinite(baselineRange[1]) && isfinite(ranges[1]);
  for (uint8_t i = 0; i < 3; ++i) {
    const float accelDelta = motionOk ? accel[i] - baselineAccel[i] : 0;
    const float gyroDelta = motionOk ? gyro[i] - baselineGyro[i] : 0;
    minAccelDelta[i] = maxAccelDelta[i] = accelDelta;
    minGyroDelta[i] = maxGyroDelta[i] = gyroDelta;
  }
}

void updateMotionTrace(const float* ranges, const float* accel, const float* gyro, bool motionOk) {
  peakHc4Delta = max(peakHc4Delta, hc4DeltaCm(ranges));
  peakTiltDelta = max(peakTiltDelta, accelDeltaG(accel, motionOk));
  hc2Appeared = hc2Appeared || (!isfinite(baselineRange[1]) && isfinite(ranges[1]));
  if (!motionOk) return;
  for (uint8_t i = 0; i < 3; ++i) {
    const float accelDelta = accel[i] - baselineAccel[i], gyroDelta = gyro[i] - baselineGyro[i];
    minAccelDelta[i] = min(minAccelDelta[i], accelDelta);
    maxAccelDelta[i] = max(maxAccelDelta[i], accelDelta);
    minGyroDelta[i] = min(minGyroDelta[i], gyroDelta);
    maxGyroDelta[i] = max(maxGyroDelta[i], gyroDelta);
  }
}

uint8_t actionForTrace() {
  const float sideAccel = max(fabsf(minAccelDelta[1]), fabsf(maxAccelDelta[1]));
  const float sideGyro = max(fabsf(minGyroDelta[2]), fabsf(maxGyroDelta[2]));
  const bool directionalSide = sideAccel >= kSideAccelY && sideGyro >= kSideGyroZ;
  const bool rangeBackedSide = sideAccel >= kRangeBackedSideAccelY && peakHc4Delta >= kRangeBackedSideCm;
  const bool hc2BackedSide = hc2Appeared && sideAccel >= kRangeBackedSideAccelY;
  if (directionalSide || rangeBackedSide || hc2BackedSide) return 1;  // generic stretch
  return 3;
}

void finishAction(uint32_t now) {
  const uint8_t action = actionForTrace();
  publish(action);
  Serial.printf(
      "PROGRESS,ACTION_DONE,CODE=%u,HC4_PEAK=%.2f,TILT_PEAK=%.3f,AY_RANGE=%.3f/%.3f,GZ_RANGE=%.2f/%.2f,HC2_APPEARED=%u,ELAPSED_MS=%lu\n",
      action, peakHc4Delta, peakTiltDelta, minAccelDelta[1], maxAccelDelta[1], minGyroDelta[2], maxGyroDelta[2], hc2Appeared ? 1U : 0U,
      static_cast<unsigned long>(now - actionStarted));
  phase = RecognitionPhase::kNeedsNeutral;
  neutralStarted = settleStarted = 0;
}

void updateRecognition(uint32_t now, const float* ranges, const float* accel, const float* gyro, bool motionOk) {
  float movement = gyroDeltaDps(gyro, motionOk);
  const float tilt = accelDeltaG(accel, motionOk), hc4Delta = hc4DeltaCm(ranges);
  // If startup calibration caught the chair while it was being positioned,
  // the gyro mean can be several dps away from its true stationary bias. A
  // near-zero acceleration change proves the chair is still, so repair only
  // the gyro bias before listening for the next deliberate action.
  if (phase == RecognitionPhase::kWaiting && motionOk && tilt < 0.010f) {
    for (uint8_t i = 0; i < 3; ++i) baselineGyro[i] += (gyro[i] - baselineGyro[i]) * 0.65f;
    movement = gyroDeltaDps(gyro, motionOk);
  }
  if (phase == RecognitionPhase::kNeedsNeutral) {
    // The user can finish a stretch a few centimetres away from the original
    // seated pose. Rearm on stillness and adopt that pose as the new IMU
    // baseline, otherwise the recognizer remains locked after one action.
    if (!motionOk || movement > kSettleMovementDps) { neutralStarted = 0; return; }
    if (!neutralStarted) neutralStarted = now;
    if (now - neutralStarted >= kNeutralRearmMs) {
      for (uint8_t i = 0; i < 3; ++i) { baselineAccel[i] = accel[i]; baselineGyro[i] = gyro[i]; }
      resetRecognition();
      Serial.println("PROGRESS,REARMED");
    }
    return;
  }
  if (phase == RecognitionPhase::kWaiting) {
    // HC4 has two stable echo modes roughly 25 cm apart even when nobody
    // moves. A range change is therefore accepted only alongside real body
    // motion; chest movement uses the stricter IMU-only gate.
    const bool rangeMovement = hc4Delta >= kLeftDistanceCm &&
                               movement >= kRangeMovementDps && tilt >= kRangeTiltG;
    const bool chestMovement = movement >= kStartMovementDps && tilt >= kStartTiltG;
    const bool deliberate = rangeMovement || chestMovement;
    const bool strongMovement = movement >= kStrongMovementDps && tilt >= kStrongTiltG;
    if (deliberate) {
      if (startEvidenceFrames < 3) ++startEvidenceFrames;
    } else {
      startEvidenceFrames = 0;
    }
    if (!strongMovement && startEvidenceFrames < 2) return;
    phase = RecognitionPhase::kTracking; actionStarted = now;
    outwardSettled = returnMovementSeen = false; settleStarted = 0;
    beginMotionTrace(ranges, accel, gyro, motionOk);
    Serial.printf("PROGRESS,MOTION_STARTED,HC4_DELTA=%.2f,GYRO_DELTA=%.2f,TILT_DELTA=%.3f\n", hc4Delta, movement, tilt);
    return;
  }
  updateMotionTrace(ranges, accel, gyro, motionOk);
  const uint32_t elapsed = now - actionStarted;
  // Angular speed identifies whether the sitter is still moving. Acceleration
  // tilt deliberately remains high while a side pose is being held, so it
  // must not block the held-pose and return transitions.
  const bool moving = movement > kSettleMovementDps;
  if (!outwardSettled) {
    if (elapsed < kMinimumActionMs || moving) settleStarted = 0;
    else if (!settleStarted) settleStarted = now;
    else if (now - settleStarted >= kSettleConfirmMs) {
      // A quick out-and-back gesture can complete between two relatively slow
      // ultrasonic frames. If its current tilt has already fallen well below
      // the captured peak, treat this first settled pose as the completed
      // return instead of waiting for a second movement that already happened.
      if (tilt <= max(0.018f, peakTiltDelta * 0.65f)) {
        finishAction(now);
        return;
      }
      outwardSettled = true;
      settleStarted = 0;
      Serial.println("PROGRESS,OUTWARD_POSE_REACHED,RETURN_TO_NEUTRAL");
    }
  } else if (!returnMovementSeen) {
    if (moving) {
      returnMovementSeen = true;
      settleStarted = 0;
      Serial.println("PROGRESS,RETURN_STARTED");
    }
  } else if (moving) {
    settleStarted = 0;
  } else if (!settleStarted) {
    settleStarted = now;
  } else if (now - settleStarted >= kSettleConfirmMs) {
    finishAction(now);
    return;
  }
  if (elapsed >= kMaximumActionMs) {
    finishAction(now);
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
  calibrationStarted = millis(); baselineGyroCount = 0; calibrationFinalized = false;
  for (uint8_t i = 0; i < 5; ++i) { baselineRange[i] = NAN; baselineRangeCount[i] = 0; }
  for (uint8_t i = 0; i < 3; ++i) baselineAccel[i] = baselineGyro[i] = 0;
  resetRecognition(); recognitionEnabled = false;
  Serial.println("CONTROL,CALIBRATING,KEEP_NEUTRAL_FOR_3_5_SECONDS");
}

void serviceSerialCommands() {
  static String command;
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value != '\n' && value != '\r') { if (command.length() < 32) command += value; continue; }
    command.trim(); command.toUpperCase();
    if (command == "START") {
      // Calibrate after the sitter is in place. Calibrating only at power-on
      // measured the empty chair and caused an immediate false chest event.
      beginCalibration();
      recognitionEnabled = true;
      Serial.println("CONTROL,STARTED,CALIBRATING_CURRENT_SEATED_POSE_3_5S");
    }
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
  float ranges[5], accel[3], gyro[3];
  for (uint8_t i = 0; i < 5; ++i) { ranges[i] = readStableDistance(i); delay(30); }
  const bool motionOk = readMotion(accel, gyro);
  if (now - calibrationStarted < kCalibrationMs) { calibrate(ranges, accel, gyro, motionOk); return; }
  if (!calibrationFinalized) finishCalibration();
  if (recognitionEnabled && now - lastSensorLog >= 450) {
    lastSensorLog = now;
    Serial.printf(
        "PROGRESS,SENSORS,HC=%.1f/%.1f/%.1f/%.1f/%.1f,DHC=%.1f/%.1f/%.1f/%.1f/%.1f,"
        "DA=%.3f/%.3f/%.3f,DG=%.2f/%.2f/%.2f\n",
        ranges[0], ranges[1], ranges[2], ranges[3], ranges[4],
        isfinite(ranges[0]) && isfinite(baselineRange[0]) ? ranges[0] - baselineRange[0] : NAN,
        isfinite(ranges[1]) && isfinite(baselineRange[1]) ? ranges[1] - baselineRange[1] : NAN,
        isfinite(ranges[2]) && isfinite(baselineRange[2]) ? ranges[2] - baselineRange[2] : NAN,
        isfinite(ranges[3]) && isfinite(baselineRange[3]) ? ranges[3] - baselineRange[3] : NAN,
        isfinite(ranges[4]) && isfinite(baselineRange[4]) ? ranges[4] - baselineRange[4] : NAN,
        motionOk ? accel[0] - baselineAccel[0] : NAN,
        motionOk ? accel[1] - baselineAccel[1] : NAN,
        motionOk ? accel[2] - baselineAccel[2] : NAN,
        motionOk ? gyro[0] - baselineGyro[0] : NAN,
        motionOk ? gyro[1] - baselineGyro[1] : NAN,
        motionOk ? gyro[2] - baselineGyro[2] : NAN);
  }
  if (recognitionEnabled) updateRecognition(now, ranges, accel, gyro, motionOk);
}
