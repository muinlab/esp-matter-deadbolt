# ESP32-S3 Matter 도어락 — API 명세서 (Mobile App)

**버전:** 4.1.3
**최종 수정:** 2026-04-07
**대상:** 모바일 앱 (iOS/Android) 및 PC 관리 소프트웨어 개발자

---

## 개요

도어락은 **Matter 프로토콜(WiFi)**을 통해 제어됩니다.

| 클러스터 | ID | 용도 |
|----------|-----|------|
| **Door Lock** (표준) | `0x0101` | 잠금/해제 |
| **Custom Control** (커스텀) | `0x131BFC00` | 퇴실(EXIT_OPEN), 팩토리 리셋 |

### 통신 구조

```
모바일 앱 / PC 관리 SW
       │
       │  Matter (WiFi)
       ▼
  ESP32-S3 Door Lock
  ├── Door Lock Cluster (0x0101)    — 잠금/해제
  └── Custom Control (0x131BFC00)   — 퇴실, 팩토리 리셋
```

### BLE에 대하여

BLE는 **Matter 커미셔닝(초기 기기 등록) 전용**입니다. CHIP SDK가 BLE 스택을 완전히 독점하므로 커스텀 BLE 서비스는 사용할 수 없습니다. WiFi 연결 실패 시 기기가 자동으로 커미셔닝 윈도우를 열어 BLE를 통해 새 WiFi 크레덴셜을 수신합니다.

---

## 1. Door Lock Cluster (0x0101) — 잠금/해제

표준 Matter Door Lock Cluster.

### 1-1. LockDoor (잠금)

**Request**

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| endpointId | uint16 | Y | 도어락 엔드포인트 (기본: 1) |
| timedInteractionTimeoutMs | uint16 | Y | 권장: 1000ms |

**Response**

| 필드 | 타입 | 값 |
|------|------|-----|
| status | StatusCode | `0x00` = 성공 |

**동작:** GPIO 4 → LOW → 릴레이 OFF → 잠금

```bash
chip-tool doorlock lock-door <node_id> <endpoint_id> --timedInteractionTimeoutMs 1000
```

---

### 1-2. UnlockDoor (해제)

**Request**

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| endpointId | uint16 | Y | 도어락 엔드포인트 (기본: 1) |
| timedInteractionTimeoutMs | uint16 | Y | 권장: 1000ms |

**Response**

| 필드 | 타입 | 값 |
|------|------|-----|
| status | StatusCode | `0x00` = 성공 |

**동작:** GPIO 4 → HIGH → 릴레이 ON → 해제 (지속)

```bash
chip-tool doorlock unlock-door <node_id> <endpoint_id> --timedInteractionTimeoutMs 1000
```

---

### 1-3. Read: LockState (상태 조회)

**Response**

| value | 타입 | 의미 |
|-------|------|------|
| `0` | enum8 | NotFullyLocked (동작 실패) |
| `1` | enum8 | **Locked** (잠금) |
| `2` | enum8 | **Unlocked** (해제) |

```bash
chip-tool doorlock read lock-state <node_id> <endpoint_id>
```

---

### 1-4. Subscribe: LockState (실시간 상태 감지)

```bash
chip-tool doorlock subscribe lock-state <min-interval> <max-interval> <node_id> <endpoint_id>
```

앱에서 Subscribe를 설정하면 잠금 상태 변경 시 자동으로 알림을 받습니다.

---

### 1-5. Read: LockType

| value | 의미 |
|-------|------|
| `0` | DeadBolt (고정값) |

---

### 1-6. 미지원 기능

| 기능 | 응답 |
|------|------|
| User 관리 (Get/Set) | `false` |
| Credential 관리 (Get/Set) | `false` |
| Schedule 관리 | `DlStatus::kFailure` |
| PIN 코드 인증 | 무시 (항상 성공) |
| UnlockWithTimeout | 미등록 (커스텀 클러스터 exit_open 사용) |

