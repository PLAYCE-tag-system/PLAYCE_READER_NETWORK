# PLAYCE 태그 모니터링

UHF RFID 태그를 리더 1·2·3에서 읽고 LoRa로 중계하여 PC 화면에 실시간 위치와 체류 시간을 표시하는 소스입니다.

## 파일

- `src/`, `include/`, `platformio.ini`: 리더기와 수신기 ESP32 펌웨어
- `pc/collector.py`: USB/Wi-Fi 이벤트 수집, API, 웹 서버
- `pc/index.html`, `pc/dashboard.css`, `pc/dashboard.js`, `pc/fonts/`: 모니터링 화면
- `pc/test_collector.py`, `pc/test_dashboard.cjs`: 테스트

## 실행

PlatformIO에서 펌웨어를 빌드하기 전 `include/ota_config.example.h`를 `include/ota_config.h`로, `ota.local.example.ini`를 `ota.local.ini`로 복사하고 본인 환경의 Wi-Fi/OTA 정보를 입력하세요. 실제 설정 파일은 Git에서 제외됩니다.

PC에서 `python -m pip install -r pc/requirements.txt`로 의존성을 설치한 다음 프로젝트 루트에서 `python pc/collector.py --port auto`를 실행하세요. 모니터링 화면은 `http://127.0.0.1:8765/`입니다.

## 테스트

```powershell
python -m unittest discover -s pc -p "test_*.py"
node --test pc/test_dashboard.cjs
```

실제 비밀번호, 태그 이력, 로그, 데이터베이스, 빌드 결과물은 포함하지 않았습니다. QR 등록 화면은 별도 [PLAYCE_REGISTRATION 저장소](https://github.com/PLAYCE-tag-system/PLAYCE_REGISTRATION)에 있습니다.
