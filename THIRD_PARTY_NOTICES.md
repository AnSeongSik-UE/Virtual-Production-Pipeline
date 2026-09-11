# Third-Party Notices

이 문서는 Virtual Production Pipeline이 참조하거나 별도 설치하는 외부 구성요소와 데모 에셋의 출처를 기록합니다. 저장소 자체 코드에 대한 사용 권한을 부여하는 문서가 아니며, 각 구성요소에는 해당 권리자의 최신 라이선스와 이용조건이 적용됩니다.

## 저장소에 포함된 파생 자료

### VRM4U compatibility patch

- Upstream: [ruyo/VRM4U](https://github.com/ruyo/VRM4U)
- License: [MIT](https://github.com/ruyo/VRM4U/blob/master/LICENSE)
- Copyright: Copyright (c) 2018 Haruyoshi Yamamoto
- Included scope: `tools/vrm4u/patches/VRM4U-v1.2026.07.22-UE58-runtime.patch`

The following MIT license applies to the portions of VRM4U represented by the compatibility patch.

> MIT License
>
> Copyright (c) 2018 Haruyoshi Yamamoto
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

## 소스 저장소에는 포함되지 않는 구성요소

| 구성요소 | 확인한 조건 | 저장소 처리 |
|---|---|---|
| [Unreal Engine](https://www.unrealengine.com/eula/unreal) | Epic Games EULA | 엔진 코드·바이너리 미포함, 사용자 별도 설치 |
| [VRM4U](https://github.com/ruyo/VRM4U) | MIT, 포함된 assimp는 3-clause BSD 등 자체 고지 적용 | 플러그인 본체 제외, 고정 릴리스 설치 스크립트와 호환성 패치만 포함 |
| [UE5_Spout2_DX12](https://github.com/GPUbrainStorm/UE5_Spout2_DX12) | MIT, 포함된 Spout SDK는 소스 내 BSD 고지 적용 | 플러그인 본체 제외, 사용자 별도 설치 |
| [MediaPipe](https://github.com/google-ai-edge/mediapipe) | Apache-2.0 | Python 패키지는 `uv`로 설치, 모델 번들은 Google 공식 저장소에서 별도 다운로드 |
| [OpenCV](https://github.com/opencv/opencv) | Apache-2.0 | `uv`로 별도 설치 |
| [cv2-enumerate-cameras](https://github.com/lukehugh/cv2_enumerate_cameras) | MIT | Windows 카메라 장치명·경로·OpenCV 인덱스 열거, 버전은 `vp-tracker/uv.lock`에 고정 |
| Python 직접·간접 의존성 | 각 패키지의 배포 메타데이터와 라이선스 | 소스 저장소에는 미포함, 버전과 해시는 `vp-tracker/uv.lock`에 기록 |

MediaPipe `.task` 모델은 Git 저장소에 포함하지 않습니다. `tools/mediapipe/Install-MediaPipeModels.ps1`가 공식 URL에서 다운로드하고 SHA-256을 확인합니다.

## 원클릭 배포본에 포함되는 구성요소

`tools/Build-OneClick.ps1`로 생성하는 로컬 배포 폴더에는 패키징된 Unreal 런타임, 컴파일된 VRM4U·Spout 플러그인, Python 런타임과 직접·간접 의존성, MediaPipe `.task` 모델이 포함됩니다. 사용자 VRM은 포함하지 않습니다.

- PyInstaller 6.22.2는 `GPL-2.0-or-later with Bootloader Exception` 조건으로 빌드 도구와 부트로더를 사용합니다.
- 생성된 `ThirdPartyLicenses/`에는 빌드 환경의 Python 배포 메타데이터와 사용 가능한 라이선스 원문, OpenCV 고지, Spout2_DX12 및 VRM4U에 포함된 assimp·RapidJSON 고지를 복사합니다.
- Unreal 패키지의 `Runtime/NOTICES.txt`와 Visual C++ 재배포 파일도 원래 구조로 유지합니다.
- 모델 및 각 런타임 구성요소를 외부에 재배포할 때에는 이 요약만 의존하지 말고 해당 릴리스에 적용되는 원저작자의 조건과 Unreal Engine EULA를 확인해야 합니다.

## 데모 및 사용자 아바타

### Seed-san

- Character/model: Seed-san
- Rights holder: VirtualCast, Inc.
- Terms: [Seed-san 공식 배포 및 이용조건](https://wiki.virtualcast.jp/wiki/vrm/seedsanvrm)
- Repository scope: 데모 GIF와 검증 결과의 화면 표시만 포함하며 `.vrm` 모델 데이터는 포함하지 않음

### User-supplied VRM files

사용자가 선택한 원본과 앱의 관리 복사본은 Git에서 제외됩니다. 각 VRM의 저작권, 크레딧, 상업 이용, 개조 및 재배포 조건은 파일의 내장 VRM 메타데이터와 원저작자의 별도 약관이 우선합니다. 이 프로젝트가 특정 아바타의 이용 권한을 부여하지 않습니다.