---

## 2. Custom Control Cluster (0x131BFC00) — 퇴실/팩토리 리셋/헬스체크

커스텀 클러스터. Attribute Write로 동작을 트리거합니다.

| Attribute | ID | 타입 | 용도 |
|-----------|----|------|------|
| factory_reset | `0x00000000` | uint16 | `0xDEAD`(57005) 쓰기 → 팩토리 리셋 |
| exit_open | `0x00000001` | uint8 | duration(3~30) 쓰기 → 퇴실 열림 |
| ota_trigger | `0x00000002` | uint8 | `1` 쓰기 → HTTPS OTA 시작 |
| health | `0x00000003` | uint32 | `1` 쓰기 → 헬스체크, 읽기 → 결과 반환 |

---

### 2-1. EXIT_OPEN (퇴실)

도어를 일시적으로 해제한 후 자동 잠금합니다.

**Request — Attribute Write**

| 필드 | 값 |
|------|-----|
| cluster_id | `0x131BFC00` |
| attribute_id | `0x00000001` |
| value | duration (uint8, 3~30초. 범위 밖이면 5초로 보정) |

**동작:**
1. GPIO HIGH → 해제
2. duration초 대기
3. GPIO LOW → 자동 잠금

```bash
# 5초 퇴실
chip-tool any write-by-id 0x131BFC00 1 5 <node_id> <endpoint_id>

# 10초 퇴실
chip-tool any write-by-id 0x131BFC00 1 10 <node_id> <endpoint_id>
```

---

### 2-2. OTA_TRIGGER (무선 펌웨어 업데이트)

최신 릴리즈 펌웨어를 GitHub Releases에서 직접 다운로드하여 업데이트합니다.

**Request — Attribute Write**

| 필드 | 값 |
|------|-----|
| cluster_id | `0x131BFC00` |
| attribute_id | `0x00000002` |
| value | `1` (uint8) |

**동작:**
1. HTTPS로 GitHub Releases latest 바이너리 다운로드
2. OTA 파티션에 기록
3. 완료 시 자동 재부팅

> [!NOTE]
> URL은 펌웨어에 하드코딩됨 — 버전 업 시 별도 설정 불필요.
> OTA 진행 중 **보라색 느린 점멸** LED로 상태 표시. 완료 후 자동 재부팅.

```bash
chip-tool any write-by-id 0x131BFC00 2 1 <node_id> <endpoint_id>
```

---

### 2-3. FACTORY_RESET (팩토리 리셋)

NVS 전체 삭제 + 재부팅. 재커미셔닝이 필요합니다.

**Request — Attribute Write**

| 필드 | 값 |
|------|-----|
| cluster_id | `0x131BFC00` |
| attribute_id | `0x00000000` |
| value | `57005` (0xDEAD, uint16) |

**동작:**
1. `ScheduleFactoryReset()` 호출
2. NVS 전체 삭제 (WiFi, Matter fabric, 모든 설정)
3. 자동 재부팅
4. 커미셔닝 대기 상태 (파란 LED 깜빡임)

```bash
chip-tool any write-by-id 0x131BFC00 0 57005 <node_id> <endpoint_id>
```

> 0xDEAD 이외의 값은 무시됩니다 (안전장치).

---

### 2-4. HEALTH (헬스체크)

기기 상태를 한 번에 조회합니다. `1`을 쓰면 측정 후 결과를 같은 속성에 저장합니다.

**Request — Attribute Write (트리거)**

| 필드 | 값 |
|------|-----|
| cluster_id | `0x131BFC00` |
| attribute_id | `0x00000003` |
| value | `1` (uint32) |

**Response — Attribute Read (결과)**

```bash
chip-tool any write-by-id 0x131BFC00 3 1 <node_id> <endpoint_id>
chip-tool any read-by-id  0x131BFC00 3   <node_id> <endpoint_id>
```

