#include <SPI.h>
#include <LoRa.h>

#define SCK     5
#define MISO    19
#define MOSI    27
#define SS      18
#define RST     14
#define DIO0    26
#define BAND    915E6

const int MAX_HISTORY = 30;
String msgHistory[MAX_HISTORY];
int historyIndex = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(BAND)) {
    Serial.println("LoRa init failed!");
    while (1);
  }
  Serial.println("Mesh Gateway Ready. Waiting for data...");
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
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    Serial.println("[DEBUG] Raw Received: " + incoming);
    
    // 데이터 형식: "SourceID,MsgID,Hops,Data"
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
        
        // 라즈베리파이(시리얼 모니터)로 보기 좋게 출력
        Serial.println("=== NEW DATA RECEIVED ===");
        Serial.println("Sender : " + sourceID);
        Serial.println("Msg ID : " + msgID);
        Serial.println("Hops   : " + hops + " 번 중계됨"); // 누적 중계 횟수 출력
        Serial.println("Status : " + data);
        Serial.println("RSSI   : " + String(rssi));
        Serial.println("=========================\n");
      }
    }
  }
}
