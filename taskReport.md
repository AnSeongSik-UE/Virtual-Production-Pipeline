# Task Report

## 2026.04.06 23:16:38
### guide.md 실현가능성 검토
- guide.md 전체 내용 분석 완료
- MediaPipe Holistic 레거시 상태 확인 → MediaPipe Tasks API(FaceLandmarker, PoseLandmarker) 전환 필요
- UE 5.7 출시 확인 (2025.11.12 릴리즈)
- MediaPipe FaceLandmarker의 ARKit 52 블렌드쉐이프 네이티브 출력 지원 확인
- OBS WebSocket v5 Python 라이브러리(`obsws-python`) 사용 가능 확인
- 각 레이어별(Tracking, Rendering, Broadcasting) 기술적 실현가능성 평가 완료
- 4주 마일스톤 현실성 분석 → 6~8주 권장
- 수정 권장사항 4건 도출
- 결론: 기술적으로 실현 가능, 일부 용어/일정 수정 권장

## 2026.04.06 23:25:49
### guide.md 카메라 구성 변경 (휴대폰 → 노트북 웹캠)
- 모바일 카메라(IP WebCam/RTSP) → 노트북 내장 웹캠으로 전면 변경
- 다중 카메라 구조 → 단일 웹캠 구조로 아키텍처 단순화
- 변경 영역: 프로젝트 개요, 핵심 과제, Tracking Layer, Broadcasting Layer, 3.1 세부 전략, 포트폴리오 강조 포인트, Week 1 마일스톤
- RTSP/IP Webcam/모바일 관련 내용 모두 제거

## 2026.04.07 06:48:48
### guide.md 실현가능성 검토 결과 반영 및 전면 개선
- MediaPipe Holistic(레거시) → MediaPipe Tasks API (FaceLandmarker + PoseLandmarker) 용어/구현 전환
- FaceLandmarker 네이티브 ARKit 52 블렌드쉐이프 출력 기능 명시 (커스텀 변환 로직 불필요)
- 최소 하드웨어 요구사항 추가 (RTX 3060+, RAM 16GB+)
- AI 자동화 스택 추가 (Claude Desktop + MCP, Unreal MCP Server, Cursor/Gemini CLI)
- 마일스톤 4주 → 6주로 현실적 조정 (Week 2~3, Week 4~5 묶음)
- 각 주차별 구체적 작업 항목 및 검증 기준 추가
- 1PC 환경 GPU 분배 전략(렌더링 vs NVENC 인코딩) 명시
- Lumen 저사양 대안(Screen Space GI) 명시

### workflow.md 상세 워크플로우 문서 신규 작성
- .agents/workflows/workflow.md 생성
- Phase 0~4 (6주) 단계별 실행 가이드 작성
- Phase 0: 개발 환경 셋업 (Python uv, UE5.7, MCP 서버)
- Phase 1: MediaPipe Tracking 모듈 (tracker.py, sender.py 참조 코드 포함)
- Phase 2: UE5.7 C++ 수신 플러그인 (VPTrackerReceiver 구조, 핵심 헤더 코드)
- Phase 3: 애니메이션 리타겟팅 + Unreal Insights 프로파일링 워크플로우
- Phase 4: OBS WebSocket 방송 자동화 + 원클릭 런처
- 프로젝트 전용 MCP 서버(mcp_server.py) 설계 포함: 5개 진단 도구
- 각 Phase별 AI 자동화 활용 포인트 명시
- 트러블슈팅 가이드 및 성능 최적화 우선순위 포함
- 최종 프로젝트 디렉토리 구조 명시

## 2026.04.09 08:01:49
### 보안 취약점 검토 및 수정
- guide.md, workflow.md 전체 보안 취약점 분석 수행 (8건 식별)
- [수정] OBS WebSocket 비밀번호 하드코딩(`password=''`) → 환경변수(`VP_OBS_PASSWORD`) 로드 방식으로 전환
- [수정] MCP 서버 `check_obs_connection`에서 password 파라미터 노출 → 환경변수 내부 로드로 변경
- [수정] UDP 패킷 무결성 검증 없음 → CRC32 체크섬 + 패킷 크기 제한(4096B) 추가
- [수정] `except:` bare exception 7건 → 구체적 예외 타입(`Empty`, `OSError`, `ImportError`, `ConnectionRefusedError` 등)으로 교체
- [수정] RawUDPSender 외부 IP 전송 시 경고 없음 → `warnings.warn` 추가
- [수정] MCP 서버 `mcp.run()` → `mcp.run(transport="stdio")` stdio 전송만 허용
- [추가] guide.md에 섹션 3.5 보안 가이드라인 신규 추가 (네트워크 바인딩, 패킷 검증, 비밀정보 분리, MCP 접근 제한, 예외 처리)
- [추가] MCP 서버에 포트 범위 검증(1024~65535), 로깅(`logging`) 도입
- [추가] workflow.md MCP 서버 코드 상단에 보안 고려사항 docstring 추가

