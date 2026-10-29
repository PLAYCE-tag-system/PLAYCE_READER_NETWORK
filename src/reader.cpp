#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Adafruit_SSD1306.h>
#include <LoRa.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>

#include "ota_config.h"
#include "oled_tag_flash.h"

namespace {
constexpr int kRfidRxPin = 34;
constexpr int kRfidTxPin = 4;
constexpr long kLoRaFrequencyHz = 915000000;
constexpr uint8_t kLoRaSyncWord = 0x42;
constexpr unsigned long kLoRaRetryMs = 10000;
constexpr unsigned long kInventoryIntervalMs = 1000;
constexpr unsigned long kTagLostMs = 3000;
constexpr unsigned long kUpstreamLostMs = 35000;
constexpr unsigned long kForwardIntervalMs = 2000;
constexpr unsigned long kMergeWindowMs = 300;
constexpr unsigned long kFrameMs = 2000;
constexpr unsigned long kSlotMs = 600;
constexpr unsigned long kFinalSlotMs = 750;
constexpr unsigned long kRelaySlotWaitMs = 450;
constexpr unsigned long kSlotGuardMs = 45;
constexpr unsigned long kTagAfterHeartbeatGapMs = 150;
constexpr unsigned long kFinalTagGapMs = 200;
constexpr unsigned long kReaderHeartbeatLostMs = 12000;
constexpr unsigned long kWifiRetryMs = 10000;
constexpr size_t kMaxTags = 64;
constexpr size_t kMaxFrame = 256;
constexpr size_t kMaxLoRaPacket = 240;

constexpr uint8_t kInventoryCommand[] = {0xBB, 0x00, 0x22, 0x00,
                                          0x00, 0x22, 0x7E};
const char *const kNodeSuffixes[] = {"92f6ac", "ca3490", "cb21f8"};
const char *const kNodeNames[] = {"playce-92f6ac", "playce-ca3490",
                                  "playce-cb21f8"};
const uint8_t PROGMEM kBeonGlyph[] = {
    0x00, 0x18, 0xc3, 0x18, 0xc3, 0x18, 0xc3, 0x18,
    0xff, 0xf8, 0xc3, 0x18, 0xc3, 0x18, 0xc3, 0x18,
    0xff, 0x18, 0x00, 0x18, 0x38, 0x18, 0x38, 0x00,
    0x38, 0x00, 0x38, 0x00, 0x3f, 0xf8, 0x00, 0x00,
};

struct TagState {
  String epc;
  bool localActive = false;
  unsigned long localLastSeen = 0;
  uint8_t upstreamMask = 0;
  uint8_t upstreamRouteMask = 0;
  int8_t rssi[3] = {0, 0, 0};
  unsigned long sourceLastSeen[3] = {0, 0, 0};
  bool dirty = false;
  unsigned long dirtySince = 0;
  unsigned long lastForward = 0;
  uint8_t retryRemaining = 0;
};

HardwareSerial rfidSerial(2);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
OledTagFlash tagFlash;
TagState tags[kMaxTags];
uint8_t frame[kMaxFrame];
size_t frameLength = 0;
size_t expectedFrameLength = 0;
String hostname;
String lastEpc;
bool oledReady = false;
bool otaRunning = false;
bool loraReady = false;
bool readerSeen = false;
int8_t nodeIndex = -1;
uint8_t upstreamReadersMask = 0;
uint32_t sequence = 0;
unsigned long readerHeartbeatSeen[3] = {0, 0, 0};
unsigned long lastWifiAttempt = 0;
unsigned long lastInventory = 0;
unsigned long lastReaderFrame = 0;
unsigned long lastLoRaAttempt = 0;
unsigned long lastUpstreamPacket = 0;
uint32_t upstreamMissed = 0;
uint32_t upstreamSequence = 0;
bool upstreamSequenceKnown = false;
int upstreamRssi = -200;
int upstreamSnr10 = 0;
int link12Rssi = -200, link12Snr10 = 0;
uint32_t link12Missed = 0;
unsigned long link12AgeMs = 0;
size_t nextTagIndex = 0;
unsigned long nextRootFrameAt = 0;
unsigned long nextRelaySlotAt = 0;
unsigned long slotStartedAt = 0;
unsigned long heartbeatFinishedAt = 0;
bool relaySlotPending = false;
bool tagSentInSlot = false;

uint8_t localBit() {
  return nodeIndex >= 0 ? static_cast<uint8_t>(1U << nodeIndex) : 0;
}

uint8_t aggregateMask(const TagState &tag) {
  return tag.upstreamMask | (tag.localActive ? localBit() : 0);
}

uint8_t aggregateRouteMask(const TagState &tag) {
  const uint8_t activeSources = aggregateMask(tag);
  return activeSources == 0
             ? 0
             : static_cast<uint8_t>(tag.upstreamRouteMask | localBit());
}

size_t activeTagCount() {
  size_t count = 0;
  for (const TagState &tag : tags) {
    if (aggregateMask(tag) != 0) ++count;
  }
  return count;
}

void showStatus(const char *status) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PLAYCE RELAY");
  display.println(hostname);
  display.println(status);
  display.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                                : String("Wi-Fi offline"));
  display.printf("Tags: %u\n", static_cast<unsigned>(activeTagCount()));
  if (lastEpc.length()) display.println(lastEpc.substring(0, 20));
  if (nodeIndex >= 0) {
    display.setTextSize(2);
    display.setCursor(0, 48);
    display.print(nodeIndex + 1);
    display.drawBitmap(14, 48, kBeonGlyph, 16, 16, SSD1306_WHITE);
  }
  display.display();
}

