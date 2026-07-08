
#include <SPI.h>
#include <LoRa.h>

// LilyGO LoRa32 (V1.6 / V2.1 등) 기본 핀 매핑
#define SCK     5
#define MISO    19
#define MOSI    27
#define SS      18
#define RST     14
#define DIO0    26

// 통신 주파수 (송신기와 반드시 똑같은 주파수를 사용해야 합니다)
#define BAND    915E6

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("LoRa Receiver Test");

  // SPI 핀 설정
  SPI.begin(SCK, MISO, MOSI, SS);
  // LoRa 모듈 핀 설정
  LoRa.setPins(SS, RST, DIO0);

  // 송신기와 동일한 주파수로 LoRa 통신 시작
  if (!LoRa.begin(BAND)) {
    Serial.println("LoRa 모듈 초기화 실패! 핀 연결이나 모듈 상태를 확인하세요.");
    while (1);
  }
  Serial.println("LoRa 초기화 성공! 신호 수신 대기 중...");
}

void loop() {
  // 수신된 패킷이 있는지 확인
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    // 패킷을 성공적으로 수신했을 때
    Serial.print("Received packet '");

    // 패킷 내용을 문자열로 읽어서 출력
    while (LoRa.available()) {
      Serial.print((char)LoRa.read());
    }

    // 신호 세기(RSSI) 출력
    Serial.print("' with RSSI ");
    Serial.println(LoRa.packetRssi());
  }
}