### AI 자동화 도구 명칭 변경 (Cursor → Antigravity)
- guide.md: Cursor/Gemini CLI → Antigravity/Gemini CLI (3건)
- guide.md: Claude/Cursor → Claude/Antigravity (1건)
- workflow.md: Cursor → Antigravity (5건)
- workflow.md: Cursor/Gemini CLI → Antigravity/Gemini CLI (1건)

## 2026.04.09 08:16:13
### 과잉보안 항목 롤백 (1인 개발 + localhost 환경 기준)
- [롤백] OBS 비밀번호 환경변수(`VP_OBS_PASSWORD`) → `password=''` 단순 파라미터 복원 (localhost 전용, 외부 노출 불가)
- [롤백] UDP CRC32 체크섬 + 패킷 크기 제한 제거 (localhost UDP 손상 가능성 0)
- [롤백] RawUDPSender 외부 IP 경고 `warnings.warn` 제거 (외부 전송 미사용)
- [롤백] MCP 서버 포트 범위 검증(1024~65535) 제거 (자체 호출 전용)
- [롤백] MCP `transport="stdio"` 명시 제거 → `mcp.run()` 기본값 복원
- [롤백] MCP 서버 `logging` 모듈 도입 제거 (print 수준 충분)
- [롤백] MCP 서버 보안 고려사항 docstring 제거
- [롤백] guide.md 섹션 3.5 보안 가이드라인 전체 삭제
- [유지] `except:` bare exception → 구체적 예외 타입 (`ImportError`, `ConnectionRefusedError`, `OSError`, `Empty` 등) — 보안이 아닌 코드 품질/디버깅 용이성

## 2026.04.10 07:29:12
### workflow.md 12,000자 제한 대응 — Phase별 파일 분할
- 기존 workflow.md (28,927자, 976줄) → 6개 파일로 분할
- workflow.md (3,844자): 마스터 인덱스 (전체 흐름도 + Phase 링크 테이블 + 프로젝트 구조 + MCP 도구 요약)
- phase0_setup.md (4,692자): Phase 0 — Python 환경, UE5.7 프로젝트, MCP 서버 셋업
- phase1_tracking.md (7,858자): Phase 1 — MediaPipe 트래킹 + UDP 네트워크 발신 모듈
- phase2_ue_plugin.md (4,240자): Phase 2 — UE5.7 C++ 수신 플러그인 개발
- phase3_animation.md (2,783자): Phase 3 — 애니메이션 리타겟팅 + 프로파일링 최적화
- phase4_broadcast.md (7,353자): Phase 4 — 방송 자동화 + 트러블슈팅 가이드
- 모든 파일 12,000자 이하 (최대 7,858자) 확인 완료
- 각 파일에 frontmatter description 추가 (슬래시 커맨드 인식용)

## 2026.04.10 08:08:48
### Phase 0: 개발 환경 셋업 완료
- uv 0.11.6 설치 (astral.sh 공식 인스톨러)
- `uv init vp-tracker --python 3.12` 프로젝트 초기화
- 의존성 설치: mediapipe 0.10.33, opencv-python 4.13.0, python-osc 1.10.2, obsws-python 1.8.0, numpy 2.4.4 등 22패키지
- `uv add "mcp[cli]"` MCP 서버 의존성 추가 (mcp 1.27.0 등 36패키지)
- `vp-tracker/mcp_server.py` 생성 (5개 진단 도구: check_webcam, check_port_available, check_obs_connection, check_mediapipe_models, run_pipeline_diagnostics)
- `claude_desktop_config.json` MCP 서버 2개 등록 (unreal-mcp, vp-pipeline)
- `unreal-mcp` clone 경로: `C:\UnrealWork\tools\unreal-mcp` (VP 레포와 분리)
- phase0_setup.md 수정: unreal-mcp 경로/엔트리포인트 실제 구조 반영
- UE 5.7 프로젝트 생성 + 플러그인 활성화 (사용자 수동 완료)
- MCP 서버 import 및 도구 등록 검증 완료

