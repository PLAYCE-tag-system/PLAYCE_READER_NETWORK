#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Adafruit_SSD1306.h>
#include <LoRa.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

#include "ota_config.h"
#include "oled_tag_flash.h"
#include "receiver_label.h"

namespace {
constexpr long kLoRaFrequencyHz = 915000000;
constexpr uint8_t kLoRaSyncWord = 0x42;
constexpr unsigned long kWifiRetryMs = 10000;
constexpr unsigned long kLoRaRetryMs = 10000;
constexpr unsigned long kGatewayReportMs = 1000;
constexpr unsigned long kRelayTagDelayMs = 6000;
constexpr unsigned long kRfidInventoryIntervalMs = 500;
constexpr unsigned long kRfidTagLostMs = 3000;
constexpr unsigned long kRfidRepeatReportMs = 1000;
constexpr uint16_t kCollectorUdpPort = 8766;
constexpr size_t kMaxTags = 64;
constexpr size_t kMaxLoRaPacket = 240;
constexpr int kRfidRxPin = 34;
constexpr int kRfidTxPin = 4;
constexpr size_t kMaxRfidFrame = 128;
const uint8_t kInventoryCommand[] = {0xBB, 0x00, 0x27, 0x00, 0x03,
                                     0x22, 0xFF, 0xFF, 0x4A, 0x7E};
// 15 dBm is the lowest transmit power supported by this UHF command protocol.
const uint8_t kSetRfidPowerCommand[] = {0xBB, 0x00, 0xB6, 0x00, 0x02,
                                        0x05, 0xDC, 0x99, 0x7E};
const uint8_t kGetRfidPowerCommand[] = {0xBB, 0x00, 0xB7, 0x00,
                                        0x00, 0xB7, 0x7E};
constexpr uint16_t kRfidPowerCdbm = 1500;
const char *const kFinalRelay = "playce-cb21f8";
const char *const kReaderNames[] = {"playce-92f6ac", "playce-ca3490",
                                    "playce-cb21f8"};

struct ReceivedTag {
  String epc;
  uint8_t mask = 0;
  uint8_t routeMask = 0;
  int8_t rssi[3] = {0, 0, 0};
  unsigned long lastSeen = 0;
};

struct DirectTag {
  String epc;
  unsigned long lastSeen = 0;
  unsigned long lastReported = 0;
  bool active = false;
};

Adafruit_SSD1306 display(128, 64, &Wire, -1);
OledTagFlash tagFlash;
HardwareSerial rfidSerial(2);
WiFiUDP wifiUdp;
ReceivedTag tags[kMaxTags];
DirectTag directTags[kMaxTags];
uint8_t rfidFrame[kMaxRfidFrame];
size_t rfidFrameLength = 0;
size_t expectedRfidFrameLength = 0;
String hostname;
String lastSender;
String lastEpc;
bool oledReady = false;
bool otaRunning = false;
bool loraReady = false;
bool udpReady = false;
bool rfidPowerSetSent = false;
bool rfidPowerReadSent = false;
bool rfidPowerVerified = false;
uint16_t rfidReportedPowerCdbm = 0;
uint32_t packetCount = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastLoRaAttempt = 0;
unsigned long lastGatewayReport = 0;
unsigned long lastRfidInventory = 0;
unsigned long lastRfidPowerAttempt = 0;
unsigned long lastRelayPacket = 0;
uint32_t relayMissed = 0;
uint32_t relaySequence = 0;
bool relaySequenceKnown = false;
uint32_t bootId = 0;
uint32_t messageId = 0;

void publish(String message) {
  if (!message.endsWith("}")) return;
  message.remove(message.length() - 1);
  message += ",\"boot_id\":" + String(bootId) +
             ",\"msg_id\":" + String(++messageId) + "}";
  Serial.println(message);
  if (!udpReady || WiFi.status() != WL_CONNECTED) return;
  const IPAddress ip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (uint8_t index = 0; index < 4; ++index) {
    broadcast[index] = ip[index] | static_cast<uint8_t>(~mask[index]);
  }
  if (wifiUdp.beginPacket(broadcast, kCollectorUdpPort)) {
    wifiUdp.print(message);
    wifiUdp.endPacket();
  }
}

void showStatus(const char *status) {
  if (!oledReady) return;
  display.clearDisplay();
  display.drawBitmap(0, 0, kReceiverLabel, kReceiverLabelWidth,
                     kReceiverLabelHeight, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 21);
  display.println(hostname);
  display.println(status);
  if (lastSender.length()) display.println(lastSender);
  if (lastEpc.length()) display.println(lastEpc.substring(0, 20));
  display.display();
}

void reportGateway() {
  String message = "{\"v\":1,\"device\":\"" + hostname +
                   "\",\"event\":\"gateway\",\"uptime_ms\":" + String(millis()) +
                   ",\"packets\":" + String(packetCount) +
                   ",\"radio_ready\":" + (loraReady ? "true" : "false") +
                   ",\"relay_age_ms\":" +
                   String(lastRelayPacket ? millis() - lastRelayPacket : 0UL) +
                   ",\"wifi_connected\":" +
                   (WiFi.status() == WL_CONNECTED ? "true" : "false") +
                   ",\"wifi_ip\":\"" +
                   (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") +
                   "\",\"wifi_rssi\":" +
                   String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -200) +
                   ",\"rfid_power_cdbm\":" + String(rfidReportedPowerCdbm) +
                   ",\"rfid_power_verified\":" +
                   (rfidPowerVerified ? "true" : "false") + "}";
  publish(message);
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

bool validEpc(const String &epc) {
  if (epc.length() < 2 || epc.length() > 64 || epc.length() % 2 != 0) return false;
  for (size_t i = 0; i < epc.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(epc[i]))) return false;
  }
  return true;
}