void markDirty(TagState &tag) {
  if (!tag.dirty) tag.dirtySince = millis();
  tag.dirty = true;
  tag.retryRemaining = 3;
}

unsigned long airtimeMs(size_t bytes) {
  // SF7, 125 kHz, coding rate 4/5, explicit header and CRC.
  const int numerator = static_cast<int>(8 * bytes) - 28 + 44;
  const int payloadSymbols = 8 + ((numerator + 27) / 28) * 5;
  return static_cast<unsigned long>((12.25 + payloadSymbols) * 1.024 + 2);
}

bool sendLoRa(const String &packet) {
  if (!loraReady || nodeIndex < 0 || packet.length() > kMaxLoRaPacket) return false;
  LoRa.idle();
  if (LoRa.beginPacket() == 0) return false;
  LoRa.print(packet);
  const bool sent = LoRa.endPacket() == 1;
  LoRa.receive();
  if (!sent) Serial.println("LoRa TX failed");
  return sent;
}

String tagPacket(const TagState &tag, uint32_t nextSequence) {
  String packet;
  packet.reserve(210);
  packet = "{\"v\":2,\"from\":\"" + hostname +
           "\",\"event\":\"tag\",\"seq\":" + String(nextSequence) +
           ",\"epc\":\"" + tag.epc + "\",\"mask\":" +
           String(aggregateMask(tag)) + ",\"path\":" +
           String(aggregateRouteMask(tag));
  for (uint8_t source = 0; source < 3; ++source) {
    if ((aggregateMask(tag) & (1U << source)) == 0) continue;
    packet += ",\"r" + String(source) + "\":" + String(tag.rssi[source]);
  }
  packet += "}";
  return packet;
}

void forwardTag(TagState &tag, const String &packet) {
  ++sequence;
  if (sendLoRa(packet)) {
    tag.lastForward = millis();
    tag.dirty = false;
    if (tag.retryRemaining) --tag.retryRemaining;
    Serial.printf("RELAY_TAG,%s,mask=%u\n", tag.epc.c_str(), aggregateMask(tag));
  }
}

void forwardHeartbeat() {
  uint8_t readers = upstreamReadersMask;
  if (readerSeen && millis() - lastReaderFrame < kReaderHeartbeatLostMs) {
    readers |= localBit();
  }
  String packet =
      "{\"v\":2,\"from\":\"" + hostname +
      "\",\"event\":\"heartbeat\",\"seq\":" + String(++sequence) +
      ",\"readers\":" + String(readers) +
      ",\"rx\":" + String(nodeIndex > 0 ? upstreamRssi : -200) +
      ",\"sn\":" + String(nodeIndex > 0 ? upstreamSnr10 : 0) +
      ",\"miss\":" + String(nodeIndex > 0 ? upstreamMissed : 0) +
      ",\"age\":" + String(nodeIndex > 0 && lastUpstreamPacket
                                      ? millis() - lastUpstreamPacket : 0);
  if (nodeIndex == 2) {
    packet += ",\"r12\":" + String(link12Rssi) +
              ",\"s12\":" + String(link12Snr10) +
              ",\"m12\":" + String(link12Missed) +
              ",\"a12\":" + String(link12AgeMs);
  }
  packet += "}";
  sendLoRa(packet);
}

void startLoRa() {
  lastLoRaAttempt = millis();
  loraReady = LoRa.begin(kLoRaFrequencyHz);
  if (loraReady) {
    LoRa.setSyncWord(kLoRaSyncWord);
    LoRa.enableCrc();
    LoRa.receive();
    Serial.println("LoRa relay ready at 915 MHz");
    showStatus("LoRa relay ready");
  } else {
    Serial.println("LoRa init failed; retrying");
    showStatus("LoRa failed");
  }
}

