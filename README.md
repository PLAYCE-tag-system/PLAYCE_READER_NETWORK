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

## 백엔드로 보내기 (어느 PC·라즈베리파이든)

수신기를 꽂은 컴퓨터에서 `pc/uplink.py`를 실행하면 시리얼 JSON 줄을 PLAYCE 백엔드로 보냅니다. 그러면 `https://playce-admin.mooo.com/live`(실시간 중계)와 `/qr`(등록 QR)에 그 데이터가 뜹니다. 백엔드의 업링크 토큰을 가진 사람만 보낼 수 있습니다.

1. `.env.example`을 `.env`로 복사하고 `KHUROUND_RFID_UPLINK_TOKEN`에 백엔드와 같은 값을 넣으세요. 이 토큰은 수신기 줄 업로드에만 통하므로 노트북·라즈베리파이를 잃어버려도 다른 내부 API는 열리지 않습니다. `.env`는 Git에서 제외되며, Linux/macOS에서는 `chmod 600 .env`로 두세요.
2. `python -m pip install -r pc/requirements.txt`
3. 프로젝트 루트에서 `python pc/uplink.py --port auto`를 실행하세요. 라즈베리파이는 `python3 pc/uplink.py --port /dev/ttyUSB0`처럼 포트를 직접 줄 수도 있습니다(`dialout` 그룹 권한 필요).
4. 등록 QR까지 쓰려면 같은 컴퓨터에서 [PLAYCE_REGISTRATION](https://github.com/PLAYCE-tag-system/PLAYCE_REGISTRATION)의 `registration_reader.py`도 실행하세요. uplink가 수신기 직접 읽기 이벤트를 `collector.py`와 똑같이 UDP 8767로 넘겨주고, `REGISTRATION_API_BASE`는 `https://playce-admin.mooo.com`으로 두면 됩니다.

수신기를 Wi-Fi UDP로 받으려면 `--wifi-port 8766 --wifi-allow <수신기 IP>`를 더하세요. 기본은 꺼져 있고, `--wifi-allow` 없이 켜면 같은 LAN의 아무 기기나 가짜 줄을 보낼 수 있습니다. 백엔드 주소는 `https://`만 허용합니다(`http://`는 localhost만), 리다이렉트는 따라가지 않습니다.

`uplink.py`와 `collector.py`는 같은 시리얼 포트를 쓰므로 한 컴퓨터에서는 둘 중 하나만 실행하세요. 3초 넘게 늦어진 줄은 보내지 않고 버리고, 바로 실패한 전송은 한 번만 다시 보냅니다(백엔드가 도착 시각으로 체류 시간을 재기 때문입니다). 리더가 태그를 계속 다시 보내므로 연결이 돌아오면 화면이 따라오고, 놓친 이탈은 30초 뒤 자동으로 닫힙니다. 백엔드는 상태를 메모리에 두므로 백엔드를 재시작하면 실시간 상태와 이력이 비워집니다.

## 테스트

```powershell
python -m unittest discover -s pc -p "test_*.py"
node --test pc/test_dashboard.cjs
```

실제 비밀번호, 태그 이력, 로그, 데이터베이스, 빌드 결과물은 포함하지 않았습니다. QR 등록 화면은 별도 [PLAYCE_REGISTRATION 저장소](https://github.com/PLAYCE-tag-system/PLAYCE_REGISTRATION)에 있습니다.