DirectTag *findDirectTag(const String &epc) {
  DirectTag *freeSlot = nullptr;
  for (DirectTag &tag : directTags) {
    if (tag.epc == epc) return &tag;
    if (tag.epc.isEmpty() && freeSlot == nullptr) freeSlot = &tag;
  }
  if (freeSlot != nullptr) freeSlot->epc = epc;
  return freeSlot;
}

void emitDirectTag(const DirectTag &tag, const char *event, int rssi = 0) {
  if (strcmp(event, "seen") == 0) {
    Serial.printf("DIRECT_TAG_DETECTED,%s,%d\n", tag.epc.c_str(), rssi);
  } else {
    Serial.printf("DIRECT_TAG_LOST,%s\n", tag.epc.c_str());
  }
  String message = "{\"v\":1,\"device\":\"" + hostname +
                   "\",\"event\":\"" + event + "\",\"epc\":\"" + tag.epc +
                   "\",\"gateway\":\"" + hostname +
                   "\",\"tag_source\":\"direct_uhf\",\"relay_path\":\"\"";
  if (strcmp(event, "seen") == 0) message += ",\"rssi\":" + String(rssi);
  message += "}";
  publish(message);
  lastEpc = tag.epc;
}

void onDirectTag(const String &epc, int rssi) {
  DirectTag *tag = findDirectTag(epc);
  if (tag == nullptr) return;
  const unsigned long now = millis();
  const bool shouldReport = !tag->active || now - tag->lastReported >= kRfidRepeatReportMs;
  tag->active = true;
  tag->lastSeen = now;
  if (shouldReport) {
    tag->lastReported = now;
    emitDirectTag(*tag, "seen", rssi);
    showStatus("Direct UHF tag read");
    if (oledReady) tagFlash.trigger(display);
  }
}

uint8_t calcChecksum(const uint8_t *data, size_t start, size_t end) {
  uint16_t sum = 0;
  for (size_t i = start; i <= end; ++i) sum += data[i];
  return static_cast<uint8_t>(sum & 0xFF);
}

