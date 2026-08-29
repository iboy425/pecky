#include <Arduino.h>
#include <Wire.h>

#include <math.h>

namespace {

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kWindowDurationMs = 3000;

// 160 ms gives approximately 6.25 complete five-sensor frames per second.
constexpr uint32_t kFramePeriodMs = 160;
// A 150 cm round trip is about 8.75 ms. The timeout deliberately excludes
// echoes beyond the useful chair-sensing range.
constexpr uint32_t kEchoTimeoutUs = 9000;
constexpr float kMinimumDistanceCm = 2.0f;
constexpr float kMaximumDistanceCm = 150.0f;
// Every ultrasonic trigger is at least 30 ms after the preceding trigger,
// measured trigger-to-trigger. This leaves enough acoustic settling time even
// when the preceding target is close and its echo pulse ends quickly.
constexpr uint32_t kTriggerToTriggerUs = 30000;

constexpr uint8_t kMpuSdaPin = 8;
constexpr uint8_t kMpuSclPin = 9;
constexpr uint8_t kBuzzerPin = 13;

constexpr uint8_t kMpuRegSampleRateDivider = 0x19;
constexpr uint8_t kMpuRegConfig = 0x1A;
constexpr uint8_t kMpuRegGyroConfig = 0x1B;
constexpr uint8_t kMpuRegAccelConfig = 0x1C;
constexpr uint8_t kMpuRegAccelXoutHigh = 0x3B;
constexpr uint8_t kMpuRegPowerManagement1 = 0x6B;
constexpr uint8_t kMpuRegWhoAmI = 0x75;

struct UltrasonicChannel {
  uint8_t trigPin;
  uint8_t echoPin;
};

// Keep this mapping synchronized with hardware_bringup.ino.
constexpr UltrasonicChannel kUltrasonicChannels[] = {
    {4, 5},    // HC1 left outer
    {6, 7},    // HC2 left inner
    {10, 11},  // HC3 center
    {15, 16},  // HC4 right inner
    {17, 18},  // HC5 right outer
};

constexpr size_t kUltrasonicCount =
    sizeof(kUltrasonicChannels) / sizeof(kUltrasonicChannels[0]);

// Non-adjacent modules are sampled first to reduce 40 kHz cross-talk.
constexpr size_t kUltrasonicReadOrder[] = {0, 3, 1, 4, 2};

struct RangeReading {
  bool valid = false;
  float distanceCm = NAN;
};

struct MpuReading {
  bool valid = false;
  float axG = NAN;
  float ayG = NAN;
  float azG = NAN;
  float gxDps = NAN;
  float gyDps = NAN;
  float gzDps = NAN;
};

struct LabelWindow {
  bool active = false;
  uint32_t id = 0;
  const char* label = "UNLABELED";
  uint32_t startMs = 0;
  uint32_t endMs = 0;
};

uint8_t mpuAddress = 0;
bool mpuReady = false;

LabelWindow window;
uint32_t nextWindowId = 1;

RangeReading frameRanges[kUltrasonicCount];
bool frameInProgress = false;
size_t frameReadPosition = 0;
uint32_t frameId = 0;
uint32_t frameStartMs = 0;
uint32_t nextFrameDueMs = 0;
uint32_t nextPingDueUs = 0;

uint32_t frameWindowId = 0;
const char* frameLabel = "UNLABELED";
int32_t frameWindowElapsedMs = -1;

bool timeReachedMs(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

bool timeReachedUs(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
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

  for (uint8_t address : kCandidateAddresses) {
    uint8_t identity = 0;
    if (mpuReadRegister(address, kMpuRegWhoAmI, identity) &&
        (identity == 0x68 || identity == 0x69 || identity == 0x70)) {
      mpuAddress = address;
      break;
    }
  }

  if (mpuAddress == 0) {
    mpuReady = false;
    return false;
  }

  bool ok = mpuWriteRegister(mpuAddress, kMpuRegPowerManagement1, 0x00);
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
  const int16_t rawGx = makeInt16(data[8], data[9]);
  const int16_t rawGy = makeInt16(data[10], data[11]);
  const int16_t rawGz = makeInt16(data[12], data[13]);

  reading.valid = true;
  reading.axG = rawAx / 16384.0f;
  reading.ayG = rawAy / 16384.0f;
  reading.azG = rawAz / 16384.0f;
  reading.gxDps = rawGx / 131.0f;
  reading.gyDps = rawGy / 131.0f;
  reading.gzDps = rawGz / 131.0f;
  return reading;
}

RangeReading readUltrasonic(const UltrasonicChannel& channel) {
  RangeReading reading;

  if (digitalRead(channel.echoPin) == HIGH) {
    delayMicroseconds(100);
    if (digitalRead(channel.echoPin) == HIGH) {
      return reading;
    }
  }

  digitalWrite(channel.trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(channel.trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(channel.trigPin, LOW);

  const uint32_t echoTimeUs =
      pulseIn(channel.echoPin, HIGH, kEchoTimeoutUs);
  if (echoTimeUs == 0) {
    return reading;
  }

  const float distanceCm = echoTimeUs * 0.0343f * 0.5f;
  if (distanceCm < kMinimumDistanceCm ||
      distanceCm > kMaximumDistanceCm) {
    return reading;
  }

  reading.valid = true;
  reading.distanceCm = distanceCm;
  return reading;
}

void printCsvFloat(float value, unsigned int digits) {
  if (!isfinite(value)) {
    Serial.print("NaN");
    return;
  }
  Serial.print(value, digits);
}

void playStartCue() {
  tone(kBuzzerPin, 2400, 110);
}

void playEndCue() {
  tone(kBuzzerPin, 1400, 180);
}

void playCancelCue() {
  tone(kBuzzerPin, 700, 100);
}

const char* labelForCommand(char command) {
  switch (command) {
    case 'N':
      return "NEUTRAL";
    case 'L':
      return "LEFT";
    case 'R':
      return "RIGHT";
    case 'C':
      return "CHEST";
    default:
      return nullptr;
  }
}

void finishWindowIfDue(uint32_t nowMs) {
  if (!window.active || !timeReachedMs(nowMs, window.endMs)) {
    return;
  }

  // A frame takes readings sequentially. If it began inside this window, emit
  // that SAMPLE before WINDOW,END so a host can safely close the capture as
  // soon as it receives END without needing an arbitrary serial grace period.
  if (frameInProgress && frameWindowId == window.id) {
    return;
  }

  // Output may follow the deadline by one in-flight frame, but the timestamp
  // remains the exact device-clock deadline.
  Serial.printf("WINDOW,END,%lu,%s,%lu,%lu\n",
                static_cast<unsigned long>(window.id), window.label,
                static_cast<unsigned long>(window.endMs),
                static_cast<unsigned long>(kWindowDurationMs));
  window.active = false;
  window.label = "UNLABELED";
  playEndCue();
}

void startWindow(const char* label, uint32_t nowMs) {
  finishWindowIfDue(nowMs);
  if (window.active) {
    Serial.printf("STATUS,COMMAND,REJECTED,BUSY,%lu,%s\n",
                  static_cast<unsigned long>(window.id), window.label);
    return;
  }

  if (nextWindowId == 0) {
    nextWindowId = 1;
  }
  window.active = true;
  window.id = nextWindowId++;
  window.label = label;
  window.startMs = nowMs;
  window.endMs = nowMs + kWindowDurationMs;

  Serial.printf("WINDOW,START,%lu,%s,%lu,%lu\n",
                static_cast<unsigned long>(window.id), window.label,
                static_cast<unsigned long>(window.startMs),
                static_cast<unsigned long>(kWindowDurationMs));
  playStartCue();
}

void cancelWindow(uint32_t nowMs) {
  finishWindowIfDue(nowMs);
  if (!window.active) {
    Serial.println("STATUS,COMMAND,IGNORED,NO_ACTIVE_WINDOW");
    return;
  }

  const uint32_t elapsedMs = nowMs - window.startMs;
  Serial.printf("WINDOW,CANCEL,%lu,%s,%lu,%lu\n",
                static_cast<unsigned long>(window.id), window.label,
                static_cast<unsigned long>(nowMs),
                static_cast<unsigned long>(elapsedMs));
  window.active = false;
  window.label = "UNLABELED";
  playCancelCue();
}

void handleSerialCommand(char command) {
  if (command >= 'a' && command <= 'z') {
    command = static_cast<char>(command - 'a' + 'A');
  }

  const char* label = labelForCommand(command);
  if (label != nullptr) {
    startWindow(label, millis());
    return;
  }

  if (command == 'Q' || command == 'X') {
    cancelWindow(millis());
    return;
  }

  Serial.printf("STATUS,COMMAND,UNKNOWN,%u\n",
                static_cast<unsigned int>(static_cast<uint8_t>(command)));
}

void processSerialCommands() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == '\r' || command == '\n' || command == ' ' ||
        command == '\t') {
      continue;
    }
    handleSerialCommand(command);
  }
}

void beginFrame(uint32_t nowMs) {
  frameInProgress = true;
  frameReadPosition = 0;
  frameStartMs = nowMs;
  ++frameId;
  if (frameId == 0) {
    frameId = 1;
  }

  for (RangeReading& reading : frameRanges) {
    reading = RangeReading{};
  }

  if (window.active) {
    frameWindowId = window.id;
    frameLabel = window.label;
    frameWindowElapsedMs =
        static_cast<int32_t>(frameStartMs - window.startMs);
  } else {
    frameWindowId = 0;
    frameLabel = "UNLABELED";
    frameWindowElapsedMs = -1;
  }

  nextPingDueUs = micros();
}

void printSample(const MpuReading& mpu, uint32_t frameSpanMs) {
  uint32_t validMask = 0;
  for (size_t i = 0; i < kUltrasonicCount; ++i) {
    if (frameRanges[i].valid) {
      validMask |= (1UL << i);
    }
  }

  Serial.printf("SAMPLE,%lu,%lu,%lu,%s,%ld,%lu",
                static_cast<unsigned long>(frameStartMs),
                static_cast<unsigned long>(frameId),
                static_cast<unsigned long>(frameWindowId), frameLabel,
                static_cast<long>(frameWindowElapsedMs),
                static_cast<unsigned long>(frameSpanMs));

  for (size_t i = 0; i < kUltrasonicCount; ++i) {
    Serial.print(',');
    printCsvFloat(frameRanges[i].distanceCm, 2);
  }

  Serial.printf(",%lu,%u,", static_cast<unsigned long>(validMask),
                mpu.valid ? 1U : 0U);
  printCsvFloat(mpu.axG, 4);
  Serial.print(',');
  printCsvFloat(mpu.ayG, 4);
  Serial.print(',');
  printCsvFloat(mpu.azG, 4);
  Serial.print(',');
  printCsvFloat(mpu.gxDps, 3);
  Serial.print(',');
  printCsvFloat(mpu.gyDps, 3);
  Serial.print(',');
  printCsvFloat(mpu.gzDps, 3);
  Serial.println();
}

void serviceFrameAcquisition() {
  if (!frameInProgress || !timeReachedUs(micros(), nextPingDueUs)) {
    return;
  }

  const size_t channelIndex = kUltrasonicReadOrder[frameReadPosition];
  const uint32_t pingStartedUs = micros();
  frameRanges[channelIndex] =
      readUltrasonic(kUltrasonicChannels[channelIndex]);
  ++frameReadPosition;

  if (frameReadPosition < kUltrasonicCount) {
    nextPingDueUs = pingStartedUs + kTriggerToTriggerUs;
    return;
  }

  const MpuReading mpu = readMpu();
  const uint32_t completedAtMs = millis();
  printSample(mpu, completedAtMs - frameStartMs);
  frameInProgress = false;
  nextFrameDueMs = frameStartMs + kFramePeriodMs;
}

void printProtocolHeader() {
  Serial.println();
  Serial.println("BOOT,CHAIR_DATA_COLLECTION_V1");
  Serial.println("CONFIG,SERIAL_BAUD,115200");
  Serial.println(
      "CONFIG,PINS,HC1=4/5,HC2=6/7,HC3=10/11,HC4=15/16,HC5=17/18,"
      "MPU_SDA=8,MPU_SCL=9,BUZZER=13,DHT=DISABLED");
  Serial.println(
      "CONFIG,CAPTURE,WINDOW_MS=3000,FRAME_PERIOD_MS=160,TARGET_FPS=6.25,"
      "MAX_RANGE_CM=150,TRIGGER_INTERVAL_US=30000,"
      "SAMPLE_TIMESTAMP=FRAME_START");
  Serial.println(
      "SCHEMA,SAMPLE,t_ms,frame_id,window_id,label,window_elapsed_ms,"
      "frame_span_ms,hc1_cm,hc2_cm,hc3_cm,hc4_cm,hc5_cm,"
      "range_valid_mask,mpu_ok,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps");
  Serial.println(
      "SCHEMA,WINDOW,event,window_id,label,t_ms,elapsed_or_duration_ms");
  Serial.println(
      "READY,COMMANDS,N=NEUTRAL,L=LEFT,R=RIGHT,C=CHEST,"
      "Q_OR_X=CANCEL,DURATION_MS=3000");
  Serial.printf("STATUS,MPU,%s,ADDRESS=0x%02X\n",
                mpuReady ? "READY" : "NOT_FOUND", mpuAddress);
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(800);

  for (const UltrasonicChannel& channel : kUltrasonicChannels) {
    pinMode(channel.trigPin, OUTPUT);
    digitalWrite(channel.trigPin, LOW);
    pinMode(channel.echoPin, INPUT);
  }

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);

  Wire.begin(kMpuSdaPin, kMpuSclPin, 400000);
  Wire.setTimeOut(50);
  initializeMpu();

  printProtocolHeader();
  nextFrameDueMs = millis();
}

void loop() {
  processSerialCommands();

  const uint32_t nowMs = millis();
  finishWindowIfDue(nowMs);

  if (!frameInProgress && timeReachedMs(nowMs, nextFrameDueMs)) {
    beginFrame(nowMs);
  }

  serviceFrameAcquisition();
}
