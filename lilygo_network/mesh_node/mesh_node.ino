#include <SPI.h>
#include <LoRa.h>

#define SCK     5
#define MISO    19
#define MOSI    27
#define SS      18
#define RST     14
#define DIO0    26
#define BAND    915E6

// 이 기기의 고유 이름 (여러 대 만들 경우 NODE_B, NODE_C 등으로 변경)
String myNodeID = "NODE_B";
int msgCounter = 0;

// 중복 수신 방지용 히스토리
const int MAX_HISTORY = 10;
String msgHistory[MAX_HISTORY];
int historyIndex = 0;

unsigned long lastSendTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(BAND)) {
    Serial.println("LoRa init failed!");
    while (1);
  }
  Serial.println("Mesh Node Ready: " + myNodeID);
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

void loop() {
  // ---------------------------------------------------------
  // 1. 수신 및 중계 (Relay) 처리
  // ---------------------------------------------------------
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    
    // 데이터 형식: "SourceID,MsgID,Hops,Data"
    int firstComma = incoming.indexOf(',');
    int secondComma = incoming.indexOf(',', firstComma + 1);
    int thirdComma = incoming.indexOf(',', secondComma + 1);
    
    if (firstComma > 0 && secondComma > 0 && thirdComma > 0) {
      String sourceID = incoming.substring(0, firstComma);
      String msgID = incoming.substring(firstComma + 1, secondComma);
      int hops = incoming.substring(secondComma + 1, thirdComma).toInt();
      String data = incoming.substring(thirdComma + 1);
      
      String uniqueMsgID = sourceID + "_" + msgID;
      
      // 내가 보낸 메시지가 아니면 중계 처리
      if (sourceID != myNodeID) {
        if (!isDuplicate(uniqueMsgID)) {
          addToHistory(uniqueMsgID);
          
          // 핵심: 중계할 때마다 Hops(건너뛴 횟수)를 1 증가시킴
          hops++; 
          String relayedPayload = sourceID + "," + msgID + "," + String(hops) + "," + data;
          
          Serial.println("[Relay] Forwarding msg from " + sourceID + " (Hops: " + String(hops) + ")");
          
          delay(random(50, 150)); // 충돌 방지 딜레이
          
          LoRa.beginPacket();
          LoRa.print(relayedPayload); // 카운트가 1 증가된 새로운 패킷으로 전송
          LoRa.endPacket();
        }
      }
    }
  }

  // ---------------------------------------------------------
  // 2. 내 연결 상태 생성 및 송신 (1초마다)
  // ---------------------------------------------------------
  if (millis() - lastSendTime > 1000) {
    msgCounter++;
    String msgID = String(msgCounter);
    String uniqueMsgID = myNodeID + "_" + msgID;
    
    // 초기 전송이므로 Hops는 0으로 시작
    String payload = myNodeID + "," + msgID + ",0,Status:OK";
    
    Serial.println("[Send] My data: " + payload);
    
    addToHistory(uniqueMsgID); // 내 메시지도 히스토리 기록
    
    LoRa.beginPacket();
    LoRa.print(payload);
    LoRa.endPacket();
    
    lastSendTime = millis();
  }
}
