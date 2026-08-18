<div align="center">

<h1><b>esp-matter-deadbolt</b></h1>
<p><b>ESP32-S3 Matter smart door lock firmware for unattended study cafes.</b></p>

<p>
  <a href="#features"><strong>Features</strong></a> ·
  <a href="#hardware"><strong>Hardware</strong></a> ·
  <a href="#getting-started"><strong>Getting Started</strong></a> ·
  <a href="#usage"><strong>Usage</strong></a> ·
  <a href="#api"><strong>API</strong></a>
</p>

<p>

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2.1-red?logo=espressif)](https://docs.espressif.com/projects/esp-idf)
[![Matter](https://img.shields.io/badge/Matter-1.x-blue)](https://csa-iot.org/developer-resource/matter-overview/)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-lightgrey?logo=espressif)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Language](https://img.shields.io/badge/language-C%2B%2B-00599C?logo=cplusplus)](https://isocpp.org)

</p>

</div>

---

## Overview

WiFi 기반 Matter 프로토콜로 원격 제어하는 ESP32-S3 도어락 펌웨어입니다. 스터디카페 등 무인 출입 관리 환경을 위해 설계되었으며, CW-020 단채널 릴레이와 WS2812 상태 LED를 구동합니다.

```
모바일 앱 / PC
     │
     │  Matter (WiFi)
     ▼
ESP32-S3 Door Lock
├── Door Lock Cluster (0x0101)     — 잠금 / 해제
└── Custom Control  (0x131BFC00)   — 퇴실(EXIT_OPEN), 팩토리 리셋
```

## Features

- **Lock / Unlock** — Matter Door Lock Cluster 표준 명령으로 릴레이 즉시 제어
- **EXIT_OPEN** — 퇴실 시 일시 해제 후 3~30초 내 자동 잠금 (스터디카페 퇴실 흐름)
- **Multi-device commissioning** — 최대 5개 컨트롤러 동시 연결, 부팅 시 180초 추가 등록 윈도우 자동 오픈
- **Factory Reset** — BOOT 버튼 5초 또는 Matter 커스텀 클러스터로 원격 초기화
- **OTA** — Matter OTA 클러스터 + 듀얼 슬롯 파티션으로 무선 펌웨어 업데이트
- **Status LED** — WS2812 RGB로 8가지 상태 시각화 (파랑/주황/초록/빨강)
- **BLE Commissioning** — 최초 설정 시 BLE로 WiFi 자격증명 프로비저닝

## Hardware

| 구성 | 사양 |
|------|------|
| MCU | ESP32-S3-DevKitC-1 |
| 릴레이 | CW-020 1채널 (Low Level Trigger) |
| LED | WS2812B RGB |
| 플래시 | 4MB (OTA 듀얼 슬롯) |
| 통신 | 2.4GHz WiFi + BLE 5.0 |

**GPIO 매핑**

| GPIO | 기능 | 비고 |
|------|------|------|
| 0 | BOOT 버튼 | 5초 → 팩토리 리셋 |
| 4 | CW-020 릴레이 IN | LOW=잠금, HIGH=해제 |
| 48 | WS2812 LED | 상태 표시 |

**Status LED**

| 상태 | 색상 | 패턴 |
|------|------|------|
| 부팅 중 | 파랑 | 느린 깜빡임 |
| 커미셔닝 대기 | 파랑 | 빠른 깜빡임 |
| 잠금 | 주황 | 고정 |
| 해제 | 초록 | 고정 |
| 동작 진행 | 주황/초록 | 빠른 깜빡임 (2초) |
| 동작 실패 | 빨강 | 빠른 깜빡임 (2초) |

## Getting Started

### Prerequisites

- [ESP-IDF v5.2](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/get-started/index.html)
- [esp-matter](https://github.com/espressif/esp-matter)

```bash
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh
```

### Build & Flash

```bash
git clone https://github.com/muinlab/esp-matter-deadbolt.git
cd esp-matter-deadbolt

idf.py set-target esp32s3
idf.py build

# 자동 포트 감지 + 플래시 + 모니터 (macOS)
./run_esp32-s3
```

### Commissioning (최초 등록)

```bash
# BLE로 WiFi 등록
chip-tool pairing ble-wifi <node_id> "<SSID>" "<password>" 20202021 3840

# 이미 WiFi 연결 시
chip-tool pairing onnetwork <node_id> 20202021
```

> [!NOTE]
> Setup PIN: `20202021` / Discriminator: `3840`

## Usage

```bash
# 잠금
chip-tool doorlock lock-door <node_id> 1 --timedInteractionTimeoutMs 1000

# 해제
chip-tool doorlock unlock-door <node_id> 1 --timedInteractionTimeoutMs 1000

# 상태 조회 (1=잠금, 2=해제)
chip-tool doorlock read lock-state <node_id> 1

# 퇴실 (5초 후 자동 잠금)
chip-tool any write-by-id 0x131BFC00 1 5 <node_id> 1

# OTA (GitHub Releases latest 자동 다운로드)
chip-tool any write-by-id 0x131BFC00 2 1 <node_id> 1

# 팩토리 리셋
chip-tool any write-by-id 0x131BFC00 0 57005 <node_id> 1
```

## API

### Door Lock Cluster `0x0101`

| Command | Endpoint | 동작 |
|---------|----------|------|
| `LockDoor` | 1 | GPIO 4 → LOW (잠금) |
| `UnlockDoor` | 1 | GPIO 4 → HIGH (해제) |
| `read lock-state` | 1 | 0=오류 / 1=잠금 / 2=해제 |

### Custom Control Cluster `0x131BFC00`

| Attribute | ID | Type | 동작 |
|-----------|----|------|------|
| `factory_reset` | `0x00000000` | uint16 | `57005`(0xDEAD) 쓰기 → 팩토리 리셋 |
| `exit_open` | `0x00000001` | uint8 | duration(3~30) 쓰기 → 퇴실 열림 후 자동 잠금 |
| `ota_trigger` | `0x00000002` | uint8 | `1` 쓰기 → GitHub Releases latest OTA |
| `health` | `0x00000003` | uint32 | `1` 쓰기 → 기존 헬스체크 비트맵 갱신 |

장애 원인 수집용 읽기 전용 HTTP 스냅샷은
`GET http://<deadbolt-ip>/diag`에서 확인할 수 있습니다 (`/diagnostics`도 지원). 기존 Matter
속성 계약은 변경하지 않으며, 필드와 판정 기준은
[`docs/RCA_DIAGNOSTICS.md`](docs/RCA_DIAGNOSTICS.md)를 참고하세요.
카나리 설치 여부는 `/diag`의 `diagnosticsRevision`과 `firmwareBuildId`로 확인합니다.

전체 API 명세: [`docs/API_SPECIFICATION.md`](docs/API_SPECIFICATION.md)

## Architecture

```
main/
├── app_main.cpp        # Matter 노드 초기화, 이벤트 콜백, BOOT 버튼 태스크
├── diagnostics.cpp     # RCA 카운터, 최근 명령 단계, 리셋/WiFi 상태
├── door_controller.cpp # 명령 큐, 릴레이 상태, 자동 잠금 타이머
├── hal_gpio.cpp        # GPIO 릴레이 추상화
├── comm_layer.cpp      # Matter 연결 상태 관리
└── status_led.cpp      # WS2812 RGB 상태 렌더링
```

## Multi-Device Support

기기 부팅 후 WiFi 연결 시 **180초간 커미셔닝 윈도우 자동 오픈**합니다.

- BOOT 버튼을 누르면 재부팅 → 새 기기가 180초 내 커미셔닝 가능
- 최대 **5개 Fabric** 동시 지원
- 기존 연결 기기는 커미셔닝 중에도 정상 제어 가능

```bash
# 이미 연결된 기기에서 추가 커미셔닝 윈도우 열기
chip-tool pairing open-commissioning-window <node_id> 1 180 1000 0
```