## 2026.04.10 08:35:24
### Phase 1: MediaPipe 트래킹 + UDP 발신 모듈 구현
- MediaPipe 모델 다운로드: face_landmarker.task (3.6MB), pose_landmarker_heavy.task (29.2MB)
- `tracker.py` 구현: UnifiedTracker 클래스 (FaceLandmarker + PoseLandmarker 동시 추론, Thread-safe Queue)
- `sender.py` 구현: RawUDPSender (바이너리 UDP) + OSCSender (OSC 프로토콜) 듀얼 모드
- `test_phase1.py` 검증 스크립트 작성
- Windows CP949 인코딩 이슈 해결: 모든 .py 파일에서 이모지 → ASCII 텍스트로 교체
- 검증 결과 (1280x720, CPU XNNPACK):
  - [PASS] 웹캠 캡처: 108프레임/8초
  - [PASS] ARKit 52 블렌드쉐이프: 52개, 108/108 프레임 검출 (100%)
  - [PASS] PoseLandmarker 33 랜드마크: 33개
  - [FAIL] 추론 FPS: 14.1fps (목표 20fps 미달) — Face+Pose 동시 순차추론 CPU 부하
- FPS 최적화는 Phase 3 프로파일링 단계에서 대응 예정 (모델 교체, 해상도 축소 등)

## 2026.04.10 14:33:31
### Phase 2: UE5.7 C++ 수신 플러그인 생성
- `VPTrackerReceiver` 플러그인 디렉토리 구조 생성 (Plugins/VPTrackerReceiver/)
- `VPTrackerReceiver.uplugin` 플러그인 정의 파일
- `VPTrackerReceiver.Build.cs` (Sockets, Networking, AnimGraphRuntime 의존성)
- `VPTrackerReceiver.h/.cpp` 모듈 등록
- `VPTrackingData.h/.cpp` 트래킹 데이터 구조체 (FVPBlendshapeData, FVPPoseLandmark, FVPTrackingFrame)
- `VPUDPReceiver.h/.cpp` UDP 수신 컴포넌트 (SPSC TQueue, FUdpSocketReceiver, 바이너리 패킷 파서)
  - ARKit 52 블렌드쉐이프 이름 매핑 테이블 내장
  - Python sender.py의 VPFR 매직 헤더 + 바이너리 포맷과 1:1 대응
- `VPAnimInstance.h/.cpp` 블렌드쉐이프 -> MorphTarget 브릿지 AnimInstance
- `VPPipeline.uproject`에 VPTrackerReceiver 플러그인 등록
- UHT 헤더 파싱 성공 (문법/UCLASS/USTRUCT 오류 없음)
- 빌드 이슈 해결: `Sockets/Public/IPAddress.h` include 경로 오류 수정
- UE 에디터 컴파일 성공
- 검증 완료: Python sender.py -> UDP -> UVPUDPReceiver 패킷 수신 정상 (BS=52, Pose=33)

## 2026.04.10 (Phase 3)
### Phase 3: 애니메이션 리타겟팅 C++ 구현
- VPTrackingData.h: `FVPBlendshapeMapping` DataTable row 구조체 추가 (ARKitName, MorphTargetName, Scale)
- VPAnimInstance.h 확장:
  - `BlendshapeMappingTable` (UDataTable*): ARKit -> Morph Target 이름 리매핑 (DataTable 기반, 런타임 교체 가능)
  - `FaceBlendshapes` (TMap<FName, float>): 리매핑 후 블렌드쉐이프 값 (Blueprint/AnimGraph 접근용)
  - `PoseBoneTransforms` (TArray<FTransform>): MediaPipe 33 랜드마크 -> UE 좌표계 변환
  - `PosePositionScale`: 정규화 좌표(0~1) -> UE 단위 스케일 팩터
  - `bAutoFindReceiver`: 동일 Actor의 VPUDPReceiver 자동 연결
  - `ApplyTrackingData()`: 블렌드쉐이프 + 포즈 통합 적용 함수
  - `UpdatePoseBoneTransforms()`: 포즈 -> 본 트랜스폼 변환 함수