TagState *findTag(const String &epc) {
  TagState *freeSlot = nullptr;
  for (TagState &tag : tags) {
    if (tag.epc == epc) return &tag;
    if (tag.epc.length() == 0 && freeSlot == nullptr) freeSlot = &tag;
  }
  if (freeSlot != nullptr) freeSlot->epc = epc;
  return freeSlot;
}

bool validEpc(const String &epc) {
  if (epc.length() < 2 || epc.length() > 64 || epc.length() % 2 != 0) return false;
  for (size_t i = 0; i < epc.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(epc[i]))) return false;
  }
  return true;
}

String stringField(const String &json, const char *key) {
  const String prefix = String("\"") + key + "\":\"";
  const int start = json.indexOf(prefix);
  if (start < 0) return "";
  const int valueStart = start + prefix.length();
  const int valueEnd = json.indexOf('"', valueStart);
  return valueEnd < 0 ? String() : json.substring(valueStart, valueEnd);
}

bool intField(const String &json, const char *key, int &value) {
  const String prefix = String("\"") + key + "\":";
  const int start = json.indexOf(prefix);
  if (start < 0) return false;
  int position = start + prefix.length();
  bool negative = false;
  if (position < static_cast<int>(json.length()) && json[position] == '-') {
    negative = true;
    ++position;
  }
  if (position >= static_cast<int>(json.length()) || !isDigit(json[position])) return false;
  long parsed = 0;
  while (position < static_cast<int>(json.length()) && isDigit(json[position])) {
    parsed = parsed * 10 + (json[position++] - '0');
  }
  value = static_cast<int>(negative ? -parsed : parsed);
  return true;
}

void receiveRelayPackets() {
  if (!loraReady || nodeIndex <= 0) return;
  const int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;
  if (packetSize > static_cast<int>(kMaxLoRaPacket)) {
    while (LoRa.available()) LoRa.read();
    return;
  }
  String packet;
  packet.reserve(packetSize + 1);
  while (LoRa.available()) packet += static_cast<char>(LoRa.read());
  LoRa.receive();
  if (!packet.startsWith("{\"v\":2,") || !packet.endsWith("}")) return;
  if (stringField(packet, "from") != kNodeNames[nodeIndex - 1]) return;

  int receivedSequence = 0;
  if (intField(packet, "seq", receivedSequence) && receivedSequence > 0) {
    const uint32_t current = static_cast<uint32_t>(receivedSequence);
    if (upstreamSequenceKnown && current > upstreamSequence + 1) {
      upstreamMissed += current - upstreamSequence - 1;
    }
    upstreamSequence = current;
    upstreamSequenceKnown = true;
  }
  lastUpstreamPacket = millis();
  upstreamRssi = LoRa.packetRssi();
  upstreamSnr10 = static_cast<int>(LoRa.packetSnr() * 10);

  const String event = stringField(packet, "event");
  if (event == "heartbeat") {
    nextRelaySlotAt = millis() + kRelaySlotWaitMs;
    relaySlotPending = true;
    int readers = 0;
    if (!intField(packet, "readers", readers)) return;
    const uint8_t allowedMask = static_cast<uint8_t>((1U << nodeIndex) - 1U);
    upstreamReadersMask = static_cast<uint8_t>(readers) & allowedMask;
    if (nodeIndex == 2) {
      int value = 0;
      if (intField(packet, "rx", value)) link12Rssi = value;
      if (intField(packet, "sn", value)) link12Snr10 = value;
      if (intField(packet, "miss", value)) link12Missed = value;
      if (intField(packet, "age", value)) link12AgeMs = value;
    }
    const unsigned long now = millis();
    for (uint8_t source = 0; source < nodeIndex; ++source) {
      if (upstreamReadersMask & (1U << source)) readerHeartbeatSeen[source] = now;
    }
    return;
  }
  if (event != "tag") return;

  const String epc = stringField(packet, "epc");
  int receivedMask = 0;
  if (!validEpc(epc) || !intField(packet, "mask", receivedMask)) return;
  TagState *tag = findTag(epc);
  if (tag == nullptr) return;
  const uint8_t allowedMask = static_cast<uint8_t>((1U << nodeIndex) - 1U);
  const uint8_t newMask = static_cast<uint8_t>(receivedMask) & allowedMask;
  int receivedPath = 0;
  if (!intField(packet, "path", receivedPath)) {
    receivedPath = receivedMask | (1U << (nodeIndex - 1));
  }
  const uint8_t newRouteMask = static_cast<uint8_t>(receivedPath) & allowedMask;
  const bool changed = tag->upstreamMask != newMask;
  const bool routeChanged = tag->upstreamRouteMask != newRouteMask;
  tag->upstreamMask = newMask;
  tag->upstreamRouteMask = newRouteMask;
  const unsigned long now = millis();
  for (uint8_t source = 0; source < nodeIndex; ++source) {
    if ((newMask & (1U << source)) == 0) continue;
    int sourceRssi = 0;
    const String key = "r" + String(source);
    if (intField(packet, key.c_str(), sourceRssi) && sourceRssi >= -128 && sourceRssi <= 127) {
      tag->rssi[source] = static_cast<int8_t>(sourceRssi);
    }
    tag->sourceLastSeen[source] = now;
  }
  if (changed || routeChanged) markDirty(*tag);
  lastEpc = epc;
}

