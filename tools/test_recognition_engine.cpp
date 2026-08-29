// Deterministic host tests for the pure C++ recognition state machines.
// These tests prove sequencing, self-test gates, release, neutral re-arm and
// de-duplication. They do not replace participant event-level validation.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "recognition_engine.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Harness {
  pecky::RecognitionEngine engine;
  uint32_t timeMs = 0;
  uint32_t counts[4] = {};

  Harness() {
    pecky::CalibrationProfile profile;
    profile.neutralAxG = 0.0f;
    profile.neutralAyG = 0.0f;
    profile.neutralAzG = 1.0f;
    profile.pressureBaseline = 0.0f;
    profile.pressureNoise = 1.0f;
    profile.extensionSign = 1.0f;
    engine.begin(profile);
  }

  void sample(float pitchDeg = 0.0f, float pitchRateDps = 0.0f,
              float sagittalLinearG = 0.0f, float pressureAdc = 0.0f) {
    const float radians = pitchDeg * kPi / 180.0f;
    pecky::SensorFrame frame;
    frame.timeMs = timeMs;
    // With neutral gravity on +Z and board Y as lateral, forward is -X.
    // Add the requested sagittal pulse along that head-forward direction.
    frame.axG = std::sin(radians) - sagittalLinearG;
    frame.ayG = 0.0f;
    frame.azG = std::cos(radians);
    frame.gyDps = pitchRateDps;
    frame.pressureAdc = pressureAdc;
    const auto event = engine.update(frame);
    if (event.valid) ++counts[static_cast<unsigned>(event.action)];
    timeMs += pecky::kSamplePeriodMs;
  }

  void repeat(int samples, float pitchDeg = 0.0f, float pitchRateDps = 0.0f,
              float sagittalLinearG = 0.0f, float pressureAdc = 0.0f) {
    for (int index = 0; index < samples; ++index) {
      sample(pitchDeg, pitchRateDps, sagittalLinearG, pressureAdc);
    }
  }

  void neutral(int samples = 20) { repeat(samples); }
};

bool expectEqual(const char* name, uint32_t actual, uint32_t expected) {
  if (actual == expected) return true;
  std::cerr << "FAIL," << name << ",actual=" << actual
            << ",expected=" << expected << '\n';
  return false;
}

bool testNeckExtension() {
  Harness harness;
  harness.neutral(20);
  for (int index = 1; index <= 10; ++index) {
    harness.sample(index * 2.0f, 50.0f);
  }
  harness.repeat(20, 20.0f);
  for (int index = 9; index >= 0; --index) {
    harness.sample(index * 2.0f, -50.0f);
  }
  harness.neutral(50);
  return expectEqual("neck_extension_once", harness.counts[1], 1) &&
         expectEqual("neck_extension_not_chin", harness.counts[2], 0) &&
         expectEqual("neck_extension_not_resistance", harness.counts[3], 0);
}

bool testChinTuck() {
  Harness harness;
  harness.neutral(20);
  harness.repeat(3, 0.0f, 0.0f, 0.14f);
  harness.neutral(6);
  harness.repeat(3, 0.0f, 0.0f, -0.09f);
  harness.neutral(60);
  return expectEqual("chin_tuck_once", harness.counts[2], 1) &&
         expectEqual("chin_tuck_not_extension", harness.counts[1], 0) &&
         expectEqual("chin_tuck_not_resistance", harness.counts[3], 0);
}

bool testPressureGateAndResistance() {
  Harness disabled;
  disabled.neutral(20);
  disabled.repeat(80, 0.0f, 0.0f, 0.0f, 1200.0f);
  disabled.neutral(30);
  if (!expectEqual("resistance_disabled_without_self_test", disabled.counts[3], 0)) {
    return false;
  }

  Harness harness;
  harness.engine.startPressureSelfTest();
  harness.repeat(15, 0.0f, 0.0f, 0.0f, 0.0f);
  harness.repeat(20, 0.0f, 0.0f, 0.0f, 1200.0f);
  harness.neutral(30);
  if (!harness.engine.status().pressureHealthy) {
    std::cerr << "FAIL,pressure_self_test_not_ready\n";
    return false;
  }

  harness.repeat(70, 0.0f, 0.0f, 0.0f, 1200.0f);
  harness.neutral(40);
  return expectEqual("head_resistance_once", harness.counts[3], 1) &&
         expectEqual("resistance_not_extension", harness.counts[1], 0) &&
         expectEqual("resistance_not_chin", harness.counts[2], 0);
}

}  // namespace

int main() {
  bool ok = true;
  ok = testNeckExtension() && ok;
  ok = testChinTuck() && ok;
  ok = testPressureGateAndResistance() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "PASS,recognition_engine_synthetic_sequence_tests\n";
  return EXIT_SUCCESS;
}
