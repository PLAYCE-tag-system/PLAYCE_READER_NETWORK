#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>

// 릴리고 LoRa 핀맵
#define SCK     5
#define MISO    19
#define MOSI    27
#define SS      18
#define RST     14
#define DIO0    26
#define BAND    915E6

// ==========================================
// ⚠️ 수정해야 할 부분 (와이파이 및 디스코드)
// ==========================================
const char* ssid = "";          // 스마트폰 핫스팟 이름
const char* password = "";  // 스마트폰 핫스팟 비밀번호
const char* discord_webhook = ""; 

// 중복 수신 방지용 히스토리
const int MAX_HISTORY = 30;
String msgHistory[MAX_HISTORY];
int historyIndex = 0;

void setup() {
  Serial.begin(115200);
  delay(5000);
  
  // 1. 와이파이 연결 시도 (최대 10초만 대기하도록 변경)
  Serial.print("\nConnecting to WiFi");
  WiFi.begin(ssid, password);
  
  int wifi_attempts = 0;
  // 20번 시도 (20 * 0.5초 = 10초)
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 20) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }
  
  Serial.println();
  
  // 10초가 지났을 때 와이파이 연결 성공 여부 체크
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("❌ WiFi Connection FAILED! (일단 와이파이 없이 LoRa 수신만 진행합니다)");
  }

  // 2. LoRa 초기화 (와이파이 연결 실패해도 무조건 실행됨)
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(BAND)) {
    Serial.println("LoRa init failed!");
    while (1);
  }
  Serial.println("Discord Gateway Ready. Waiting for data...");
}

bool isDuplicate(String uniqueMsgID) {
  for (int i = 0; i < MAX_HISTORY; i++) {
    if (msgHistory[i] == uniqueMsgID) return true;
  }
  return false;
}

void addToHistory(String uniqueMsgID) {
  msgHistory[historyIndex] = uniqueMsgID;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
}

// 디스코드로 메시지를 쏘는 함수
void sendToDiscord(String sender, String hops, String data, int rssi) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(discord_webhook);
    http.addHeader("Content-Type", "application/json");

    String content = "📡 **[LoRa Mesh] 새로운 데이터 수신!**\\n";
    content += "- **전송기기:** " + sender + "\\n";
    content += "- **중계횟수:** " + hops + " 번 뛰어넘음\\n";
    content += "- **상태정보:** " + data + "\\n";
    content += "- **신호강도:** " + String(rssi) + " dBm";
    
    String httpRequestData = "{\"content\":\"" + content + "\"}";
    
    int httpResponseCode = http.POST(httpRequestData);
    
    if (httpResponseCode > 0) {
      Serial.println("  [Discord] 메시지 전송 성공! (HTTP " + String(httpResponseCode) + ")");
    } else {
      Serial.println("  [Discord] ⚠️ 메시지 전송 실패... 에러: " + String(httpResponseCode));
    }
    http.end();
  } else {
    Serial.println("  [Discord] ❌ 와이파이가 연결되지 않아 디스코드로 보낼 수 없습니다.");
  }
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    
    int firstComma = incoming.indexOf(',');
    int secondComma = incoming.indexOf(',', firstComma + 1);
    int thirdComma = incoming.indexOf(',', secondComma + 1);
    
    if (firstComma > 0 && secondComma > 0 && thirdComma > 0) {
      String sourceID = incoming.substring(0, firstComma);
      String msgID = incoming.substring(firstComma + 1, secondComma);
      String hops = incoming.substring(secondComma + 1, thirdComma);
      String data = incoming.substring(thirdComma + 1);
      
      String uniqueMsgID = sourceID + "_" + msgID;
      
      if (!isDuplicate(uniqueMsgID)) {
        addToHistory(uniqueMsgID);
        int rssi = LoRa.packetRssi(); 
        
        // 1. 와이파이 상태에 상관없이 무조건 시리얼 모니터에 출력부터 함
        Serial.println("\n=== NEW DATA RECEIVED ===");
        Serial.println("Sender : " + sourceID);
        Serial.println("Msg ID : " + msgID);
        Serial.println("Hops   : " + hops + " 번 중계됨");
        Serial.println("Status : " + data);
        Serial.println("RSSI   : " + String(rssi));
        
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("WiFi   : Connected ✅");
        } else {
          Serial.println("WiFi   : Not Connected ❌ (핫스팟 이름/비번 확인 필요)");
        }
        Serial.println("=========================");
        
        // 2. 디스코드로 쏘기 (함수 내부에서 와이파이 상태를 다시 검사함)
        sendToDiscord(sourceID, hops, data, rssi);
      }
    }
  }
}
