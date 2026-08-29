#include <Arduino.h>
#include <Wire.h>

#include <math.h>

namespace {

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kEchoTimeoutUs = 30000;
constexpr uint32_t kUltrasonicGapMs = 65;
constexpr uint32_t kTelemetryPeriodMs = 3000;

constexpr uint8_t kMpuSdaPin = 8;
constexpr uint8_t kMpuSclPin = 9;
constexpr uint8_t kDhtPin = 12;
constexpr uint8_t kBuzzerPin = 13;

constexpr uint8_t kMpuRegSampleRateDivider = 0x19;
constexpr uint8_t kMpuRegConfig = 0x1A;
constexpr uint8_t kMpuRegGyroConfig = 0x1B;
constexpr uint8_t kMpuRegAccelConfig = 0x1C;
constexpr uint8_t kMpuRegAccelXoutHigh = 0x3B;
constexpr uint8_t kMpuRegPowerManagement1 = 0x6B;
constexpr uint8_t kMpuRegWhoAmI = 0x75;

struct UltrasonicChannel {
  const char* name;
  uint8_t trigPin;
  uint8_t echoPin;
};

constexpr UltrasonicChannel kUltrasonicChannels[] = {
    {"HC1_LEFT_OUTER", 4, 5},
    {"HC2_LEFT_INNER", 6, 7},
    {"HC3_CENTER", 10, 11},
    {"HC4_RIGHT_INNER", 15, 16},
    {"HC5_RIGHT_OUTER", 17, 18},
};

constexpr size_t kUltrasonicCount =
    sizeof(kUltrasonicChannels) / sizeof(kUltrasonicChannels[0]);

// Read non-adjacent modules first to reduce 40 kHz cross-talk.
constexpr size_t kUltrasonicReadOrder[] = {0, 3, 1, 4, 2};

enum class RangeStatus {
  kOk,
  kEchoStuckHigh,
  kNoEcho,
  kOutOfRange,
};

struct RangeReading {
  RangeStatus status = RangeStatus::kNoEcho;
  uint32_t echoTimeUs = 0;
  float distanceCm = NAN;
};

struct MatrixEchoCapture {
  bool initiallyHigh = false;
  bool sawRise = false;
  bool sawFall = false;
  uint32_t riseAtUs = 0;
  uint32_t pulseWidthUs = 0;
};

enum class DhtStatus {
  kOk,
  kNoResponse,
  kTimingError,
  kChecksumError,
};

struct DhtReading {
  DhtStatus status = DhtStatus::kNoResponse;
  float temperatureC = NAN;
  float humidityRh = NAN;
};

struct MpuReading {
  bool ok = false;
  float axG = NAN;
  float ayG = NAN;
  float azG = NAN;
  float gxDps = NAN;
  float gyDps = NAN;
  float gzDps = NAN;
  float temperatureC = NAN;
};

uint8_t mpuAddress = 0;
uint8_t mpuWhoAmI = 0;
bool mpuReady = false;
uint32_t nextTelemetryMs = 0;

const char* rangeStatusName(RangeStatus status) {
  switch (status) {
    case RangeStatus::kOk:
      return "PASS";
    case RangeStatus::kEchoStuckHigh:
      return "ECHO_STUCK_HIGH";
    case RangeStatus::kNoEcho:
      return "NO_ECHO";
    case RangeStatus::kOutOfRange:
      return "OUT_OF_RANGE";
  }
  return "UNKNOWN";
}

const char* dhtStatusName(DhtStatus status) {
  switch (status) {
    case DhtStatus::kOk:
      return "PASS";
    case DhtStatus::kNoResponse:
      return "NO_RESPONSE";
    case DhtStatus::kTimingError:
      return "TIMING_ERROR";
    case DhtStatus::kChecksumError:
      return "CHECKSUM_ERROR";
  }
  return "UNKNOWN";
}

int16_t makeInt16(uint8_t highByte, uint8_t lowByte) {
  return static_cast<int16_t>((static_cast<uint16_t>(highByte) << 8) |
                              lowByte);
}

bool mpuWriteRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool mpuReadRegisters(uint8_t address, uint8_t startReg, uint8_t* data,
                      size_t length) {
  Wire.beginTransmission(address);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const size_t received = Wire.requestFrom(
      static_cast<int>(address), static_cast<int>(length), true);
  if (received != length) {
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool mpuReadRegister(uint8_t address, uint8_t reg, uint8_t& value) {
  return mpuReadRegisters(address, reg, &value, 1);
}

bool initializeMpu() {
  constexpr uint8_t kCandidateAddresses[] = {0x68, 0x69};
  mpuAddress = 0;
  mpuWhoAmI = 0;

  for (uint8_t address : kCandidateAddresses) {
    uint8_t identity = 0;
    if (!mpuReadRegister(address, kMpuRegWhoAmI, identity)) {
      continue;
    }
    if (identity == 0x68 || identity == 0x69 || identity == 0x70) {
      mpuAddress = address;
      mpuWhoAmI = identity;
      break;
    }
  }

  if (mpuAddress == 0) {
    mpuReady = false;
    return false;
  }

  bool ok = true;
  ok &= mpuWriteRegister(mpuAddress, kMpuRegPowerManagement1, 0x00);
  delay(100);
  ok &= mpuWriteRegister(mpuAddress, kMpuRegConfig, 0x03);
  ok &= mpuWriteRegister(mpuAddress, kMpuRegSampleRateDivider, 0x09);
  ok &= mpuWriteRegister(mpuAddress, kMpuRegGyroConfig, 0x00);
  ok &= mpuWriteRegister(mpuAddress, kMpuRegAccelConfig, 0x00);
  mpuReady = ok;
  return ok;
}

MpuReading readMpu() {
  MpuReading reading;
  if (!mpuReady) {
    return reading;
  }

  uint8_t data[14];
  if (!mpuReadRegisters(mpuAddress, kMpuRegAccelXoutHigh, data,
                        sizeof(data))) {
    mpuReady = false;
    return reading;
  }

  const int16_t rawAx = makeInt16(data[0], data[1]);
  const int16_t rawAy = makeInt16(data[2], data[3]);
  const int16_t rawAz = makeInt16(data[4], data[5]);
  const int16_t rawTemperature = makeInt16(data[6], data[7]);
  const int16_t rawGx = makeInt16(data[8], data[9]);
  const int16_t rawGy = makeInt16(data[10], data[11]);
  const int16_t rawGz = makeInt16(data[12], data[13]);

  reading.ok = true;
  reading.axG = rawAx / 16384.0f;
  reading.ayG = rawAy / 16384.0f;
  reading.azG = rawAz / 16384.0f;
  reading.gxDps = rawGx / 131.0f;
  reading.gyDps = rawGy / 131.0f;
  reading.gzDps = rawGz / 131.0f;
  reading.temperatureC = mpuWhoAmI == 0x70
                             ? rawTemperature / 333.87f + 21.0f
                             : rawTemperature / 340.0f + 36.53f;
  return reading;
}

RangeReading readUltrasonic(const UltrasonicChannel& channel) {
  RangeReading reading;

  if (digitalRead(channel.echoPin) == HIGH) {
    delayMicroseconds(100);
    if (digitalRead(channel.echoPin) == HIGH) {
      reading.status = RangeStatus::kEchoStuckHigh;
      return reading;
    }
  }

  digitalWrite(channel.trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(channel.trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(channel.trigPin, LOW);

  reading.echoTimeUs = pulseIn(channel.echoPin, HIGH, kEchoTimeoutUs);
  if (reading.echoTimeUs == 0) {
    reading.status = RangeStatus::kNoEcho;
    return reading;
  }

  reading.distanceCm = reading.echoTimeUs * 0.0343f * 0.5f;
  if (reading.distanceCm < 2.0f || reading.distanceCm > 400.0f) {
    reading.status = RangeStatus::kOutOfRange;
    return reading;
  }

  reading.status = RangeStatus::kOk;
  return reading;
}

void runUltrasonicMatrixAttempt(size_t triggerIndex, size_t attempt) {
  MatrixEchoCapture captures[kUltrasonicCount];
  for (size_t echoIndex = 0; echoIndex < kUltrasonicCount; ++echoIndex) {
    captures[echoIndex].initiallyHigh =
        digitalRead(kUltrasonicChannels[echoIndex].echoPin) == HIGH;
  }

  const UltrasonicChannel& triggerChannel =
      kUltrasonicChannels[triggerIndex];
  digitalWrite(triggerChannel.trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(triggerChannel.trigPin, HIGH);
  delayMicroseconds(10);
  const bool triggerReadbackHigh =
      digitalRead(triggerChannel.trigPin) == HIGH;
  digitalWrite(triggerChannel.trigPin, LOW);
  const bool triggerReadbackLow = digitalRead(triggerChannel.trigPin) == LOW;

  const uint32_t listenStartUs = micros();
  while (micros() - listenStartUs < kEchoTimeoutUs) {
    const uint32_t nowUs = micros();
    for (size_t echoIndex = 0; echoIndex < kUltrasonicCount; ++echoIndex) {
      MatrixEchoCapture& capture = captures[echoIndex];
      if (capture.initiallyHigh || capture.sawFall) {
        continue;
      }

      const bool isHigh =
          digitalRead(kUltrasonicChannels[echoIndex].echoPin) == HIGH;
      if (!capture.sawRise && isHigh) {
        capture.sawRise = true;
        capture.riseAtUs = nowUs;
      } else if (capture.sawRise && !isHigh) {
        capture.sawFall = true;
        capture.pulseWidthUs = nowUs - capture.riseAtUs;
      }
    }
  }

  Serial.printf("MATRIX,TRIG=HC%u,TRIG_GPIO=%u,HIGH_READ=%u,LOW_READ=%u,"
                "ATTEMPT=%u",
                static_cast<unsigned>(triggerIndex + 1),
                static_cast<unsigned>(triggerChannel.trigPin),
                triggerReadbackHigh ? 1U : 0U,
                triggerReadbackLow ? 1U : 0U,
                static_cast<unsigned>(attempt + 1));
  bool reported = false;
  for (size_t echoIndex = 0; echoIndex < kUltrasonicCount; ++echoIndex) {
    const MatrixEchoCapture& capture = captures[echoIndex];
    if (capture.initiallyHigh) {
      Serial.printf(",ECHO_HC%u=STUCK_HIGH",
                    static_cast<unsigned>(echoIndex + 1));
      reported = true;
    } else if (capture.sawFall) {
      const float distanceCm = capture.pulseWidthUs * 0.0343f * 0.5f;
      Serial.printf(",ECHO_HC%u_US=%lu,ECHO_HC%u_CM=%.2f",
                    static_cast<unsigned>(echoIndex + 1),
                    static_cast<unsigned long>(capture.pulseWidthUs),
                    static_cast<unsigned>(echoIndex + 1), distanceCm);
      reported = true;
    } else if (capture.sawRise) {
      Serial.printf(",ECHO_HC%u=RISE_NO_FALL",
                    static_cast<unsigned>(echoIndex + 1));
      reported = true;
    }
  }
  if (!reported) {
    Serial.print(",NO_ECHO_ON_ANY_GPIO");
  }
  Serial.println();
}

void runEchoBiasProbe() {
  Serial.println("ECHO_PROBE,START,WEAK_INTERNAL_PULL_TEST");
  for (size_t echoIndex = 0; echoIndex < kUltrasonicCount; ++echoIndex) {
    const uint8_t echoPin = kUltrasonicChannels[echoIndex].echoPin;

    pinMode(echoPin, INPUT);
    delay(2);
    const int floatingLevel = digitalRead(echoPin);

    pinMode(echoPin, INPUT_PULLUP);
    delay(2);
    const int pullupLevel = digitalRead(echoPin);

    pinMode(echoPin, INPUT_PULLDOWN);
    delay(2);
    const int pulldownLevel = digitalRead(echoPin);

    pinMode(echoPin, INPUT);
    Serial.printf("ECHO_PROBE,HC%u,ECHO_GPIO=%u,INPUT=%d,PULLUP=%d,"
                  "PULLDOWN=%d\n",
                  static_cast<unsigned>(echoIndex + 1),
                  static_cast<unsigned>(echoPin), floatingLevel, pullupLevel,
                  pulldownLevel);
  }
  Serial.println("ECHO_PROBE,END");
}

void runUltrasonicMatrixTest() {
  constexpr size_t kAttempts = 3;
  Serial.println(
      "MATRIX,START,TRIGGER_EACH_HC_AND_LISTEN_ON_ALL_ECHO_GPIOS");
  for (size_t triggerIndex = 0; triggerIndex < kUltrasonicCount;
       ++triggerIndex) {
    for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
      runUltrasonicMatrixAttempt(triggerIndex, attempt);
      delay(kUltrasonicGapMs);
    }
  }
  runEchoBiasProbe();
  Serial.println("MATRIX,END");
}

bool waitForDhtLevel(uint8_t level, uint32_t timeoutUs) {
  const uint32_t start = micros();
  while (digitalRead(kDhtPin) != level) {
    if (micros() - start > timeoutUs) {
      return false;
    }
  }
  return true;
}

bool measureDhtLevel(uint8_t level, uint32_t timeoutUs, uint32_t& durationUs) {
  const uint32_t start = micros();
  while (digitalRead(kDhtPin) == level) {
    if (micros() - start > timeoutUs) {
      return false;
    }
  }
  durationUs = micros() - start;
  return true;
}

DhtReading readDht11() {
  DhtReading reading;
  uint8_t data[5] = {0, 0, 0, 0, 0};

  pinMode(kDhtPin, OUTPUT);
  digitalWrite(kDhtPin, LOW);
  delay(20);
  digitalWrite(kDhtPin, HIGH);
  delayMicroseconds(30);
  pinMode(kDhtPin, INPUT_PULLUP);

  uint32_t pulseLengthUs = 0;
  if (!waitForDhtLevel(LOW, 200) ||
      !measureDhtLevel(LOW, 200, pulseLengthUs) ||
      !waitForDhtLevel(HIGH, 200) ||
      !measureDhtLevel(HIGH, 200, pulseLengthUs)) {
    reading.status = DhtStatus::kNoResponse;
    return reading;
  }

  for (int bit = 0; bit < 40; ++bit) {
    uint32_t lowDurationUs = 0;
    uint32_t highDurationUs = 0;
    if (!waitForDhtLevel(LOW, 100) ||
        !measureDhtLevel(LOW, 100, lowDurationUs) ||
        !waitForDhtLevel(HIGH, 100) ||
        !measureDhtLevel(HIGH, 150, highDurationUs)) {
      reading.status = DhtStatus::kTimingError;
      return reading;
    }

    data[bit / 8] <<= 1;
    if (highDurationUs > 45) {
      data[bit / 8] |= 1;
    }
  }

  const uint8_t expectedChecksum =
      static_cast<uint8_t>(data[0] + data[1] + data[2] + data[3]);
  if (data[4] != expectedChecksum) {
    reading.status = DhtStatus::kChecksumError;
    return reading;
  }

  reading.humidityRh = data[0] + data[1] * 0.1f;
  reading.temperatureC = (data[2] & 0x7F) + data[3] * 0.1f;
  if (data[2] & 0x80) {
    reading.temperatureC = -reading.temperatureC;
  }
  reading.status = DhtStatus::kOk;
  return reading;
}

void playBuzzerTest() {
  Serial.println("CHECK,BUZZER,MANUAL_CONFIRM,TONE_SENT_2000HZ");
  tone(kBuzzerPin, 2000, 250);
  delay(300);
  noTone(kBuzzerPin);
}

bool printUltrasonicCheck(size_t index) {
  const UltrasonicChannel& channel = kUltrasonicChannels[index];
  constexpr size_t kAttempts = 3;
  float validDistances[kAttempts];
  size_t validCount = 0;
  RangeStatus lastFailure = RangeStatus::kNoEcho;

  for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
    const RangeReading reading = readUltrasonic(channel);
    if (reading.status == RangeStatus::kOk) {
      validDistances[validCount++] = reading.distanceCm;
    } else {
      lastFailure = reading.status;
    }
    if (attempt + 1 < kAttempts) {
      delay(kUltrasonicGapMs);
    }
  }

  if (validCount >= 2) {
    for (size_t i = 0; i < validCount; ++i) {
      for (size_t j = i + 1; j < validCount; ++j) {
        if (validDistances[j] < validDistances[i]) {
          const float temporary = validDistances[i];
          validDistances[i] = validDistances[j];
          validDistances[j] = temporary;
        }
      }
    }
    const float medianDistance = validDistances[validCount / 2];
    Serial.printf("CHECK,%s,PASS,DIST_CM=%.2f,VALID=%u/%u\n", channel.name,
                  medianDistance, static_cast<unsigned>(validCount),
                  static_cast<unsigned>(kAttempts));
    return true;
  }

  if (validCount == 1) {
    Serial.printf("CHECK,%s,FAIL,UNSTABLE_ECHO,VALID=1/%u,LAST=%s\n",
                  channel.name, static_cast<unsigned>(kAttempts),
                  rangeStatusName(lastFailure));
    return false;
  }

  Serial.printf("CHECK,%s,FAIL,%s,VALID=0/%u\n", channel.name,
                rangeStatusName(lastFailure),
                static_cast<unsigned>(kAttempts));
  return false;
}

bool printMpuCheck() {
  if (!mpuReady && !initializeMpu()) {
    Serial.println("CHECK,MPU6050,FAIL,NO_I2C_DEVICE_AT_0X68_OR_0X69");
    return false;
  }

  const MpuReading reading = readMpu();
  if (!reading.ok) {
    Serial.println("CHECK,MPU6050,FAIL,READ_ERROR");
    return false;
  }

  const float accelerationMagnitude =
      sqrtf(reading.axG * reading.axG + reading.ayG * reading.ayG +
            reading.azG * reading.azG);
  Serial.printf(
      "CHECK,MPU6050,PASS,ADDR=0x%02X,WHO_AM_I=0x%02X,ACC_MAG_G=%.3f,"
      "TEMP_C=%.2f\n",
      mpuAddress, mpuWhoAmI, accelerationMagnitude, reading.temperatureC);
  return true;
}

bool printDhtCheck() {
  const DhtReading reading = readDht11();
  if (reading.status != DhtStatus::kOk) {
    Serial.printf("CHECK,DHT11,FAIL,%s\n", dhtStatusName(reading.status));
    return false;
  }

  Serial.printf("CHECK,DHT11,PASS,TEMP_C=%.1f,HUMIDITY_RH=%.1f\n",
                reading.temperatureC, reading.humidityRh);
  return true;
}

void runFullSelfTest() {
  Serial.println("SELFTEST,START");
  int passed = 0;
  int failed = 0;

  for (size_t orderIndex = 0; orderIndex < kUltrasonicCount; ++orderIndex) {
    const size_t channelIndex = kUltrasonicReadOrder[orderIndex];
    if (printUltrasonicCheck(channelIndex)) {
      ++passed;
    } else {
      ++failed;
    }
    delay(kUltrasonicGapMs);
  }

  if (printMpuCheck()) {
    ++passed;
  } else {
    ++failed;
  }

  if (printDhtCheck()) {
    ++passed;
  } else {
    ++failed;
  }

  playBuzzerTest();
  Serial.printf("SUMMARY,PASS=%d,FAIL=%d,BUZZER=MANUAL_CONFIRM\n", passed,
                failed);
  Serial.println("SELFTEST,END");
}

void printTelemetry() {
  RangeReading ranges[kUltrasonicCount];
  for (size_t orderIndex = 0; orderIndex < kUltrasonicCount; ++orderIndex) {
    const size_t channelIndex = kUltrasonicReadOrder[orderIndex];
    ranges[channelIndex] =
        readUltrasonic(kUltrasonicChannels[channelIndex]);
    delay(kUltrasonicGapMs);
  }

  if (!mpuReady) {
    initializeMpu();
  }
  const MpuReading mpu = readMpu();
  const DhtReading dht = readDht11();

  Serial.printf("DATA,T_MS=%lu", static_cast<unsigned long>(millis()));
  for (size_t i = 0; i < kUltrasonicCount; ++i) {
    if (ranges[i].status == RangeStatus::kOk) {
      Serial.printf(",%s_CM=%.2f", kUltrasonicChannels[i].name,
                    ranges[i].distanceCm);
    } else {
      Serial.printf(",%s=%s", kUltrasonicChannels[i].name,
                    rangeStatusName(ranges[i].status));
    }
  }

  if (dht.status == DhtStatus::kOk) {
    Serial.printf(",DHT_TEMP_C=%.1f,DHT_RH=%.1f", dht.temperatureC,
                  dht.humidityRh);
  } else {
    Serial.printf(",DHT11=%s", dhtStatusName(dht.status));
  }

  if (mpu.ok) {
    Serial.printf(",MPU_AX_G=%.3f,MPU_AY_G=%.3f,MPU_AZ_G=%.3f,"
                  "MPU_GX_DPS=%.2f,MPU_GY_DPS=%.2f,MPU_GZ_DPS=%.2f",
                  mpu.axG, mpu.ayG, mpu.azG, mpu.gxDps, mpu.gyDps,
                  mpu.gzDps);
  } else {
    Serial.print(",MPU6050=READ_ERROR");
  }
  Serial.println();
}

void handleSerialCommand(char command) {
  if (command >= 'a' && command <= 'z') {
    command = command - 'a' + 'A';
  }

  if (command == 'R') {
    runFullSelfTest();
    return;
  }
  if (command == 'B') {
    playBuzzerTest();
    return;
  }
  if (command >= '1' && command <= '5') {
    const size_t index = static_cast<size_t>(command - '1');
    printUltrasonicCheck(index);
    return;
  }
  if (command == 'M') {
    printMpuCheck();
    return;
  }
  if (command == 'D') {
    printDhtCheck();
    return;
  }
  if (command == 'X') {
    runUltrasonicMatrixTest();
    return;
  }
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(1500);

  Serial.println();
  Serial.println("BOOT,QINGXIAN_CHAIR_HARDWARE_BRINGUP_V1");
  Serial.println("INFO,SERIAL_PORT_EXPECTED,COM8,BAUD=115200");
  Serial.println(
      "INFO,PINS,HC1=4/5,HC2=6/7,HC3=10/11,HC4=15/16,HC5=17/18,"
      "MPU_SDA=8,MPU_SCL=9,DHT=12,BUZZER=13");

  for (const UltrasonicChannel& channel : kUltrasonicChannels) {
    pinMode(channel.trigPin, OUTPUT);
    digitalWrite(channel.trigPin, LOW);
    pinMode(channel.echoPin, INPUT);
  }

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);
  pinMode(kDhtPin, INPUT_PULLUP);

  Wire.begin(kMpuSdaPin, kMpuSclPin, 400000);
  Wire.setTimeOut(50);
  initializeMpu();

  runFullSelfTest();
  nextTelemetryMs = millis() + kTelemetryPeriodMs;
}

void loop() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command != '\r' && command != '\n' && command != ' ' &&
        command != '\t') {
      handleSerialCommand(command);
    }
  }

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextTelemetryMs) >= 0) {
    printTelemetry();
    nextTelemetryMs = millis() + kTelemetryPeriodMs;
  }
}