반환값은 uint32 비트맵:

| 비트 | 필드 | 설명 |
|------|------|------|
| `[1:0]` | result | `1`=정상, `2`=힙 부족(<20KB), `3`=WiFi 끊김 |
| `[2]` | door | `1`=잠김, `0`=해제 |
| `[3]` | wifi | `1`=연결됨, `0`=끊김 |
| `[15:8]` | heap_kb | 잔여 힙 (KB) |
| `[23:16]` | rssi+128 | WiFi RSSI + 128. 미연결=`0` (예: -62dBm → `66`) |

**디코딩 예시 (Python)**

```python
val = read_result  # chip-tool로 읽은 uint32
result  = val & 0x3
locked  = (val >> 2) & 0x1
wifi    = (val >> 3) & 0x1
heap_kb = (val >> 8) & 0xFF
rssi    = ((val >> 16) & 0xFF) - 128  # dBm, 미연결=-128
```

> [!NOTE]
> 쓰기 직후 읽기가 이루어지면 측정이 완료되기 전일 수 있습니다. 100ms 이상 후 읽기를 권장합니다.

#### 장애 분석 스냅샷 (로컬 HTTP)

기존 Matter `health` 비트맵을 변경하지 않고 더 상세한 장애 경계를 조회할 수
있습니다.

```bash
curl --fail --max-time 2 http://<deadbolt-ip>/diag
```

응답에는 `diagnosticsRevision`, 실행 ELF의 `firmwareBuildId` SHA-256, 리셋 사유,
uptime, 현재/최소 힙, WiFi 단절·재연결 횟수,
Matter 명령 콜백 횟수와 최근 8개 명령의 수신·큐·실행·릴레이 명령·완료
시각이 포함됩니다. 모든 시각은 부팅 후 경과 밀리초이며 재부팅 시 기록이
초기화됩니다. 상세 스키마와 판정표는
[`RCA_DIAGNOSTICS.md`](RCA_DIAGNOSTICS.md)를 참고하세요.

---

## 3. 커미셔닝 (기기 등록)

### 3-1. BLE-WiFi 커미셔닝 (최초 등록)

| 파라미터 | 기본값 | 설명 |
|----------|--------|------|
| Setup PIN | `20202021` | 페어링 PIN |
| Discriminator | `3840` | 기기 식별자 |

```bash
chip-tool pairing ble-wifi <node_id> <ssid> <password> 20202021 3840
```

### 3-2. 온네트워크 커미셔닝 (WiFi 이미 연결됨)

```bash
chip-tool pairing onnetwork <node_id> 20202021
```

### 3-3. 커미셔닝 해제

```bash
chip-tool pairing unpair <node_id>
```

### 3-4. 수동 팩토리 리셋

BOOT 버튼(GPIO 0)을 **5초 길게 누르면** 팩토리 리셋됩니다.

---

## 4. WiFi 실패 시 복구

WiFi 연결이 장시간 실패하면 기기가 자동으로 커미셔닝 윈도우를 다시 엽니다.

```
WiFi 실패 지속 → 커미셔닝 윈도우 오픈 → BLE 광고 재개
→ 모바일 앱에서 ble-wifi pairing으로 새 WiFi 크레덴셜 전달
→ WiFi 재연결 → 정상 운영
```

또는 원격으로 `factory_reset`(섹션 2-2)을 보내 완전 초기화할 수 있습니다.

---

## 5. 모바일 앱 통합 가이드

### 5-1. 연결 전략

```
1. Matter 컨트롤러로 기기 검색 (mDNS)
   ├─ 발견 → Matter API로 제어
   └─ 미발견 → BLE 스캔
2. BLE 커미셔닝 광고 발견 시
   → BLE-WiFi 페어링 진행
```

### 5-2. 일반 사용 시나리오