void parseRfidFrame(const uint8_t *data, size_t length) {
  if (length < 7 || data[0] != 0xBB || data[length - 1] != 0x7E) return;
  const size_t payloadLength = (static_cast<size_t>(data[3]) << 8) | data[4];
  if (length != payloadLength + 7 || payloadLength > 37) return;
  if (data[5 + payloadLength] != calcChecksum(data, 1, 4 + payloadLength)) return;

  if (data[1] == 0x01 && data[2] == 0xB6 && payloadLength == 1) {
    Serial.printf("UHF_POWER_SET,%s\n", data[5] == 0 ? "OK" : "FAILED");
    return;
  }
  if (data[1] == 0x01 && data[2] == 0xB7 && payloadLength == 2) {
    rfidReportedPowerCdbm = (static_cast<uint16_t>(data[5]) << 8) | data[6];
    rfidPowerVerified = rfidReportedPowerCdbm == kRfidPowerCdbm;
    Serial.printf("UHF_POWER_READBACK,%u,verified=%u\n",
                  rfidReportedPowerCdbm, rfidPowerVerified ? 1 : 0);
    return;
  }
  if (payloadLength < 6 || !rfidPowerVerified) return;

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
  if (validEpc(epc)) onDirectTag(epc, static_cast<int8_t>(data[5]));
}

void readDirectRfid() {
  while (rfidSerial.available()) {
    const uint8_t value = static_cast<uint8_t>(rfidSerial.read());
    if (rfidFrameLength == 0 && value != 0xBB) continue;
    if (rfidFrameLength >= kMaxRfidFrame) {
      rfidFrameLength = 0;
      expectedRfidFrameLength = 0;
      continue;
    }
    rfidFrame[rfidFrameLength++] = value;
    if (rfidFrameLength == 5) {
      expectedRfidFrameLength =
          ((static_cast<size_t>(rfidFrame[3]) << 8) | rfidFrame[4]) + 7;
      if (expectedRfidFrameLength > kMaxRfidFrame) {
        rfidFrameLength = 0;
        expectedRfidFrameLength = 0;
      }
    }
    if (expectedRfidFrameLength != 0 &&
        rfidFrameLength == expectedRfidFrameLength) {
      parseRfidFrame(rfidFrame, rfidFrameLength);
      rfidFrameLength = 0;
      expectedRfidFrameLength = 0;
    }
  }
}

void configureRfidPower() {
  if (rfidPowerVerified) return;
  const unsigned long now = millis();
  if (!rfidPowerSetSent || now - lastRfidPowerAttempt >= 3000) {
    rfidSerial.write(kSetRfidPowerCommand, sizeof(kSetRfidPowerCommand));
    rfidPowerSetSent = true;
    rfidPowerReadSent = false;
    lastRfidPowerAttempt = now;
  } else if (!rfidPowerReadSent && now - lastRfidPowerAttempt >= 250) {
    rfidSerial.write(kGetRfidPowerCommand, sizeof(kGetRfidPowerCommand));
    rfidPowerReadSent = true;
  }
}

void expireDirectTags() {
  const unsigned long now = millis();
  for (DirectTag &tag : directTags) {
    if (!tag.active || now - tag.lastSeen < kRfidTagLostMs) continue;
    tag.active = false;
    emitDirectTag(tag, "lost");
  }
}

ReceivedTag *findTag(const String &epc) {
  ReceivedTag *freeSlot = nullptr;
  for (ReceivedTag &tag : tags) {
    if (tag.epc == epc) return &tag;
    if (tag.epc.length() == 0 && freeSlot == nullptr) freeSlot = &tag;
  }
  if (freeSlot != nullptr) freeSlot->epc = epc;
  return freeSlot;
}

