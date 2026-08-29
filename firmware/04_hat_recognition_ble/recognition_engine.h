#pragma once

// Pure C++ recognition core shared by the ESP32 sketch and the host CSV replay
// tool.  It intentionally has no Arduino or BLE dependency.

#include <math.h>
#include <stdint.h>

namespace pecky {

constexpr float kRadToDeg = 57.29577951308232f;
constexpr uint32_t kSamplePeriodMs = 40;  // Current collected data is 25 Hz.

inline float clampf(float value, float lower, float upper) {
  return value < lower ? lower : (value > upper ? upper : value);
}

inline float square(float value) { return value * value; }

inline float vectorNorm(float x, float y, float z) {
  return sqrtf(square(x) + square(y) + square(z));
}

enum class ActionType : uint8_t {
  kNone = 0,
  kNeckExtension = 1,
  kChinTuck = 2,
  kHeadResistance = 3,
};

struct SensorFrame {
  uint32_t timeMs = 0;
  float axG = 0.0f;
  float ayG = 0.0f;
  float azG = 0.0f;
  float gxDps = 0.0f;
  float gyDps = 0.0f;
  float gzDps = 0.0f;
  float pressureAdc = 0.0f;
};

struct CalibrationProfile {
  float neutralAxG = 0.0f;
  float neutralAyG = 0.0f;
  float neutralAzG = 1.0f;
  float gyroBiasXDps = 0.0f;
  float gyroBiasYDps = 0.0f;
  float gyroBiasZDps = 0.0f;
  float pressureBaseline = 0.0f;
  float pressureNoise = 1.0f;

  // This is a mechanical-installation constant, not a per-user ML parameter.
  // The present cap mounting normally produces negative pitch for extension.
  // Set +1 if a fixed final enclosure mounts the IMU in the opposite direction.
  float extensionSign = -1.0f;
};

struct MotionFeatures {
  uint32_t timeMs = 0;
  float pitchDeltaDeg = 0.0f;
  float rollDeltaDeg = 0.0f;
  float tiltDeg = 0.0f;
  float gxDps = 0.0f;
  float gyDps = 0.0f;
  float gzDps = 0.0f;
  float pitchRateDps = 0.0f;
  float omegaDps = 0.0f;
  float yawRateDps = 0.0f;
  float sagittalLinearG = 0.0f;
  float pressureFiltered = 0.0f;
  float pressureDelta = 0.0f;
  bool neutralStable = false;
};

struct RecognitionEvent {
  bool valid = false;
  ActionType action = ActionType::kNone;
  float confidence = 0.0f;
  uint32_t durationMs = 0;
};

struct RecognitionStatus {
  ActionType candidate = ActionType::kNone;
  uint8_t state = 0;  // 0 idle, 1 moving/pressing, 2 hold, 3 return, 4 cooldown.
  uint32_t candidateDurationMs = 0;
  bool pressureHealthy = false;
  bool pressureTestActive = false;
  bool neutralStable = false;
  float pitchDeltaDeg = 0.0f;
  float tiltDeg = 0.0f;
  float pressureDelta = 0.0f;
};

class FeatureExtractor {
 public:
  void begin(const CalibrationProfile& profile) {
    profile_ = profile;
    const float magnitude = vectorNorm(profile.neutralAxG, profile.neutralAyG,
                                       profile.neutralAzG);
    const float safeMagnitude = magnitude > 0.1f ? magnitude : 1.0f;
    verticalX_ = profile.neutralAxG / safeMagnitude;
    verticalY_ = profile.neutralAyG / safeMagnitude;
    verticalZ_ = profile.neutralAzG / safeMagnitude;
    gravityX_ = profile.neutralAxG;
    gravityY_ = profile.neutralAyG;
    gravityZ_ = profile.neutralAzG;
    pressureFiltered_ = profile.pressureBaseline;

    // Build an orthonormal coordinate system fixed to the wearer's neutral
    // head, rather than comparing raw sensor XYZ between people:
    //   vertical = measured neutral gravity
    //   lateral  = board Y projected onto the horizontal plane
    //   forward  = vertical x lateral
    // The current mounting keeps board Y approximately left/right. The
    // projection removes small cap-roll differences between wearers.
    const float boardYDotVertical = verticalY_;
    lateralX_ = -boardYDotVertical * verticalX_;
    lateralY_ = 1.0f - boardYDotVertical * verticalY_;
    lateralZ_ = -boardYDotVertical * verticalZ_;
    float lateralNorm = vectorNorm(lateralX_, lateralY_, lateralZ_);
    if (lateralNorm < 0.1f) {
      // Defensive fallback for an impossible/incorrect mounting with board Y
      // parallel to gravity: project board Z instead.
      const float boardZDotVertical = verticalZ_;
      lateralX_ = -boardZDotVertical * verticalX_;
      lateralY_ = -boardZDotVertical * verticalY_;
      lateralZ_ = 1.0f - boardZDotVertical * verticalZ_;
      lateralNorm = vectorNorm(lateralX_, lateralY_, lateralZ_);
    }
    const float safeLateral = lateralNorm > 0.1f ? lateralNorm : 1.0f;
    lateralX_ /= safeLateral;
    lateralY_ /= safeLateral;
    lateralZ_ /= safeLateral;
    forwardX_ = verticalY_ * lateralZ_ - verticalZ_ * lateralY_;
    forwardY_ = verticalZ_ * lateralX_ - verticalX_ * lateralZ_;
    forwardZ_ = verticalX_ * lateralY_ - verticalY_ * lateralX_;
    neutralStableCount_ = 0;
    initialized_ = true;
  }

