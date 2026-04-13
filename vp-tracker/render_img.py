import base64
import urllib.request
import os

code = """graph TD
    classDef hardware fill:#d1dcff,stroke:#2b5ca8,stroke-width:2px,color:#000
    classDef python fill:#e8f4e8,stroke:#2e7d32,stroke-width:2px,color:#000
    classDef ue fill:#ffebee,stroke:#c62828,stroke-width:2px,color:#000
    classDef broadcast fill:#fff3e0,stroke:#e65100,stroke-width:2px,color:#000
    classDef network fill:#f3e5f5,stroke:#4a148c,stroke-width:2px,color:#000,stroke-dasharray: 4 4

    subgraph Input ["영상 입력 (Hardware)"]
        Cam["노트북 내장 웹캠 (720p 30fps+)"]:::hardware
    end

    subgraph Tracking ["Tracking Layer (Python)"]
        Launcher["launcher.py (Preflight & Setup)"]:::python
        Tracker["tracker.py (Unified Tracker)"]:::python
        MP_Face["MediaPipe FaceLandmarker (52 ARKit Blendshapes)"]:::python
        MP_Pose["MediaPipe PoseLandmarker (33 Landmarks)"]:::python
        Sender["sender.py (Binary UDP Sender)"]:::python

        Launcher -->|1. Starts| Tracker
        Tracker -->|Capture| Cam
        Tracker -->|Frames| MP_Face
        Tracker -->|Frames| MP_Pose
        MP_Face -->|ARKit Data| Sender
        MP_Pose -->|Pose Data| Sender
    end

    subgraph Network ["Network Transport"]
        UDP["UDP Protocol (Port 7000, 620B VPFR Packets)"]:::network
        Sender -->|Send Binary Data| UDP
    end

    subgraph Unreal ["Rendering Layer (Unreal Engine 5.7)"]
        UPlugin["VPTrackerReceiver (C++ Plugin)"]:::ue
        UDPReceiver["VPUDPReceiver (SPSC Thread-Safe Queue)"]:::ue
        VPAnim["VPAnimInstance (C++ AnimInstance)"]:::ue
        DataTable["Face/Pose DataTable (ARKit to VRoid Map)"]:::ue
        Avatar["VRoid Avatar (MorphTargets & Head IK)"]:::ue

        UDP -->|Listen| UDPReceiver
        UDPReceiver -->|Parse & Map| UPlugin
        UPlugin -->|Update| VPAnim
        VPAnim -.->|Read Map| DataTable
        VPAnim -->|Apply Transforms| Avatar
    end

    subgraph Broadcasting ["Broadcasting Layer (OBS Studio)"]
        OBS["OBS Studio (Chroma Key & Streaming)"]:::broadcast
        OBS_Controller["obs_controller.py (OBS WebSocket v5)"]:::python
        Platform["송출 채널 (치지직, 유튜브 등)"]:::broadcast

        Launcher -->|2. Control| OBS_Controller
        OBS_Controller -.->|Scene Control, Rec/Stream| OBS
        Avatar -->|Screen Capture| OBS
        OBS -->|RTMP| Platform
    end
"""

# Mermaid.ink API expects base64 encoded string
# A proper URL base64 encoding without padding is required
json_state = f'{{"code":"{code}","mermaid":{{"theme":"default"}}}}'.encode('utf-8')
base64_str = base64.urlsafe_b64encode(code.encode('utf-8')).decode('ascii').rstrip('=')
url = f"https://mermaid.ink/img/{base64_str}"

print(f"Downloading from: {url}")
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
try:
    with urllib.request.urlopen(req) as response:
        with open('C:/UnrealWork/VP/vp_architecture.png', 'wb') as f:
            f.write(response.read())
    print("SUCCESS: C:/UnrealWork/VP/vp_architecture.png")
except Exception as e:
    print(f"FAILED: {e}")