void onTag(const String &epc, int8_t rssi) {
  TagState *tag = findTag(epc);
  if (tag == nullptr || nodeIndex < 0) {
    Serial.println("RFID tag table full or unknown relay node");
    return;
  }
  const bool becameActive = !tag->localActive;
  tag->localActive = true;
  tag->localLastSeen = millis();
  tag->sourceLastSeen[nodeIndex] = millis();
  tag->rssi[nodeIndex] = rssi;
  if (becameActive) markDirty(*tag);
  lastEpc = epc;
  Serial.printf("TAG_DETECTED,%s,%d\n", epc.c_str(), static_cast<int>(rssi));
  showStatus("TAG DETECTED");
  if (oledReady) tagFlash.trigger(display);
}

void parseFrame(const uint8_t *data, size_t length) {
  if (length < 7 || data[0] != 0xBB || data[length - 1] != 0x7E) return;
  const size_t payloadLength = (static_cast<size_t>(data[3]) << 8) | data[4];
  if (length != payloadLength + 7) return;
  uint8_t checksum = 0;
  for (size_t i = 1; i < 5 + payloadLength; ++i) checksum += data[i];
  if (checksum != data[5 + payloadLength]) return;
  readerSeen = true;
  lastReaderFrame = millis();
  if (data[1] != 0x02 || data[2] != 0x22 || payloadLength < 6) return;
  const size_t epcLength = payloadLength - 5;
  if (epcLength == 0 || epcLength > 32) return;
  String epc;
  epc.reserve(epcLength * 2);
  constexpr char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < epcLength; ++i) {
    const uint8_t value = data[8 + i];
    epc += hex[value >> 4];
    epc += hex[value & 0x0F];
  }
  onTag(epc, static_cast<int8_t>(data[5]));
}

void readRfid() {
  while (rfidSerial.available()) {
    const uint8_t value = static_cast<uint8_t>(rfidSerial.read());
    if (frameLength == 0 && value != 0xBB) continue;
    if (frameLength >= kMaxFrame) {
      frameLength = 0;
      expectedFrameLength = 0;
      continue;
    }
    frame[frameLength++] = value;
    if (frameLength == 5) {
      expectedFrameLength =
          ((static_cast<size_t>(frame[3]) << 8) | frame[4]) + 7;
      if (expectedFrameLength > kMaxFrame) {
        frameLength = 0;
        expectedFrameLength = 0;
      }
    }
    if (expectedFrameLength != 0 && frameLength == expectedFrameLength) {
      parseFrame(frame, frameLength);
      frameLength = 0;
      expectedFrameLength = 0;
    }
  }
}

void expireSources() {
  const unsigned long now = millis();
  for (TagState &tag : tags) {
    if (tag.epc.length() == 0) continue;
    if (tag.localActive && now - tag.localLastSeen >= kTagLostMs) {
      tag.localActive = false;
      markDirty(tag);
      Serial.printf("TAG_LOST,%s\n", tag.epc.c_str());
    }
    for (uint8_t source = 0; source < static_cast<uint8_t>(nodeIndex > 0 ? nodeIndex : 0); ++source) {
      const uint8_t bit = static_cast<uint8_t>(1U << source);
      if ((tag.upstreamMask & bit) == 0 ||
          now - tag.sourceLastSeen[source] < kUpstreamLostMs) {
        continue;
      }
      tag.upstreamMask &= ~bit;
      markDirty(tag);
    }
  }
  for (uint8_t source = 0; source < static_cast<uint8_t>(nodeIndex > 0 ? nodeIndex : 0); ++source) {
    const uint8_t bit = static_cast<uint8_t>(1U << source);
    if ((upstreamReadersMask & bit) != 0 &&
        now - readerHeartbeatSeen[source] >= kReaderHeartbeatLostMs) {
      upstreamReadersMask &= ~bit;
    }
  }
}