  MotionFeatures update(const SensorFrame& frame) {
    MotionFeatures features;
    features.timeMs = frame.timeMs;
    if (!initialized_) return features;

    features.gxDps = frame.gxDps - profile_.gyroBiasXDps;
    features.gyDps = frame.gyDps - profile_.gyroBiasYDps;
    features.gzDps = frame.gzDps - profile_.gyroBiasZDps;
    features.omegaDps = vectorNorm(features.gxDps, features.gyDps, features.gzDps);
    features.yawRateDps = features.gxDps * verticalX_ +
                          features.gyDps * verticalY_ +
                          features.gzDps * verticalZ_;
    features.pitchRateDps = features.gxDps * lateralX_ +
                            features.gyDps * lateralY_ +
                            features.gzDps * lateralZ_;

    // A slow gravity estimate retains posture while the residual keeps the
    // short front/back translation pulse used by the chin-tuck recognizer.
    constexpr float kGravityAlpha = 0.90f;
    gravityX_ = kGravityAlpha * gravityX_ + (1.0f - kGravityAlpha) * frame.axG;
    gravityY_ = kGravityAlpha * gravityY_ + (1.0f - kGravityAlpha) * frame.ayG;
    gravityZ_ = kGravityAlpha * gravityZ_ + (1.0f - kGravityAlpha) * frame.azG;
    const float gravityMagnitude = vectorNorm(gravityX_, gravityY_, gravityZ_);
    const float safeGravity = gravityMagnitude > 0.1f ? gravityMagnitude : 1.0f;
    const float unitX = gravityX_ / safeGravity;
    const float unitY = gravityY_ / safeGravity;
    const float unitZ = gravityZ_ / safeGravity;
    const float dot = clampf(verticalX_ * unitX + verticalY_ * unitY + verticalZ_ * unitZ,
                             -1.0f, 1.0f);
    features.tiltDeg = acosf(dot) * kRadToDeg;

    const float crossX = verticalY_ * unitZ - verticalZ_ * unitY;
    const float crossY = verticalZ_ * unitX - verticalX_ * unitZ;
    const float crossZ = verticalX_ * unitY - verticalY_ * unitX;
    const float pitchCross =
        crossX * lateralX_ + crossY * lateralY_ + crossZ * lateralZ_;
    const float rollCross =
        crossX * forwardX_ + crossY * forwardY_ + crossZ * forwardZ_;
    features.pitchDeltaDeg = atan2f(pitchCross, dot) * kRadToDeg;
    features.rollDeltaDeg = atan2f(rollCross, dot) * kRadToDeg;

    const float linearX = frame.axG - gravityX_;
    const float linearY = frame.ayG - gravityY_;
    const float linearZ = frame.azG - gravityZ_;
    features.sagittalLinearG = linearX * forwardX_ + linearY * forwardY_ +
                               linearZ * forwardZ_;

    constexpr float kPressureAlpha = 0.70f;
    pressureFiltered_ = kPressureAlpha * pressureFiltered_ +
                        (1.0f - kPressureAlpha) * frame.pressureAdc;
    features.pressureFiltered = pressureFiltered_;
    features.pressureDelta = pressureFiltered_ - profile_.pressureBaseline;

    // The assembled MPU6500 reports a noisier resting gyro than the bench
    // unit. These limits still require a stationary, upright pose but let the
    // physical cap leave the safety re-arm state after a USB START command.
    const bool neutralNow = features.tiltDeg < 9.0f && features.omegaDps < 35.0f;
    if (neutralNow) {
      if (neutralStableCount_ < 6) ++neutralStableCount_;
    } else {
      neutralStableCount_ = 0;
    }
    features.neutralStable = neutralStableCount_ >= 6;
    return features;
  }

