# Virtual Production Pipeline

<p align="center">
  <img src="./Virtual%20Production%20Pipeline.gif" alt="Virtual Production Pipeline demo">
</p>

단일 웹캠으로 얼굴, 머리, 양쪽 상완을 추적해 VRoid 아바타에 반영하고 OBS 송출·녹화를 제어하는 Unreal Engine 5.8 기반 실시간 버추얼 프로덕션 프로젝트입니다.

Python의 MediaPipe 추론 결과를 고정 크기 Binary UDP 패킷으로 전송하고, Unreal C++ 런타임 플러그인이 전용 네트워크 스레드에서 검증·수신합니다. 게임 스레드는 최신 프레임만 소비하며, 입력이 끊기면 표정과 본 회전이 중립 상태로 복귀합니다.

## 데모와 구조

```text
Webcam
  └─ OpenCV BGR→RGB
      └─ MediaPipe Face + Pose
          └─ VPTP schema 3 Binary UDP (localhost:7000)
              └─ VPTrackerReceiver (Unreal C++)
                  ├─ Morph Target / Head
                  └─ Left·Right Upper Arm

Supervisor ─┬─ Unreal managed session
            ├─ Python tracker + webcam
            └─ OBS WebSocket control (OBS process is externally owned)
Unreal SceneCapture ── shared RenderTarget ── Operator Preview + Spout ── OBS
MCP diagnostics ── read-only pipeline and protocol checks
```

## 구현 범위

- MediaPipe FaceLandmarker의 ARKit 52 Blendshape 추론
- FaceLandmarker 변환 행렬을 이용한 머리 Pitch/Yaw/Roll 계산과 목·머리 분산
- 양쪽 어깨·팔꿈치의 화면상 높이·깊이와 visibility/presence를 이용한 상완 계산
- 하완·손목·손가락은 추적하지 않고 아바타 기본 자세 유지
- DataTable 기반 ARKit 이름 → VRoid Morph Target 매핑
- 엄격한 `Virtual Production Tracking Packet`(`VPTP`) 스키마 검증과 크기 제한 수신 큐
- 0.5초 수신 중단 또는 지속적인 검출 신뢰도 저하 시 중립 복귀
- confidence 히스테리시스·0.25초 유예와 여러 정상 샘플을 사용하는 3초 중립 자세 캘리브레이션
- 아바타별 좌우 교환·반전·gain·각도·입력 필터·중립값 `USaveGame` 프로필
- 중립 → 왼팔 → 중립 → 오른팔 순서의 단계별 상완 검증 API와 진단값
- 좌측 제어판·상단 단계 안내·하단 추적 상태로 구성된 C++ UMG 대시보드
- 우측 상단 원시 Pose 스켈레톤과 confidence 색상 표시
- 아바타 전신을 최초 1회 자동 프레이밍하는 전용 SceneCapture
- 단일 최종 RenderTarget을 운영자 미리보기와 Spout 송신이 공유하는 방송 출력 분리
- `배경 제거`(Premultiplied Alpha)·`단색 배경` 모드와 미리보기/송출 배경색 RGB 조정
- 방송 팔레트에서 앱·SceneCapture·Spout FPS를 15~144로 조절하고 OBS가 유휴 상태이면 OBS FPS도 동기화
- OBS WebSocket 스트리밍·녹화 제어
- 단일 Supervisor의 사전 점검·중복 실행 방지·Unreal/트래커 heartbeat 감시·단계적 종료
- UE 5.8 공식 Experimental MCP/Editor Toolset과 프로젝트 전용 읽기 전용 MCP 진단 도구

## 핵심 문제 해결

### 1. 딕셔너리 순서에 의존하던 UDP 계약

기존 송신기는 MediaPipe 결과 딕셔너리의 값 순서를 그대로 전송했고, 수신기는 count가 예상보다 클 때 offset을 잘못 계산할 수 있었습니다.

현재는 `vp-tracker/protocol.py`를 단일 Python 계약으로 두고 52개 이름 순서를 명시했습니다. C++ 수신기는 버전, 플래그, count, 정확한 최종 크기, NaN/Inf와 신뢰도 범위를 모두 확인한 패킷만 전달합니다.

### 2. 추적 소실 시 마지막 자세가 고정되는 문제