```
앱 시작 → Read LockState → 현재 상태 표시
사용자 "잠금" 터치 → LockDoor 전송 → 결과 표시
사용자 "해제" 터치 → UnlockDoor 전송 → 결과 표시
사용자 "퇴실" 터치 → exit_open Write (5초) → "5초 후 자동 잠금" 표시
```

### 5-3. 관리 시나리오

```
관리자 "공장 초기화" → factory_reset Write (0xDEAD) → "재부팅 중" 표시
관리자 "WiFi 변경" → factory_reset 또는 재커미셔닝
```

### 5-4. 에러 처리

| 상황 | 처리 |
|------|------|
| Matter 명령 타임아웃 | WiFi 확인 → 재시도 |
| LockState = 0 | 동작 실패 → 수동 확인 요청 |
| 기기 mDNS 미발견 | 전원 확인 → BLE 스캔으로 재커미셔닝 |
| write-by-id 실패 | 커미셔닝 상태 확인 |

---

## 6. 하드웨어 매핑

| GPIO | 기능 | 동작 |
|------|------|------|
| 0 | BOOT 버튼 | 5초 → 팩토리 리셋 |
| 4 | CW-020 릴레이 IN | LOW=잠금, HIGH=해제 |
| 48 | WS2812 Status LED | 상태 표시 |

### Status LED

| 상태 | 색상 | 패턴 |
|------|------|------|
| 부팅 중 | 파랑 | 느린 깜빡임 |
| 커미셔닝 대기 | 파랑 | 빠른 깜빡임 |
| 잠금 | 주황 | 고정 |
| 해제 | 초록 | 고정 |
| 동작 진행 | 주황/초록 | 빠른 깜빡임 (2초) |
| 동작 실패 | 빨강 | 빠른 깜빡임 (2초) |
| OTA 다운로드 중 | 보라 | 느린 깜빡임 (재부팅까지 유지) |

---

## 7. 자동 OTA (Auto Update)

기기가 자동으로 새 펌웨어를 감지하고 업데이트합니다. 앱/서버 개입 불필요.

### 동작 흐름

```
부팅 → WiFi 연결 → 60초 대기
→ GitHub API 조회 (1시간 주기)
→ 현재 버전 ≠ 최신 버전?
   ├─ YES → 도어 잠김 대기 → 자동 OTA 시작
   └─ NO  → 다음 주기까지 대기
```

### 버전 확인 방식

- GitHub API: `GET https://api.github.com/repos/muinlab/esp-matter-deadbolt/releases/latest`
- 응답의 `tag_name` (예: `v1.1.0`)을 현재 펌웨어 버전과 비교
- 다르면 업데이트 대기 플래그 세팅

### 안전 조건

- 도어가 **잠긴 상태**일 때만 OTA 시작 (5초 간격 폴링)
- OTA 중 LED 보라색 점멸로 진행 상태 표시
- 완료 후 자동 재부팅

### 펌웨어 배포 절차 (관리자)

1. 코드 변경 + `FIRMWARE_VERSION` 값 올리기 (예: `"v1.2.0"`)
2. 빌드 → GitHub Release `v1.2.0` 생성 (latest 태그)
3. 기기가 최대 1시간 내 자동 감지 및 업데이트

---

## 8. 클러스터 요약

```
Endpoint 1
├── Door Lock Cluster (0x0101)
│   ├── Command: LockDoor          → GPIO LOW (잠금)
│   ├── Command: UnlockDoor        → GPIO HIGH (해제)
│   └── Attribute: LockState       → 0/1/2 (Read/Subscribe)
│
└── Custom Control (0x131BFC00)
    ├── Attribute 0: factory_reset  → uint16, write 57005(0xDEAD) = 리셋
    ├── Attribute 1: exit_open      → uint8, write 3~30 = 퇴실 (초)
    ├── Attribute 2: ota_trigger    → uint8, write 1 = OTA 시작
    └── Attribute 3: health         → uint32, write 1 = 헬스체크 트리거 / read = 결과 비트맵
```