- VPAnimInstance.cpp 구현:
  - `NativeInitializeAnimation()`: VPUDPReceiver 자동 탐색/캐싱
  - `NativeUpdateAnimation()`: 매 프레임 수신기에서 데이터 풀링 + 블렌드쉐이프/포즈 적용
  - `CacheMappingTable()`: DataTable 첫 사용 시 캐싱 (매 프레임 조회 방지)
  - `ApplyBlendshapesToMorphTargets()`: DataTable 리매핑 적용, 매핑 없으면 ARKit 이름 직접 사용
  - `UpdatePoseBoneTransforms()`: MediaPipe (X우,Y하,Z전방) -> UE (X전방,Y우,Z상) 좌표 변환
- UE 에디터 컴파일 성공 (오류 없음)
- VRoid 아바타 Morph Target 매핑: ARKit 이름과 VRoid 이름 불일치 확인
  - VRoid 명명 체계: Fcl_EYE_Close_L, Fcl_MTH_A 등 (ARKit: eyeBlinkLeft, jawOpen 등)
  - VRoidBlendshapeMapping.csv 생성 (21개 ARKit -> VRoid 매핑)
  - UE DataTable 임포트 후 VPAnimInstance에 할당
- 검증 완료 (test_phase3.py):
  - [PASS] 패킷 포맷: 620B, Magic VPFR, BS=52, Pose=33
  - [PASS] UDP 수신: ~60Hz, 351프레임 전송/수신 정상
  - [PASS] AnimInstance 자동 연결: VPUDPReceiver auto-connect 로그 확인
  - [PASS] DataTable 리매핑: ARKit -> VRoid Morph Target 변환 정상
  - [PASS] 시각적 검증: 눈 깜빡임(eyeBlink -> Fcl_EYE_Close) + 입 벌림(jawOpen -> Fcl_MTH_A) 확인
- VRoid 매핑 튜닝:
  - mouthPucker 비활성화 (MediaPipe 상시 노이즈 0.3~0.5)
  - mouthFrownLeft/Right 비활성화 (노이즈)
  - eyeBlink 스케일 1.0 -> 1.5 (완전 감김 보장)
  - jawOpen 데드존 0.15 적용 (미세 노이즈 필터링)
- VPAnimInstance 추가 기능:
  - BlendshapeDeadZone: 임계값 미만 블렌드쉐이프 값 0 처리 (기본 0.15)
  - RestPoseMorphTargets: 기본 Morph Target 값 설정 (Fcl_MTH_Close=1.0 등)
  - Rest pose는 트래킹 데이터 없이도 항상 적용
- 웹캠 실시간 트래킹 검증:
  - [PASS] sender.py -> UDP -> VPAnimInstance 실시간 파이프라인 정상
  - [PASS] 눈 깜빡임 + 입 벌림 실시간 반영 확인
- Unreal Insights 프로파일링:
  - [PASS] Frame Time: ~10.4ms (약 96fps, 목표 60fps 초과)
  - [PASS] GPU: 평균 16.4ms (60fps 이내)
  - [PASS] GameThread: 안정 (트래킹 파이프라인 오버헤드 무시 수준)
  - CVar 튜닝 불필요 판단 (Lumen GI 간헐 스파이크만 존재)

## 2026.04.12 (Phase 4)
### Phase 4: OBS WebSocket 방송 자동화 + 원클릭 런처
- `obs_controller.py` 구현:
  - OBS WebSocket v5 연결 (obsws-python, port 4455)
  - 씬 목록 조회, 씬 전환, 스트리밍 시작/중지, 녹화 시작/중지, 상태 조회
  - OBS 비밀번호 `.env` 파일에서 로드 (코드 하드코딩 방지)
- `launcher.py` 구현:
  - Preflight check: 웹캠, UDP 포트, OBS WebSocket, MediaPipe 모델
  - Tracker(sender.py) 자동 실행 + OBS 자동 연결
  - 대화형 명령: stream, stop, rec, stoprec, status, quit
  - 종료 시 스트리밍 중지 + 프로세스 정리
