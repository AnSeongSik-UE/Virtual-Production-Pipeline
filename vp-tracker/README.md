# VP Tracker

MediaPipe Face/Pose 추론 결과를 엄격한 `Virtual Production Tracking Packet`(`VPTP`) 스키마 3 Binary UDP로 Unreal Engine에 전송하는 Python 구성요소입니다.

## 빠른 시작

```powershell
uv sync
..\tools\mediapipe\Install-MediaPipeModels.ps1
uv run launcher.py
```

`launcher.py`는 단일 Supervisor 진입점입니다. Unreal 게임과 트래커를 함께 실행하고 세션 heartbeat를 감시하며, Unreal 종료 시 트래커와 웹캠을 자동 정리합니다. 중복 실행은 차단되고 Supervisor 비정상 종료 시 Windows Job Object가 관리 자식 프로세스를 종료합니다. OBS 설치나 WebSocket 인증은 필요하지 않습니다.

기본 실시간 모드는 `models/face_landmarker.task`와 `models/pose_landmarker_full.task`를 사용합니다. 고품질 Heavy 모델도 설치하려면 설치 스크립트에 `-IncludeHeavy`를 지정하고 실행 전에 `$env:VP_POSE_MODEL='heavy'`를 설정합니다. 모델은 공식 URL에서 다운로드하고 SHA-256을 검증하며 Git에는 포함하지 않습니다.

## 주요 파일

- `protocol.py`: 얼굴 회전·52 Blendshape·33 Pose를 담는 928-byte VPTP 스키마 3 인코더
- `tracker.py`: 1280×720 캡처, Face 원본/640×360 Pose 비동기 최신 프레임 추론, confidence 추출, 작업 스레드 오류 전달
- `sender.py`: localhost:7000 UDP 송신
- `control_channel.py`: Unreal의 localhost:7001 카메라 선택·생명주기 명령 검증·수신
- `launcher.py`: Supervisor 실행 진입점
- `supervisor.py`: 필수 사전 점검, Unreal·트래커 실행/감시와 단계적 종료
- `lifecycle.py`: 세션 토큰·heartbeat lease·단일 인스턴스·Windows Job Object
- `mcp_server.py`: 웹캠·모델·UDP·프로토콜 읽기 전용 진단
- `test_protocol.py`: MediaPipe 없이 실행 가능한 계약 테스트
- `test_control_channel.py`: 카메라 명령·응답, localhost 검증과 lifecycle 결합 테스트
- `test_tracker.py`: Pose 모델 선택과 Face/Pose 최신 결과 결합 테스트
- `test_lifecycle.py`: heartbeat timeout, 단계적 종료, 단일 인스턴스와 Job Object 테스트
- `benchmark_protocol.py`: VPTP 인코딩 처리량 측정

## 테스트

```powershell
uv run python -m unittest discover -v -p "test_*.py"
uv run test_phase1.py
uv run benchmark_protocol.py --iterations 10000
```

`test_phase1.py`는 실제 웹캠과 모델이 필요하고 실패 시 비정상 종료 코드를 반환합니다. `test_phase3.py`는 Unreal PIE가 실행된 상태에서 Blink/Smile VPTP 스키마 3 샘플을 전송합니다.

## MCP 진단 서버

```powershell
uv run mcp dev mcp_server.py
```

도구는 상태 조회만 수행합니다. UDP는 connectionless 프로토콜이므로 포트 바인딩 여부만 확인하며 소유 프로세스가 Unreal인지 단정하지 않습니다.