 private:
  CalibrationProfile profile_{};
  bool initialized_ = false;
  float verticalX_ = 0.0f;
  float verticalY_ = 0.0f;
  float verticalZ_ = 1.0f;
  float lateralX_ = 0.0f;
  float lateralY_ = 1.0f;
  float lateralZ_ = 0.0f;
  float forwardX_ = 1.0f;
  float forwardY_ = 0.0f;
  float forwardZ_ = 0.0f;
  float gravityX_ = 0.0f;
  float gravityY_ = 0.0f;
  float gravityZ_ = 1.0f;
  float pressureFiltered_ = 0.0f;
  uint8_t neutralStableCount_ = 0;
};

class NeckExtensionRecognizer {
 public:
  enum class State : uint8_t { kIdle = 0, kMoving = 1, kHold = 2, kReturn = 3 };

  void begin(float extensionSign) {
    extensionSign_ = extensionSign >= 0.0f ? 1.0f : -1.0f;
    reset();
  }

  void reset() {
    state_ = State::kIdle;
    armed_ = false;
    onsetCount_ = 0;
    holdCount_ = 0;
    startMs_ = 0;
    holdMs_ = 0;
    maxAngleDeg_ = 0.0f;
  }

  RecognitionEvent update(const MotionFeatures& features, bool pressureActive) {
    RecognitionEvent result;
    if (pressureActive) {
      reset();
      return result;
    }
    const float signedAngle = extensionSign_ * features.pitchDeltaDeg;
    const float signedRate = extensionSign_ * features.pitchRateDps;
    const bool directionSafe = fabsf(features.rollDeltaDeg) < 12.0f &&
                               fabsf(features.yawRateDps) < 55.0f;

    switch (state_) {
      case State::kIdle:
        if (features.neutralStable) armed_ = true;
        if (armed_ && signedRate > 30.0f && directionSafe) {
          if (++onsetCount_ >= 2) {
            state_ = State::kMoving;
            startMs_ = features.timeMs;
            maxAngleDeg_ = signedAngle;
          }
        } else {
          onsetCount_ = 0;
        }
        break;

      case State::kMoving:
        if (!directionSafe || features.timeMs - startMs_ > 4000) {
          reset();
          break;
        }
        if (signedAngle > maxAngleDeg_) maxAngleDeg_ = signedAngle;
        // The assembled cap's physical mounting can amplify the pitch angle.
        // Keep a broad safe range so a clinically normal extension reaches
        // the hold phase instead of being rejected at the peak.
        if (signedAngle >= 8.0f && signedAngle <= 65.0f) {
          state_ = State::kHold;
          holdCount_ = 1;
        }
        break;

      case State::kHold:
        if (!directionSafe || signedAngle < 6.0f || signedAngle > 70.0f) {
          reset();
          break;
        }
        if (signedAngle > maxAngleDeg_) maxAngleDeg_ = signedAngle;
        // Three 25 Hz samples make a deliberate extension responsive while
        // still rejecting a single IMU shock.
        if (++holdCount_ >= 3) {
          holdMs_ = holdCount_ * kSamplePeriodMs;
          state_ = State::kReturn;
        }
        break;

      case State::kReturn:
        if (features.timeMs - startMs_ > 7000) {
          reset();
          break;
        }
        if (features.neutralStable && fabsf(features.pitchDeltaDeg) < 6.0f) {
          result.valid = true;
          result.action = ActionType::kNeckExtension;
          result.confidence = clampf(0.60f + (maxAngleDeg_ - 12.0f) / 55.0f,
                                     0.60f, 0.98f);
          result.durationMs = holdMs_;
          reset();
        }
        break;
    }
    return result;
  }

