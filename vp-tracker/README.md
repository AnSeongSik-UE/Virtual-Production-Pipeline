# VP Tracker

MediaPipe Face/Pose 추론 결과를 엄격한 `Virtual Production Tracking Packet`(`VPTP`) 스키마 3 Binary UDP로 Unreal Engine에 전송하는 Python 구성요소입니다.

## 빠른 시작

```powershell
uv sync
..\tools\mediapipe\Install-MediaPipeModels.ps1
Copy-Item .env.example .env
uv run launcher.py
```

`launcher.py`는 단일 Supervisor 진입점입니다. Unreal 게임과 트래커를 함께 실행하고 세션 heartbeat를 감시하며, Unreal 종료 시 트래커와 웹캠을 자동 정리합니다. 중복 실행은 차단되고 Supervisor 비정상 종료 시 Windows Job Object가 관리 자식 프로세스를 종료합니다. OBS는 선택 기능이자 외부 소유 프로세스이므로 함께 종료하지 않으며, 비밀번호는 `.env`의 `VP_OBS_PASSWORD`에서만 읽습니다. 로컬 녹화는 `rec`/`stoprec`으로 제어하고, 실제 방송은 `stream` 입력 후 10초 안에 `stream confirm`을 입력해야 시작합니다. `status`는 방송과 녹화 상태를 분리해 표시합니다.

기본 실시간 모드는 `models/face_landmarker.task`와 `models/pose_landmarker_full.task`를 사용합니다. 고품질 Heavy 모델도 설치하려면 설치 스크립트에 `-IncludeHeavy`를 지정하고 실행 전에 `$env:VP_POSE_MODEL='heavy'`를 설정합니다. 모델은 공식 URL에서 다운로드하고 SHA-256을 검증하며 Git에는 포함하지 않습니다.

## 주요 파일

- `protocol.py`: 얼굴 회전·52 Blendshape·33 Pose를 담는 928-byte VPTP 스키마 3 인코더
- `tracker.py`: 1280×720 캡처, Face 원본/640×360 Pose 비동기 최신 프레임 추론, confidence 추출, 작업 스레드 오류 전달
- `sender.py`: localhost:7000 UDP 송신
- `obs_control.py`: Unreal의 localhost:7001 배경·생명주기 명령 검증·수신
- `obs_controller.py`: Spout 입력 선택, 배경 제거용 Premultiplied Alpha/단색용 Opaque 합성 모드 동기화, 기존 앱 관리 크로마키 필터 제거, 출력 상태를 보호하는 OBS FPS 동기화와 방송·녹화 상태/제어
- `launcher.py`: Supervisor 실행 진입점
- `supervisor.py`: 필수 사전 점검, Unreal·트래커 실행/감시, OBS 방송 2단계 확인·녹화 제어와 단계적 종료
- `lifecycle.py`: 세션 토큰·heartbeat lease·단일 인스턴스·Windows Job Object
- `mcp_server.py`: 웹캠·모델·UDP·OBS·프로토콜 읽기 전용 진단
- `test_protocol.py`: MediaPipe 없이 실행 가능한 계약 테스트
- `test_obs_control.py`: OBS 연결 없이 실행 가능한 배경 모드·FPS 명령, 출력 중 변경 거부, 방송·녹화 상태/중복 명령, Unreal 응답 왕복과 Spout 합성 설정 테스트
- `test_tracker.py`: Pose 모델 선택과 Face/Pose 최신 결과 결합 테스트
- `test_lifecycle.py`: heartbeat timeout, 방송 2단계 확인, 단계적 종료, 단일 인스턴스와 Job Object 테스트
- `benchmark_protocol.py`: VPTP 인코딩 처리량 측정

## 테스트

```powershell
uv run python -m unittest -v test_protocol.py test_obs_control.py test_tracker.py test_lifecycle.py
uv run test_phase1.py
uv run benchmark_protocol.py --iterations 10000
```

`test_phase1.py`는 실제 웹캠과 모델이 필요하고 실패 시 비정상 종료 코드를 반환합니다. `test_phase3.py`는 Unreal PIE가 실행된 상태에서 Blink/Smile VPTP 스키마 3 샘플을 전송합니다.

## MCP 진단 서버

```powershell
uv run mcp dev mcp_server.py
```

도구는 상태 조회만 수행하며 OBS 비밀번호를 도구 입력이나 결과로 노출하지 않습니다. UDP는 connectionless 프로토콜이므로 포트 바인딩 여부만 확인하며 소유 프로세스가 Unreal인지 단정하지 않습니다.
