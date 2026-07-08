#include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ==========================================
// ⚙️ 사용자 설정 영역
// ==========================================
// ★ 이 기기의 역할을 번호로 입력하세요.
// 0 = 최종 수신기 / 1 = 1번 송신기(중계) / 2 = 2번 송신기 / 3 = 3번 송신기
const int MY_ID = 1; 

// 전체 기기의 MAC 주소 목록
uint8_t macList[4][6] = {
  {0x78, 0x1C, 0x3C, 0xF5, 0xE4, 0x68}, // [0] 0번 기기 (최종 수신기)
  {0xF4, 0x65, 0x0B, 0xD8, 0xBC, 0x70}, // [1] 1번 기기 (중계 겸 송신기)
  {0x88, 0x57, 0x21, 0x95, 0x4B, 0xD0}, // [2] 2번 기기 (말단 송신기)
  {0x88, 0x57, 0x21, 0x95, 0x37, 0x80}  // [3] 3번 기기 (말단 송신기, 주소 비워둠)
};

// --- 핀 설정 ---
#define MCU_RX_PIN 4   // RFID TX와 연결
#define MCU_TX_PIN 5   // RFID RX와 연결
#define LED_PIN 25     // 네오픽셀 LED 핀
#define LED_COUNT 70   // 네오픽셀 LED 개수
// ==========================================

// --- 네오픽셀 객체 및 변수 ---
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

bool effectRunning = false;
unsigned long effectStartTime = 0;
const unsigned long effectDuration = 3000;
int rainbowOffset = 0;
unsigned long lastRainbowTime = 0;
const unsigned long rainbowInterval = 15;

// --- RFID 및 통신 변수 ---
HardwareSerial rfidSerial(2);

const uint8_t rfidCommand[] = {
  0xBB, 0x00, 0x27, 0x00, 0x03,
  0x22, 0xFF, 0xFF,
  0x4A, 0x7E
};

uint8_t rxBuf[256];
int rxLen = 0;
unsigned long lastInventoryMillis = 0;
unsigned long lastEpcMillis = 0;
unsigned long lastSendMillis = 0;
const unsigned long inventoryIntervalMs = 500;
const unsigned long sameTagPrintIntervalMs = 1000;
String lastEpc = "";

typedef struct struct_message {
  int original_id;
  int relay_id;
  int rssi;
  char epc[33]; 
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

// 함수 사전 선언
void sendInventoryCommand();
void parseFrame(uint8_t *frame, int len);
uint8_t calcChecksum(uint8_t *data, int start, int end);
String bytesToHex(uint8_t *data, int len);
void setAllPixels(int r, int g, int b);
void startFadeEffect();

// 📡 [통신] 데이터 수신 콜백 (중계 기능 수행)
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  struct_message receivedData;
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  
  if (MY_ID == 0) {
    Serial.println("=============================");
    if (receivedData.relay_id == 0) {
      Serial.printf("[알림] %d번 기기가 직접 보냈습니다.\n", receivedData.original_id);
    } else {
      Serial.printf("[알림] %d번 기기가 %d번을 통해 보냈습니다.\n", receivedData.original_id, receivedData.relay_id);
    }
    Serial.printf(" - EPC 태그: %s\n", receivedData.epc);
    Serial.printf(" - 신호 강도(RSSI): %d\n", receivedData.rssi);
    
    // 수신기가 외부 데이터를 받았을 때도 불빛을 내고 싶다면 아래 주석을 해제하세요.
    // startFadeEffect(); 
  } 
  else if (MY_ID == 1) {
    // 하드웨어가 연결되어 있지 않아도 백그라운드에서 중계 처리됨
    receivedData.relay_id = MY_ID; 
    esp_now_send(macList[0], (uint8_t *) &receivedData, sizeof(receivedData));
    Serial.printf(">> %d번 기기의 데이터를 수신기로 중계 완료!\n", receivedData.original_id);
  }
}

// 🌈 [LED] 전체 픽셀 색상 설정
void setAllPixels(int r, int g, int b) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

// 🌈 [LED] 시작 시 파스텔 노란색 브리딩 효과
void startupBreathingEffect() {
  int maxR = 255, maxG = 255, maxB = 255; 
  int breathCount = 3, breathDuration = 1200;

  for (int n = 0; n < breathCount; n++) {
    unsigned long startTime = millis();
    while (millis() - startTime < breathDuration) {
      float progress = (float)(millis() - startTime) / breathDuration;
      float fade = sin(progress * PI);
      setAllPixels(maxR * fade, maxG * fade, maxB * fade);
      delay(15);
    }
    setAllPixels(0, 0, 0);
    delay(120);
  }
}

// 🌈 [LED] 무지개 색상 반환 함수
uint32_t rainbowColor(byte pos) {
  pos = 255 - pos;
  if (pos < 85) return strip.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return strip.Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170; return strip.Color(pos * 3, 255 - pos * 3, 0);
}

// 🌈 [LED] 무지개 페이드 효과 시작 트리거
void startFadeEffect() {
  effectRunning = true;
  effectStartTime = millis();
  rainbowOffset = 0;
}