정상 패킷을 받은 로컬 시각을 기준으로 0.5초 timeout을 적용합니다. timeout 후 프레임을 무효화하고 이전 프레임에서 적용한 Morph Target을 제거하며 머리·상완을 시간 기반 보간으로 중립 자세에 복귀시킵니다.

### 3. 손목이 보이지 않는 웹캠 구도

촬영 범위가 팔꿈치까지이므로 손목을 요구하는 팔꿈치 굽힘 추정은 제외했습니다. 좌우 어깨(11, 12)와 팔꿈치(13, 14)만으로 화면상 상완 각도를 계산합니다. 획득 0.5/해제 0.3 confidence 히스테리시스와 0.25초 유예를 적용하고, 그보다 오래 추적이 끊긴 팔만 중립으로 복귀시킵니다.

### 4. 사용자와 아바타마다 다른 중립 자세

`VPAnimInstance`는 3초 동안 중립 자세를 안내하고 고유한 정상 프레임들의 머리와 양쪽 상완 입력을 원형 평균해 기준값으로 저장합니다. 짧은 confidence 흔들림은 마지막 안정 각도를 유지하고, 실제 추적이 끊기면 최대 5초 동안 복구를 기다린 뒤 카운트다운을 다시 시작합니다.

상완 입력은 시간 기반으로 평활화하며 큰 방향 변화는 처음 두 프레임 동안 보류하고 같은 변화가 지속되면 새 입력으로 인정합니다. 좌우 교환, 반전, 회전축, 좌우 gain, 각도 범위, 필터와 캘리브레이션 값은 명시적 프로필 ID 또는 Skeletal Mesh 경로별로 로컬 SaveGame에 저장됩니다.

## Virtual Production Tracking Packet 계약

현재는 VPTP 스키마 3 하나만 송수신합니다. 모든 수치는 little-endian이며 패킷 크기는 928 bytes로 고정됩니다.

| Offset | 형식 | 내용 |
|---:|---|---|
| 0 | `char[4]` | 고정 식별자 `VPTP` |
| 4 | `uint8` | Schema version `3` |
| 5 | `uint8` | bit 0 face tracked, bit 1 pose tracked |
| 6 | `uint16` | Reserved, 반드시 `0` |
| 8 | `uint32` | Frame ID |
| 12 | `double` | Capture timestamp |
| 20 | `uint16` | Blendshape count, 반드시 `52` |
| 22 | `float[52]` | 고정 이름 순서의 Blendshape 값 |
| 230 | `float[9]` | FaceLandmarker 3×3 회전 행렬 |
| 266 | `uint16` | Pose count, 반드시 `33` |
| 268 | `float[33][5]` | x, y, z, visibility, presence |

## 프로젝트 구성

```text
Virtual-Production-Pipeline/
├─ VPPipeline/
│  ├─ Plugins/VPTrackerReceiver/    # C++ UDP 수신·파서·AnimInstance·자동 테스트
│  ├─ Source/VPPipeline/            # C++ UMG 대시보드·HUD·GameMode
│  ├─ Content/Actor/Avatar/         # VRoid 자산과 Anim Blueprint
│  ├─ Content/Data/                 # Blendshape 매핑 DataTable
│  └─ VPPipeline.uproject
├─ tools/vrm4u/                    # VRM4U UE 5.8 설치·런타임 호환성 패치
├─ tools/mediapipe/                # 공식 모델 다운로드·SHA-256 검증
└─ vp-tracker/
   ├─ protocol.py                   # VPTP 스키마 3 고정 계약
   ├─ tracker.py                    # OpenCV·MediaPipe 추론
   ├─ sender.py                     # Binary UDP 송신
   ├─ launcher.py                   # Supervisor 호환 실행 진입점
   ├─ supervisor.py                 # Unreal·트래커 실행/감시/종료 소유자
   ├─ lifecycle.py                  # 세션 heartbeat·Windows Job·단일 인스턴스
   ├─ obs_controller.py             # OBS WebSocket 제어
   ├─ mcp_server.py                 # 읽기 전용 AI 진단 도구
   └─ test_protocol.py              # 의존성 없는 계약 단위 테스트
```

## 실행 환경과 준비