void emitTag(uint8_t source, const char *event, const String &epc, int rssi,
             int loraRssi, uint8_t routeMask) {
  if (strcmp(event, "seen") == 0) {
    Serial.printf("LORA_RELAY_TAG_DETECTED,%s,%s,%d\n",
                  kReaderNames[source], epc.c_str(), rssi);
  } else {
    Serial.printf("LORA_RELAY_TAG_%s,%s,%s\n",
                  strcmp(event, "lost") == 0 ? "LOST" : "DELAYED",
                  kReaderNames[source], epc.c_str());
  }
  String route;
  for (uint8_t hop = 0; hop < 3; ++hop) {
    if ((routeMask & (1U << hop)) == 0) continue;
    if (route.length()) route += ">";
    route += String(hop + 1);
  }
  if (route.length() == 0) route = "unknown";
  String message = "{\"v\":1,\"device\":\"" + String(kReaderNames[source]) +
                   "\",\"event\":\"" + event + "\",\"epc\":\"" + epc +
                   "\",\"tag_source\":\"lora_relay\"" +
                   ",\"origin_reader\":\"" + kReaderNames[source] +
                   "\",\"relay_path\":\"" + route + "\"";
  if (strcmp(event, "seen") == 0) message += ",\"rssi\":" + String(rssi);
  message += ",\"gateway\":\"" + hostname +
             "\",\"relay\":\"" + kFinalRelay +
             "\",\"lora_rssi\":" + String(loraRssi) + "}";
  publish(message);
}

void emitHeartbeat(uint8_t source, bool readerSeen, int loraRssi,
                   int loraSnr10, int missed, int ageMs) {
  const String message = "{\"v\":1,\"device\":\"" + String(kReaderNames[source]) +
                         "\",\"event\":\"heartbeat\",\"reader_seen\":" +
                         (readerSeen ? "true" : "false") +
                         ",\"gateway\":\"" + hostname +
                         "\",\"relay\":\"" + kFinalRelay +
                         "\",\"lora_rssi\":" + String(loraRssi) +
                         ",\"lora_snr10\":" + String(loraSnr10) +
                         ",\"missed\":" + String(missed) +
                         ",\"hop_age_ms\":" + String(ageMs) + "}";
  publish(message);
}

void processTagPacket(const String &packet, int loraRssi) {
  const String epc = stringField(packet, "epc");
  int parsedMask = 0;
  if (!validEpc(epc) || !intField(packet, "mask", parsedMask)) return;
  ReceivedTag *tag = findTag(epc);
  if (tag == nullptr) return;
  const uint8_t newMask = static_cast<uint8_t>(parsedMask) & 0x07;
  int parsedRoute = 0;
  if (!intField(packet, "path", parsedRoute)) {
    parsedRoute = parsedMask | (1U << 2);
  }
  const uint8_t newRouteMask = static_cast<uint8_t>(parsedRoute) & 0x07;
  const uint8_t oldMask = tag->mask;
  for (uint8_t source = 0; source < 3; ++source) {
    const uint8_t bit = static_cast<uint8_t>(1U << source);
    if (newMask & bit) {
      int rssi = tag->rssi[source];
      const String key = "r" + String(source);
      if (intField(packet, key.c_str(), rssi) && rssi >= -128 && rssi <= 127) {
        tag->rssi[source] = static_cast<int8_t>(rssi);
      }
      emitTag(source, "seen", epc, tag->rssi[source], loraRssi, newRouteMask);
    } else if (oldMask & bit) {
      emitTag(source, "lost", epc, 0, loraRssi, newRouteMask);
    }
  }
  tag->mask = newMask;
  tag->routeMask = newRouteMask;
  tag->lastSeen = millis();
  lastEpc = epc;
}

void expireRelayTags() {
  const unsigned long now = millis();
  for (ReceivedTag &tag : tags) {
    if (tag.epc.length() == 0 || tag.mask == 0 ||
        now - tag.lastSeen < kRelayTagDelayMs) {
      continue;
    }
    for (uint8_t source = 0; source < 3; ++source) {
      if (tag.mask & (1U << source)) {
        emitTag(source, "delayed", tag.epc, 0, -200, tag.routeMask);
      }
    }
    tag.mask = 0;
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
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA error: %u\n", error); });
  ArduinoOTA.begin();
  otaRunning = true;
  Serial.printf("Receiver OTA ready: %s.local (%s)\n", hostname.c_str(),
                WiFi.localIP().toString().c_str());
}

void serviceWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!otaRunning) startOta();
    if (!udpReady) udpReady = wifiUdp.begin(0);
    ArduinoOTA.handle();
    return;
  }
  if (udpReady) {
    wifiUdp.stop();
    udpReady = false;
  }
  if (otaRunning) {
    ArduinoOTA.end();
    otaRunning = false;
  }
  if (millis() - lastWifiAttempt >= kWifiRetryMs) {
    startWifi();
  }
}