// 🌈 [LED] 무지개 페이드 애니메이션 업데이트 (비동기 처리)
void updateRainbowFadeEffect() {
  if (!effectRunning) return;

  unsigned long elapsed = millis() - effectStartTime;
  if (elapsed >= effectDuration) {
    effectRunning = false;
    setAllPixels(0, 0, 0);
    return;
  }

  float progress = (float)elapsed / effectDuration;
  float fade = sin(progress * PI);

  if (millis() - lastRainbowTime >= rainbowInterval) {
    lastRainbowTime = millis();
    rainbowOffset += 3;
  }

  for (int i = 0; i < LED_COUNT; i++) {
    byte colorIndex = (i * 256 / LED_COUNT + rainbowOffset) & 255;
    uint32_t color = rainbowColor(colorIndex);
    
    int r = ((uint8_t)(color >> 16)) * fade;
    int g = ((uint8_t)(color >> 8)) * fade;
    int b = ((uint8_t)(color)) * fade;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // 1. 통신 모듈 초기화 (LED 동작 전 미리 네트워크를 열어 중계 지연 방지)
  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    if (MY_ID == 0) {
      esp_now_register_recv_cb(OnDataRecv);
    } else if (MY_ID == 1) {
      memcpy(peerInfo.peer_addr, macList[0], 6);
      peerInfo.channel = 0; peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
      esp_now_register_recv_cb(OnDataRecv);
    } else if (MY_ID >= 2) {
      memcpy(peerInfo.peer_addr, macList[1], 6);
      peerInfo.channel = 0; peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
  }

  // 2. 하드웨어 초기화 (연결 안 되어 있어도 코드는 통과됨)
  rfidSerial.begin(115200, SERIAL_8N1, MCU_RX_PIN, MCU_TX_PIN);
  
  strip.begin();
  strip.setBrightness(255);
  strip.show();

  Serial.println("시스템 시작: 통신 + RFID + LED 활성화");

  // 3. 시작 LED 효과 실행
  startupBreathingEffect();
  setAllPixels(0, 0, 0);

  // 첫 스캔 명령 전송
  sendInventoryCommand();
  lastInventoryMillis = millis();
}

void loop() {
  // 1. RFID 스캔 명령 전송 (500ms 간격)
  if (millis() - lastInventoryMillis >= inventoryIntervalMs) {
    sendInventoryCommand();
    lastInventoryMillis = millis();
  }

  // 2. RFID 데이터 수신 및 파싱 (비동기)
  while (rfidSerial.available()) {
    uint8_t b = rfidSerial.read();
    if (rxLen == 0 && b != 0xBB) continue;

    if (rxLen < sizeof(rxBuf)) rxBuf[rxLen++] = b;
    else rxLen = 0;

    if (b == 0x7E && rxLen >= 7) {
      parseFrame(rxBuf, rxLen);
      rxLen = 0;
    }
  }

  // 3. LED 애니메이션 업데이트 (비동기)
  updateRainbowFadeEffect();

  delay(2); // Watchdog 방지용 미세 대기
}

// --- RFID 파싱 및 통신 전송 함수 ---
void sendInventoryCommand() {
  rfidSerial.write(rfidCommand, sizeof(rfidCommand));
}

void parseFrame(uint8_t *frame, int len) {
  if (len < 7 || frame[0] != 0xBB || frame[len - 1] != 0x7E) return;

  int payloadLen = ((int)frame[3] << 8) | frame[4];
  if (len != 7 + payloadLen) return;
  if (frame[5 + payloadLen] != calcChecksum(frame, 1, 4 + payloadLen)) return;

  uint8_t *payload = &frame[5];
  if (payloadLen < 6) return;

  int rssi = payload[0];
  int epcStart = 3;
  int epcLen = payloadLen - 5;

  if (epcLen <= 0 || epcLen > 32) return;

  String epc = bytesToHex(&payload[epcStart], epcLen);
  if (epc.length() == 0) return;

  bool isNewTag = (epc != lastEpc);
  bool intervalPassed = (millis() - lastSendMillis >= sameTagPrintIntervalMs);

  // 새로운 태그거나 지정된 쿨타임(1초)이 지났을 때 전송
  if (isNewTag || intervalPassed) {
    lastEpc = epc;
    lastSendMillis = millis();

    // ★ 성공적으로 태그를 읽었으므로 LED 효과 발동
    if (!effectRunning) {
      startFadeEffect();
    }

    if (MY_ID == 0) {
      Serial.println("=============================");
      Serial.println("[알림] 0번(최종 수신기)이 직접 태그를 읽었습니다.");
      Serial.printf(" - EPC 태그: %s\n", epc.c_str());
      Serial.printf(" - 신호 강도(RSSI): %d\n", rssi);
    } else {
      myData.original_id = MY_ID;
      myData.relay_id = 0;
      myData.rssi = rssi;
      epc.toCharArray(myData.epc, sizeof(myData.epc));

      uint8_t* targetMac = (MY_ID == 1) ? macList[0] : macList[1];
      esp_now_send(targetMac, (uint8_t *) &myData, sizeof(myData));
      Serial.printf("전송 성공 (ID: %d) -> EPC: %s\n", MY_ID, epc.c_str());
    }
  }
}

uint8_t calcChecksum(uint8_t *data, int start, int end) {
  uint16_t sum = 0;
  for (int i = start; i <= end; i++) sum += data[i];
  return sum & 0xFF;
}

String bytesToHex(uint8_t *data, int len) {
  String result = "";
  for (int i = 0; i < len; i++) {
    if (data[i] < 0x10) result += "0";
    result += String(data[i], HEX);
  }
  result.toUpperCase();
  return result;
}