  bool active() const { return state_ != State::kIdle; }
  uint8_t stateCode() const { return static_cast<uint8_t>(state_); }
  uint32_t durationMs(uint32_t nowMs) const {
    return state_ == State::kIdle ? 0 : nowMs - startMs_;
  }

 private:
  State state_ = State::kIdle;
  bool armed_ = false;
  float extensionSign_ = -1.0f;
  uint8_t onsetCount_ = 0;
  uint8_t holdCount_ = 0;
  uint32_t startMs_ = 0;
  uint32_t holdMs_ = 0;
  float maxAngleDeg_ = 0.0f;
};

class ChinTuckRecognizer {
 public:
  enum class State : uint8_t { kIdle = 0, kFirstPulse = 1, kReturnPulse = 2 };

  void reset() {
    state_ = State::kIdle;
    armed_ = false;
    onsetCount_ = 0;
    firstSign_ = 0.0f;
    startMs_ = 0;
    maxOmegaDps_ = 0.0f;
    firstPeakAbs_ = 0.0f;
    secondPeakAbs_ = 0.0f;
  }

  RecognitionEvent update(const MotionFeatures& features, bool pressureActive) {
    RecognitionEvent result;
    const bool postureSafe = features.tiltDeg < 9.0f &&
                             fabsf(features.pitchDeltaDeg) < 9.0f &&
                             fabsf(features.rollDeltaDeg) < 8.0f &&
                             fabsf(features.yawRateDps) < 45.0f;
    if (pressureActive || !postureSafe) {
      reset();
      return result;
    }

    switch (state_) {
      case State::kIdle:
        if (features.neutralStable) armed_ = true;
        if (armed_ && fabsf(features.sagittalLinearG) >= 0.10f) {
          const float sign = features.sagittalLinearG >= 0.0f ? 1.0f : -1.0f;
          if (onsetCount_ == 0 || sign == firstSign_) {
            firstSign_ = sign;
            if (fabsf(features.sagittalLinearG) > firstPeakAbs_) {
              firstPeakAbs_ = fabsf(features.sagittalLinearG);
            }
            // Three consecutive samples rejects isolated walking/hat shocks.
            if (++onsetCount_ >= 3) {
              state_ = State::kFirstPulse;
              startMs_ = features.timeMs;
              maxOmegaDps_ = features.omegaDps;
            }
          } else {
            onsetCount_ = 1;
            firstSign_ = sign;
          }
        } else {
          onsetCount_ = 0;
        }
        break;

      case State::kFirstPulse: {
        const uint32_t elapsed = features.timeMs - startMs_;
        if (features.omegaDps > maxOmegaDps_) maxOmegaDps_ = features.omegaDps;
        if (elapsed > 1600 || maxOmegaDps_ > 65.0f) {
          reset();
          break;
        }
        const bool oppositePulse =
            firstSign_ * features.sagittalLinearG <= -0.06f;
        if (elapsed >= 240 && oppositePulse) {
          secondPeakAbs_ = fabsf(features.sagittalLinearG);
          const float ratio = secondPeakAbs_ / fmaxf(firstPeakAbs_, 0.001f);
          if (ratio >= 0.40f && ratio <= 2.50f) {
            state_ = State::kReturnPulse;
          }
        }
        break;
      }

      case State::kReturnPulse:
        if (features.timeMs - startMs_ > 3000) {
          reset();
          break;
        }
        if (features.neutralStable) {
          if (maxOmegaDps_ <= 65.0f) {
            result.valid = true;
            result.action = ActionType::kChinTuck;
            const float pulseQuality =
                fminf(firstPeakAbs_, secondPeakAbs_) / 0.30f;
            result.confidence = clampf(0.60f + pulseQuality * 0.25f,
                                       0.60f, 0.90f);
            result.durationMs = features.timeMs - startMs_;
          }
          reset();
        }
        break;
    }
    return result;
  }