- `.env` 파일 생성 (OBS WebSocket 비밀번호), `.gitignore`에 `.env` 추가
- Preflight check 검증:
  - [PASS] Webcam: Available
  - [PASS] UDP port 7000: Available
  - [PASS] OBS WebSocket: Connected (OBS v32.1.1, WebSocket v5.7.3)
  - [PASS] MediaPipe models: All models found

## 2026.04.12 (Phase 5)
### Phase 5: 헤드 트래킹 + 크로마키 + UnrealMCP 포트
- VPAnimInstance 헤드 트래킹 구현:
  - MediaPipe PoseLandmark 0(코), 11(좌어깨), 12(우어깨)로 Yaw/Pitch/Roll 계산
  - 어깨 폭 기준 정규화 → 회전 각도 변환 (Yaw 60, Pitch 60, Roll 40 계수)
  - VRoid J_Bip_C_Head 본 축 매핑: FRotator(Roll, Yaw, Pitch) 축 스왑
  - 축 부호 멀티플라이어 추가 (YawSign=-1, PitchSign=-1, RollSign=1) — 에디터에서 리빌드 없이 조정 가능
  - HeadRotationSmoothing: FMath::Lerp 기반 스무딩 (사용자 0.95 설정으로 떨림 해소)
  - HeadRotation UPROPERTY → AnimBP Transform (Modify) Bone 노드에서 사용
  - 직접 본 조작(GetBoneTransformByName 등) 시도 → UE 5.7 API 제한으로 AnimBP 방식 채택
- AnimBP 설정:
  - Transform (Modify) Bone 노드: J_Bip_C_Head 본 대상
  - HeadRotation 변수 연결 (AnimInstance에서 매 프레임 갱신)
- 크로마키 배경:
  - 아바타 뒤 초록 플레인 배치 + M_GreenScreen 머티리얼
  - OBS Chroma Key 필터로 배경 제거 확인
- UnrealMCP 플러그인 UE 5.7 포트:
  - tools/unreal-mcp → VPPipeline/Plugins/UnrealMCP 복사
  - BufferSize → MCPBufferSize 이름 충돌 해결
  - ANY_PACKAGE(deprecated) → nullptr 교체 (9건, 2파일)
  - 빌드 성공, MCP 서버 port 55557 정상 기동 확인
- 디버그 도구:
  - debug_all_bs.py: 표정별 블렌드쉐이프 활성값 실시간 확인

### 헤드 트래킹 추가 수정
- 극단적 자세(깊이 숙이기)에서 랜드마크 노이즈로 머리 부들부들 현상 발생
  - HeadMaxDeltaPerFrame 추가 (프레임당 최대 회전 변화량 제한, 기본 5.0도) → 해결
- 기울이기(Roll) 시 Yaw 오염 문제 발생
  - 어깨 Roll 기반 보상 시도 → 실패 (어깨 Roll != 머리 Roll, 머리만 기울일 때 보정 안 됨)
  - 근본 해결은 FaceLandmarker의 facial_transformation_matrixes 사용 필요
  - 현재는 사용자가 에디터에서 YawSign/PitchSign/RollSign 수동 조정으로 대응
  - 확정 기본값: YawSign=-1, PitchSign=1, RollSign=-1

### 상체 트래킹 시도 및 취소
- Two Bone IK 방식으로 상체(팔) 트래킹 시도
  - MediaPipe PoseLandmark 11-16 (어깨/팔꿈치/손목) → IK Effector/Joint Target
  - MediaPipeToUE 좌표 변환 + 아바타 어깨 본 위치 기준 오프셋
  - AnimBP Two Bone IK 노드 (J_Bip_L/R_Hand) 연결
- 문제점:
  - 좌표 스페이스 불일치 (월드 vs 컴포넌트)
  - 스케일 매칭 어려움 (MediaPipe 정규화 좌표 → 아바타 본 길이)
  - 팔 관절 부자연스럽게 꺾임
  - 손가락 트래킹 불가 (PoseLandmarker는 손목까지만, HandLandmarker 별도 필요)
- 결론: 손가락 트래킹 없이는 상체만 움직여도 부자연스러움 → 전체 취소, 코드 롤백

### jawOpen 스케일 조정
- VRoidBlendshapeMapping.csv: jawOpen(Fcl_MTH_A) 스케일 1.0 → 2.0 (입 크게 벌리기)