- Unreal Engine 5.8.2
- Visual Studio 2022 C++ toolchain
- Python 3.12 이상과 `uv`
- 웹캠
- 선택: OBS Studio WebSocket 5.x
- MediaPipe 모델
  - `vp-tracker/models/face_landmarker.task`
  - `vp-tracker/models/pose_landmarker_full.task` (기본 실시간 모드)
  - 선택: `vp-tracker/models/pose_landmarker_heavy.task` (고품질 모드)
- VRM4U for UE 5.8
  - 검증 버전: `v1.2026.07.22`
  - 설치 위치: `VPPipeline/Plugins/VRM4U`
  - 플러그인과 모델 파일은 저장소 용량 때문에 Git에서 제외됩니다.

저장소 루트에서 다음 스크립트를 실행하면 [공식 UE 5.8 릴리스](https://github.com/ruyo/VRM4U/releases/tag/v1.2026.07.22)를 내려받아 SHA-256을 확인하고, 런타임 VRM 로드에 필요한 호환성 패치를 적용합니다. 기존 설치가 지원 버전 또는 패치 상태와 다르면 덮어쓰지 않고 중단합니다.

```powershell
.\tools\vrm4u\Install-VRM4U.ps1
.\tools\mediapipe\Install-MediaPipeModels.ps1
```

스크립트를 다시 실행하면 설치와 패치 상태만 확인하고 파일을 중복 변경하지 않습니다. VRM4U 본체는 계속 Git에서 제외되며, 설치 스크립트와 최소 소스 패치만 저장소에서 관리합니다.

MediaPipe 설치 스크립트는 기본 Face+Pose Full 모델을 준비합니다. 선택적 Heavy 모델은 `.\tools\mediapipe\Install-MediaPipeModels.ps1 -IncludeHeavy`로 설치하고, 실행 전 `$env:VP_POSE_MODEL='heavy'`를 지정합니다.

```powershell
cd vp-tracker
uv sync
Copy-Item .env.example .env
```

`vp-tracker/.env`에 OBS WebSocket 비밀번호를 입력합니다. OBS를 사용하지 않으면 비워 두어도 트래킹은 실행할 수 있습니다.

```env
VP_OBS_PASSWORD=your_obs_websocket_password
```

## 실행

아래 명령 하나가 사전 점검 후 `/Game/Maps/Lvl_Empty`를 독립 게임 창으로 열고 트래커를 시작합니다. 언리얼 창을 닫거나 언리얼이 비정상 종료되면 heartbeat가 끊긴 트래커도 종료되어 웹캠을 해제합니다. Supervisor가 예기치 않게 종료되면 Windows Job Object가 두 자식 프로세스를 정리합니다. OBS 자체는 방송 안전을 위해 종료하지 않습니다.

```powershell
cd vp-tracker
uv run launcher.py
```

실행 창의 명령은 `stream`, `stream confirm`, `stop`, `rec`, `stoprec`, `status`, `quit`입니다. 실수로 방송을 시작하지 않도록 `stream`은 시작 대기만 설정하며 10초 안에 `stream confirm`을 한 번 더 입력해야 실제 송출을 시작합니다. `status`는 방송과 로컬 녹화의 활성 여부·경과 시간·기록량을 각각 표시합니다. OBS 연결 실패는 경고이며 트래킹 실행을 막지 않습니다. 방송 팔레트의 송출 FPS는 15~144 범위로 저장되며 OBS가 출력 중이 아니면 OBS 영상 FPS도 함께 바뀝니다. 방송·녹화·가상 카메라·리플레이 버퍼가 작동 중이면 OBS 출력은 중단하지 않고 기존 송출 FPS를 유지하며 앱에 경고를 표시합니다. 게임 안에서는 `C`가 3초 중립 캘리브레이션 시작/취소, `F1`이 운영 UI 표시/숨김입니다. 저장된 카메라가 없는 아바타만 최초 전신 자동 맞춤을 적용합니다.

## 검증

### Python 계약 테스트

```powershell
cd vp-tracker
uv run python -m unittest -v test_protocol.py test_obs_control.py test_tracker.py test_lifecycle.py
```

### Unreal 자동화 테스트

```powershell
UnrealEditor-Cmd.exe VPPipeline/VPPipeline.uproject `
  -Unattended -NullRHI -NoSound `
  '-ExecCmds=Automation RunTests VPPipeline.; Quit'
```

2026-09-09 기준 검증 결과:

- Python 프로토콜·OBS 제어·비동기 결합·생명주기 테스트 42/42 성공
- Unreal 자동화 테스트 26/26 성공
  - 정상 VPTP 스키마 3 파싱
  - 절단·추가 데이터·잘못된 count·NaN·Inf 거부
  - 좌우 상완 90° 계산과 낮은 confidence 거부
  - 중립값·gain·반전·각도 wrap 매핑
  - 일시적 큰 입력 거부와 지속 입력 수용
  - 상완 confidence 히스테리시스·유예와 30/120fps 시간 기반 보간
  - 중립·왼팔·오른팔 단계 판정
  - 아바타 프로필 SaveGame 메모리 직렬화
  - 아바타 콘텐츠 ID·경로 검증과 보관함 직렬화
  - 아바타별 방송 카메라 직렬화
  - 캘리브레이션·팔 검증 상태별 상단 안내 문구
  - Pose 랜드마크 투영·confidence, 방송 sRGB 색상 변환과 16:9 맞춤 영역 계산
- AMD Ryzen 9 7940HS / Development Editor / NullRHI 측정
  - Python VPTP 스키마 3 인코더 10,000회: 평균 `48.11 µs/packet`, `20,786 packets/s`
  - Unreal C++ VPTP 스키마 3 파서 100,000회: 평균 `2.012 µs/packet`, `497,134 packets/s`
  - 실제 Face+Pose 검출: Heavy 순차 처리 약 `10~12fps` → Full 비동기 처리 약 `29~31fps`
- UE 5.8 `VPPipelineEditor` Development 빌드 성공
- UE 5.8 Win64 Development 게임 타깃 빌드 성공
- 실제 Supervisor 세션에서 약 30fps 추론, 중복 실행 차단, Unreal 창 종료 시 트래커 정상 종료·웹캠 해제·종료코드 0 확인
- 실제 웹캠에서 얼굴 3×3 회전 행렬·52 Blendshape·33 Pose를 30fps로 생성하고, Seed-san에서 머리와 상완 트래킹 결과를 사용자 시각 확인
- 실제 `MINI.vrm`과 한글·공백 경로의 `Seed-san.vrm` 드롭, 교체 및 재실행 복원 확인
- 공식 VRM4U ZIP의 다운로드·SHA-256 검증·패치 적용과 재실행 멱등성 확인
- UE 5.8 공식 `ModelContextProtocol` 및 28개 Editor/Animation toolset 초기화 확인
- UE 5.8 DX12 실행에서 1280×720 SceneCapture 전신 프레이밍과 `Virtual Production Pipeline` Spout 송신 확인
- OBS 32.2.2 `Spout2 Capture`에서 배경 제거 출력의 투명 픽셀 알파 0과 아바타 픽셀 알파 255, Premultiplied Alpha 합성 모드 적용을 확인
- Unreal 배경 모드 변경이 localhost 제어 채널을 거쳐 OBS Spout 합성 모드에 동기화되고 앱이 관리하던 기존 크로마키 필터가 제거됨을 확인
- OBS 32.2.2 유휴 상태에서 현재 60fps 조회와 무변경 동기화 경로를 확인하고, 출력 중 FPS 변경 거부·응답 왕복을 자동 테스트로 검증
- OBS 32.2.2에서 로컬 MP4 녹화 시작·상태·중지·저장 경로와 비어 있지 않은 파일 생성을 확인
- `stream`은 10초 확인 대기만 설정하고, 만료된 `stream confirm`은 거부되어 실제 방송이 시작되지 않음을 확인

외부 서비스의 실제 방송 시작·중지는 별도 명시적 승인과 테스트 방송 대상이 필요한 수동 검증 항목입니다.

## 알려진 제약

- 2D 화면상 어깨–팔꿈치 방향을 사용하므로 카메라 정면 기준 팔 벌리기·들기 표현에 최적화되어 있습니다.
- 상완 회전 축과 좌우 부호는 VRoid 리그/Anim Blueprint에 맞춰 조정해야 합니다.
- 공식 Unreal MCP는 Experimental 기능이며 런타임 필수 의존성이 아닙니다.