  bool active() const { return state_ != State::kIdle; }
  uint8_t stateCode() const { return static_cast<uint8_t>(state_); }
  uint32_t durationMs(uint32_t nowMs) const {
    return state_ == State::kIdle ? 0 : nowMs - startMs_;
  }

 private:
  State state_ = State::kIdle;
  bool armed_ = false;
  uint8_t onsetCount_ = 0;
  float firstSign_ = 0.0f;
  uint32_t startMs_ = 0;
  float maxOmegaDps_ = 0.0f;
  float firstPeakAbs_ = 0.0f;
  float secondPeakAbs_ = 0.0f;
};

class HeadResistanceRecognizer {
 public:
  enum class State : uint8_t { kIdle = 0, kPressing = 1, kHold = 2, kRelease = 3 };

  void begin(const CalibrationProfile& profile) {
    pressureBaseline_ = profile.pressureBaseline;
    pressureMargin_ = fmaxf(600.0f, 8.0f * fmaxf(profile.pressureNoise, 1.0f));
    pressureOn_ = pressureBaseline_ + pressureMargin_;
    pressureOff_ = pressureBaseline_ + 0.35f * pressureMargin_;
    pressureReady_ = false;
    selfTestActive_ = false;
    pressureWasReleased_ = true;
    autoReleasedCount_ = 0;
    recentMotionSamples_ = 0;
    reset();
  }

  void startSelfTest() {
    reset();
    pressureReady_ = false;
    selfTestActive_ = true;
    selfTestSamplesRemaining_ = 200;  // Eight seconds at 25 Hz.
    selfTestReleasedCount_ = 0;
    selfTestHighCount_ = 0;
  }

  void cancelSelfTest() {
    selfTestActive_ = false;
    selfTestSamplesRemaining_ = 0;
    selfTestReleasedCount_ = 0;
    selfTestHighCount_ = 0;
    reset();
  }

  void reset() {
    state_ = State::kIdle;
    highCount_ = 0;
    releaseCount_ = 0;
    holdCount_ = 0;
    invalidHoldCount_ = 0;
    motionEvidence_ = false;
    startMs_ = 0;
    qualifiedHoldMs_ = 0;
  }

