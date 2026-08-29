// Host-side protocol-stage replay for the exact recognition core used by ESP32.
//
// Build from the repository root:
//   g++ -std=c++17 -O2 -Ifirmware/04_hat_recognition_ble
//     tools/replay_hat_recognizers.cpp -o /tmp/pecky-replay
//
// Run:
//   /tmp/pecky-replay data/raw/hat_0027.csv
//
// The phase column is printed only as an offline oracle. It is never passed to
// RecognitionEngine and therefore cannot leak the collection timer into the
// recognizers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "recognition_engine.h"

namespace {

struct CsvRow {
  uint32_t timeMs = 0;
  int phase = -1;
  int axRaw = 0;
  int ayRaw = 0;
  int azRaw = 0;
  int gxRaw = 0;
  int gyRaw = 0;
  int gzRaw = 0;
  int pressureRaw = 0;
};

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ',')) fields.push_back(field);
  return fields;
}

std::vector<CsvRow> loadRows(const std::string& path) {
  std::ifstream source(path);
  if (!source) throw std::runtime_error("cannot open " + path);
  std::string line;
  std::getline(source, line);  // Header.
  std::vector<CsvRow> rows;
  while (std::getline(source, line)) {
    const auto fields = split(line);
    if (fields.size() != 10) continue;
    CsvRow row;
    row.timeMs = static_cast<uint32_t>(std::stoul(fields[1]));
    row.phase = std::stoi(fields[2]);
    row.axRaw = std::stoi(fields[3]);
    row.ayRaw = std::stoi(fields[4]);
    row.azRaw = std::stoi(fields[5]);
    row.gxRaw = std::stoi(fields[6]);
    row.gyRaw = std::stoi(fields[7]);
    row.gzRaw = std::stoi(fields[8]);
    row.pressureRaw = std::stoi(fields[9]);
    rows.push_back(row);
  }
  return rows;
}

float median(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 == 0 ? 0.5f * (values[middle - 1] + values[middle])
                               : values[middle];
}

pecky::CalibrationProfile calibrationFromPhaseZero(const std::vector<CsvRow>& rows) {
  std::vector<CsvRow> neutral;
  for (const auto& row : rows) {
    if (row.phase == 0 && neutral.size() < 75) neutral.push_back(row);
  }
  if (neutral.size() < 75) throw std::runtime_error("need 75 phase-0 samples");
  pecky::CalibrationProfile profile;
  std::vector<float> pressure;
  for (const auto& row : neutral) {
    profile.neutralAxG += row.axRaw / 16384.0f;
    profile.neutralAyG += row.ayRaw / 16384.0f;
    profile.neutralAzG += row.azRaw / 16384.0f;
    profile.gyroBiasXDps += row.gxRaw / 131.0f;
    profile.gyroBiasYDps += row.gyRaw / 131.0f;
    profile.gyroBiasZDps += row.gzRaw / 131.0f;
    pressure.push_back(static_cast<float>(row.pressureRaw));
  }
  const float inverse = 1.0f / neutral.size();
  profile.neutralAxG *= inverse;
  profile.neutralAyG *= inverse;
  profile.neutralAzG *= inverse;
  profile.gyroBiasXDps *= inverse;
  profile.gyroBiasYDps *= inverse;
  profile.gyroBiasZDps *= inverse;
  profile.pressureBaseline = median(pressure);
  std::vector<float> deviations;
  for (const float value : pressure) {
    deviations.push_back(std::fabs(value - profile.pressureBaseline));
  }
  profile.pressureNoise = std::max(1.0f, 1.4826f * median(deviations));
  profile.extensionSign = -1.0f;
  return profile;
}

pecky::SensorFrame toFrame(const CsvRow& row) {
  pecky::SensorFrame frame;
  frame.timeMs = row.timeMs;
  frame.axG = row.axRaw / 16384.0f;
  frame.ayG = row.ayRaw / 16384.0f;
  frame.azG = row.azRaw / 16384.0f;
  frame.gxDps = row.gxRaw / 131.0f;
  frame.gyDps = row.gyRaw / 131.0f;
  frame.gzDps = row.gzRaw / 131.0f;
  frame.pressureAdc = static_cast<float>(row.pressureRaw);
  return frame;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " data/raw/hat_XXXX.csv\n";
    return 2;
  }
  try {
    const auto rows = loadRows(argv[1]);
    const auto profile = calibrationFromPhaseZero(rows);
    pecky::RecognitionEngine engine;
    engine.begin(profile);
    uint32_t actionCounts[4] = {};
    uint32_t phaseCounts[8] = {};
    for (const auto& row : rows) {
      const pecky::RecognitionEvent event = engine.update(toFrame(row));
      if (!event.valid) continue;
      const auto actionIndex = static_cast<unsigned>(event.action);
      if (actionIndex < 4) ++actionCounts[actionIndex];
      if (row.phase >= 0 && row.phase < 8) ++phaseCounts[row.phase];
      std::cout << "EVENT,t_ms=" << row.timeMs << ",phase_oracle=" << row.phase
                << ",action=" << pecky::actionName(event.action)
                << ",confidence=" << std::fixed << std::setprecision(2)
                << event.confidence << ",duration_ms=" << event.durationMs << '\n';
    }
    const auto& status = engine.status();
    std::cout << "SUMMARY,extension=" << actionCounts[1]
              << ",chin_tuck=" << actionCounts[2]
              << ",head_resistance=" << actionCounts[3]
              << ",pressure_healthy=" << (status.pressureHealthy ? 1 : 0) << '\n';
    std::cout << "PHASE_ORACLE_COUNTS";
    for (size_t phase = 0; phase < 8; ++phase) {
      std::cout << ",p" << phase << '=' << phaseCounts[phase];
    }
    std::cout << '\n';
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