void startLoRa() {
  lastLoRaAttempt = millis();
  loraReady = LoRa.begin(kLoRaFrequencyHz);
  if (loraReady) {
    LoRa.setSyncWord(kLoRaSyncWord);
    LoRa.enableCrc();
    LoRa.receive();
    Serial.println("LoRa chain receiver ready at 915 MHz");
    showStatus("LoRa RX ready");
  } else {
    Serial.println("LoRa receiver init failed");
    showStatus("LoRa RX failed");
  }
  reportGateway();
}

void receivePackets() {
  if (!loraReady) return;
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
  if (stringField(packet, "from") != kFinalRelay) return;

  const int loraRssi = LoRa.packetRssi();
  const int loraSnr10 = static_cast<int>(LoRa.packetSnr() * 10);
  int receivedSequence = 0;
  if (intField(packet, "seq", receivedSequence) && receivedSequence > 0) {
    const uint32_t current = static_cast<uint32_t>(receivedSequence);
    if (relaySequenceKnown && current > relaySequence + 1) {
      relayMissed += current - relaySequence - 1;
    }
    relaySequence = current;
    relaySequenceKnown = true;
  }
  lastRelayPacket = millis();
  const String event = stringField(packet, "event");
  lastSender = kFinalRelay;
  ++packetCount;
  if (event == "tag") {
    processTagPacket(packet, loraRssi);
  } else if (event == "heartbeat") {
    int readers = 0;
    if (!intField(packet, "readers", readers)) return;
    int r12 = -200, s12 = 0, m12 = 0, a12 = 0;
    int r23 = -200, s23 = 0, m23 = 0, a23 = 0;
    intField(packet, "r12", r12);
    intField(packet, "s12", s12);
    intField(packet, "m12", m12);
    intField(packet, "a12", a12);
    intField(packet, "rx", r23);
    intField(packet, "sn", s23);
    intField(packet, "miss", m23);
    intField(packet, "age", a23);
    emitHeartbeat(0, readers & 0x01, r12, s12, m12, a12);
    emitHeartbeat(1, readers & 0x02, r23, s23, m23, a23);
    emitHeartbeat(2, readers & 0x04, loraRssi, loraSnr10,
                  static_cast<int>(relayMissed), 0);
  }
  showStatus("Relay packet received");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  rfidSerial.begin(115200, SERIAL_8N1, kRfidRxPin, kRfidTxPin);
  bootId = esp_random();
  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  hostname = "playce-" + mac.substring(mac.length() - 6);
  WiFi.setHostname(hostname.c_str());

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() == 0) {
    oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  }
  Serial.printf("Receiver: %s\nMAC: %s\n", hostname.c_str(),
                WiFi.macAddress().c_str());
  showStatus("Starting");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);
  startLoRa();
  startWifi();
}

void loop() {
  serviceWifi();
  if (!loraReady && millis() - lastLoRaAttempt >= kLoRaRetryMs) startLoRa();
  receivePackets();
  expireRelayTags();
  readDirectRfid();
  expireDirectTags();
  configureRfidPower();
  if (rfidPowerVerified &&
      millis() - lastRfidInventory >= kRfidInventoryIntervalMs) {
    rfidSerial.write(kInventoryCommand, sizeof(kInventoryCommand));
    lastRfidInventory = millis();
  }
  if (millis() - lastGatewayReport >= kGatewayReportMs) {
    reportGateway();
    lastGatewayReport = millis();
  }
  if (oledReady) tagFlash.update(display);
  delay(2);
}