  RecognitionEvent update(const MotionFeatures& features) {
    RecognitionEvent result;
    const bool pressureHigh = features.pressureFiltered >= pressureOn_;
    const bool pressureReleased = features.pressureFiltered <= pressureOff_;
    const bool pressureEngaged = !pressureReleased;
    if (features.omegaDps >= 8.0f ||
        fabsf(features.sagittalLinearG) >= 0.025f) {
      recentMotionSamples_ = 15;  // Preserve onset evidence across pressure EMA delay.
    } else if (recentMotionSamples_ > 0) {
      --recentMotionSamples_;
    }

    // The cap must work without opening the App.  A stable released channel
    // for 0.5 s after neutral calibration is sufficient to arm pressure
    // recognition automatically.  The explicit BLE self-test remains useful
    // as a diagnostic, but is no longer a product-startup dependency.
    if (!pressureReady_ && !selfTestActive_) {
      if (pressureReleased) {
        if (autoReleasedCount_ < 12) ++autoReleasedCount_;
        if (autoReleasedCount_ >= 12) pressureReady_ = true;
      } else {
        autoReleasedCount_ = 0;
      }
    }

    if (selfTestActive_) {
      if (selfTestSamplesRemaining_ > 0) --selfTestSamplesRemaining_;
      if (pressureReleased) {
        if (selfTestReleasedCount_ < 12) ++selfTestReleasedCount_;
        selfTestHighCount_ = 0;
      } else if (selfTestReleasedCount_ >= 12 && pressureHigh) {
        if (selfTestHighCount_ < 10) ++selfTestHighCount_;
        if (selfTestHighCount_ >= 10) {
          pressureReady_ = true;
          selfTestActive_ = false;
          pressureWasReleased_ = false;
        }
      }
      if (selfTestSamplesRemaining_ == 0) selfTestActive_ = false;
      return result;
    }

    if (pressureHigh) {
      if (highCount_ < 250) ++highCount_;
    } else {
      highCount_ = 0;
    }
    const bool pressureRisingEdge =
        pressureReady_ && pressureWasReleased_ && pressureHigh;
    if (pressureReleased) pressureWasReleased_ = true;
    if (pressureRisingEdge) pressureWasReleased_ = false;

    switch (state_) {
      case State::kIdle:
        if (pressureRisingEdge) {
          state_ = State::kPressing;
          startMs_ = features.timeMs;
          motionEvidence_ = recentMotionSamples_ > 0;
        }
        break;

      case State::kPressing:
        if (!pressureEngaged) {
          reset();
          break;
        }
        if (features.timeMs - startMs_ > 2500) {
          reset();
          break;
        }
        if (features.omegaDps >= 8.0f ||
            fabsf(features.sagittalLinearG) >= 0.025f) {
          motionEvidence_ = true;
        }
        if (highCount_ >= 10 && features.tiltDeg < 10.0f) {
          state_ = State::kHold;
          holdCount_ = 0;
          invalidHoldCount_ = 0;
        }
        break;

      case State::kHold: {
        if (!pressureEngaged) {
          reset();
          break;
        }
        const bool stableHold = features.tiltDeg < 10.0f && features.omegaDps < 25.0f;
        if (stableHold) {
          if (holdCount_ < 250) ++holdCount_;
          invalidHoldCount_ = 0;
        } else if (++invalidHoldCount_ > 5) {
          reset();
          break;
        }
        // Real participant recordings contain valid holds of 1.4--2.9 s.
        // Count after one qualified second, but only when pressure and head
        // motion appeared together; touching the loose pad alone is rejected.
        if (holdCount_ >= 25 && motionEvidence_) {
          qualifiedHoldMs_ = holdCount_ * kSamplePeriodMs;
          state_ = State::kRelease;
        }
        break;
      }

      case State::kRelease:
        // A long press can never count twice. Only a full release and neutral
        // return emits the single event.
        if (pressureReleased) {
          if (releaseCount_ < 12) ++releaseCount_;
        } else {
          releaseCount_ = 0;
        }
        if (releaseCount_ >= 12 && features.neutralStable) {
          result.valid = true;
          result.action = ActionType::kHeadResistance;
          result.confidence = clampf(0.80f + features.pressureDelta / 12000.0f,
                                     0.80f, 0.99f);
          result.durationMs = qualifiedHoldMs_;
          reset();
        }
        break;
    }
    return result;
  }