void serviceForwarding() {
  if (!loraReady || nodeIndex < 0) return;
  const unsigned long now = millis();
  if (nodeIndex == 0) {
    if (static_cast<long>(now - nextRootFrameAt) >= 0) {
      slotStartedAt = now;
      tagSentInSlot = false;
      nextRootFrameAt = now + kFrameMs;
      forwardHeartbeat();
      heartbeatFinishedAt = millis();
      return;
    }
  } else if (relaySlotPending && static_cast<long>(now - nextRelaySlotAt) >= 0) {
    relaySlotPending = false;
    slotStartedAt = now;
    tagSentInSlot = false;
    nextRelaySlotAt = now + kFrameMs;
    relaySlotPending = true;
    forwardHeartbeat();
    heartbeatFinishedAt = millis();
    return;
  }
  const unsigned long slotLength = nodeIndex == 2 ? kFinalSlotMs : kSlotMs;
  const unsigned long tagGap = nodeIndex == 2 ? kFinalTagGapMs : kTagAfterHeartbeatGapMs;
  if (slotStartedAt == 0 || tagSentInSlot || now - slotStartedAt >= slotLength ||
      now - heartbeatFinishedAt < tagGap) return;
  for (size_t offset = 0; offset < kMaxTags; ++offset) {
    const size_t index = (nextTagIndex + offset) % kMaxTags;
    TagState &tag = tags[index];
    if (tag.epc.length() == 0) continue;
    const uint8_t mask = aggregateMask(tag);
    const bool mergedChangeReady = tag.dirty && now - tag.dirtySince >= kMergeWindowMs;
    const bool retryReady = !tag.dirty && tag.retryRemaining != 0;
    const bool refreshReady = mask != 0 && now - tag.lastForward >= kForwardIntervalMs;
    if (tag.dirty ? mergedChangeReady : (retryReady || refreshReady)) {
      const String packet = tagPacket(tag, sequence + 1);
      if (packet.length() > kMaxLoRaPacket ||
          now - slotStartedAt + airtimeMs(packet.length()) + kSlotGuardMs > slotLength) {
        continue;
      }
      forwardTag(tag, packet);
      tagSentInSlot = true;
      nextTagIndex = (index + 1) % kMaxTags;
      return;
    }
  }
}

void startWifi() {
  lastWifiAttempt = millis();
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void startOta() {
  ArduinoOTA.setHostname(hostname.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("OTA started"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA complete"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA error: %u\n", error); });
  ArduinoOTA.begin();
  otaRunning = true;
  Serial.printf("OTA ready: %s.local (%s)\n", hostname.c_str(),
                WiFi.localIP().toString().c_str());
  showStatus("OTA ready");
}

void serviceNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!otaRunning) startOta();
    ArduinoOTA.handle();
    return;
  }
  if (otaRunning) {
    ArduinoOTA.end();
    otaRunning = false;
  }
  if (millis() - lastWifiAttempt >= kWifiRetryMs) {
    startWifi();
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  const String suffix = mac.substring(mac.length() - 6);
  hostname = "playce-" + suffix;
  for (uint8_t i = 0; i < 3; ++i) {
    if (suffix == kNodeSuffixes[i]) nodeIndex = i;
  }
  WiFi.setHostname(hostname.c_str());

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() == 0) {
    oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  }
  Serial.printf("Device: %s, relay index: %d\nMAC: %s\n", hostname.c_str(),
                static_cast<int>(nodeIndex), WiFi.macAddress().c_str());
  if (nodeIndex < 0) Serial.println("This MAC is not configured in the relay chain");

  rfidSerial.begin(115200, SERIAL_8N1, kRfidRxPin, kRfidTxPin);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);
  startLoRa();
  startWifi();
  if (nodeIndex > 0) {
    nextRelaySlotAt = millis() + kSlotMs * nodeIndex;
    relaySlotPending = true;
  }
}

void loop() {
  serviceNetwork();
  if (!loraReady && millis() - lastLoRaAttempt >= kLoRaRetryMs) startLoRa();
  receiveRelayPackets();
  readRfid();
  const unsigned long now = millis();
  if (now - lastInventory >= kInventoryIntervalMs) {
    rfidSerial.write(kInventoryCommand, sizeof(kInventoryCommand));
    lastInventory = now;
  }
  expireSources();
  serviceForwarding();
  if (oledReady) tagFlash.update(display);
  delay(2);
}