  bool pressureActive(const MotionFeatures& features) const {
    return pressureReady_ && features.pressureFiltered >= pressureOff_;
  }
  bool pressureHealthy() const { return pressureReady_; }
  bool selfTestActive() const { return selfTestActive_; }
  bool active() const { return state_ != State::kIdle; }
  uint8_t stateCode() const { return static_cast<uint8_t>(state_); }
  uint32_t durationMs(uint32_t nowMs) const {
    return state_ == State::kIdle ? 0 : nowMs - startMs_;
  }
  float pressureOn() const { return pressureOn_; }

 private:
  State state_ = State::kIdle;
  float pressureBaseline_ = 0.0f;
  float pressureMargin_ = 600.0f;
  float pressureOn_ = 600.0f;
  float pressureOff_ = 210.0f;
  bool pressureReady_ = false;
  bool selfTestActive_ = false;
  bool pressureWasReleased_ = true;
  uint16_t selfTestSamplesRemaining_ = 0;
  uint8_t selfTestReleasedCount_ = 0;
  uint8_t selfTestHighCount_ = 0;
  uint8_t autoReleasedCount_ = 0;
  uint8_t recentMotionSamples_ = 0;
  uint16_t highCount_ = 0;
  uint8_t releaseCount_ = 0;
  uint16_t holdCount_ = 0;
  uint8_t invalidHoldCount_ = 0;
  bool motionEvidence_ = false;
  uint32_t startMs_ = 0;
  uint32_t qualifiedHoldMs_ = 0;
};

class RecognitionEngine {
 public:
  void begin(const CalibrationProfile& profile) {
    profile_ = profile;
    extractor_.begin(profile);
    extension_.begin(profile.extensionSign);
    chinTuck_.reset();
    resistance_.begin(profile);
    cooldownSamples_ = 0;
    waitForNeutral_ = true;
    status_ = RecognitionStatus{};
  }

  void startPressureSelfTest() {
    extension_.reset();
    chinTuck_.reset();
    resistance_.startSelfTest();
    cooldownSamples_ = 0;
    waitForNeutral_ = true;
    status_.pressureHealthy = false;
    status_.pressureTestActive = true;
  }

  void resetSessionRecognition() {
    extension_.reset();
    chinTuck_.reset();
    resistance_.cancelSelfTest();
    cooldownSamples_ = 0;
    waitForNeutral_ = true;
    status_.candidate = ActionType::kNone;
    status_.state = 4;
    status_.pressureTestActive = false;
  }

  RecognitionEvent update(const SensorFrame& frame) {
    const MotionFeatures features = extractor_.update(frame);
    status_.neutralStable = features.neutralStable;
    status_.pitchDeltaDeg = features.pitchDeltaDeg;
    status_.tiltDeg = features.tiltDeg;
    status_.pressureDelta = features.pressureDelta;
    status_.pressureHealthy = resistance_.pressureHealthy();
    status_.pressureTestActive = resistance_.selfTestActive();

    if (cooldownSamples_ > 0) {
      --cooldownSamples_;
      extension_.reset();
      chinTuck_.reset();
      resistance_.reset();
      status_.candidate = ActionType::kNone;
      status_.state = 4;
      status_.candidateDurationMs = cooldownSamples_ * kSamplePeriodMs;
      return RecognitionEvent{};
    }

    // A pressure self-test is an explicit calibration operation, not an
    // exercise candidate. It must run even while ordinary recognition waits
    // for a neutral re-arm after a command.
    if (resistance_.selfTestActive()) {
      resistance_.update(features);
      extension_.reset();
      chinTuck_.reset();
      if (!resistance_.selfTestActive()) waitForNeutral_ = true;
      updateStatus(frame.timeMs, features);
      return RecognitionEvent{};
    }

    if (waitForNeutral_) {
      extension_.reset();
      chinTuck_.reset();
      resistance_.reset();
      status_.candidate = ActionType::kNone;
      status_.state = 4;
      status_.candidateDurationMs = 0;
      if (features.neutralStable) waitForNeutral_ = false;
      return RecognitionEvent{};
    }

    // Pressure-confirmed resistance always has first priority. While pressure
    // is active the two IMU-only recognizers are reset, so an arm/hand movement
    // cannot also become a chin-tuck or extension count.
    RecognitionEvent resistanceEvent = resistance_.update(features);
    const bool pressureActive = resistance_.pressureActive(features);
    RecognitionEvent extensionEvent;
    RecognitionEvent chinEvent;
    if (pressureActive || resistance_.active()) {
      extension_.reset();
      chinTuck_.reset();
    } else {
      extensionEvent = extension_.update(features, false);
      chinEvent = chinTuck_.update(features, false);
    }

    RecognitionEvent accepted;
    if (resistanceEvent.valid) {
      accepted = resistanceEvent;
    } else if (extensionEvent.valid && !chinEvent.valid) {
      accepted = extensionEvent;
    } else if (chinEvent.valid && !extensionEvent.valid) {
      accepted = chinEvent;
    } else if (extensionEvent.valid && chinEvent.valid) {
      // Ambiguous movement: reject rather than guess and create a false count.
      extension_.reset();
      chinTuck_.reset();
    }

    if (accepted.valid) {
      // Pressure already requires a full release, so a short 0.48 s refractory
      // period is sufficient and does not hide the next supervised repetition.
      cooldownSamples_ = accepted.action == ActionType::kHeadResistance
                             ? 12
                             : (accepted.action == ActionType::kChinTuck ? 50 : 38);
      extension_.reset();
      chinTuck_.reset();
      resistance_.reset();
      waitForNeutral_ = true;
    }
    updateStatus(frame.timeMs, features);
    return accepted;
  }

  const RecognitionStatus& status() const { return status_; }

 private:
  void updateStatus(uint32_t nowMs, const MotionFeatures& features) {
    status_.pressureHealthy = resistance_.pressureHealthy();
    status_.pressureTestActive = resistance_.selfTestActive();
    status_.neutralStable = features.neutralStable;
    if (resistance_.active()) {
      status_.candidate = ActionType::kHeadResistance;
      status_.state = resistance_.stateCode();
      status_.candidateDurationMs = resistance_.durationMs(nowMs);
    } else if (extension_.active()) {
      status_.candidate = ActionType::kNeckExtension;
      status_.state = extension_.stateCode();
      status_.candidateDurationMs = extension_.durationMs(nowMs);
    } else if (chinTuck_.active()) {
      status_.candidate = ActionType::kChinTuck;
      status_.state = chinTuck_.stateCode();
      status_.candidateDurationMs = chinTuck_.durationMs(nowMs);
    } else {
      status_.candidate = ActionType::kNone;
      status_.state = 0;
      status_.candidateDurationMs = 0;
    }
  }

  CalibrationProfile profile_{};
  FeatureExtractor extractor_{};
  NeckExtensionRecognizer extension_{};
  ChinTuckRecognizer chinTuck_{};
  HeadResistanceRecognizer resistance_{};
  uint16_t cooldownSamples_ = 0;
  bool waitForNeutral_ = true;
  RecognitionStatus status_{};
};

inline const char* actionName(ActionType action) {
  switch (action) {
    case ActionType::kNeckExtension:
      return "neck_extension";
    case ActionType::kChinTuck:
      return "chin_tuck";
    case ActionType::kHeadResistance:
      return "head_resistance";
    default:
      return "none";
  }
}

}  // namespace pecky
