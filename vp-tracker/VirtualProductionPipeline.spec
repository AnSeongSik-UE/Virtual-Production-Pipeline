from pathlib import Path

from PyInstaller.utils.hooks import collect_all


project_dir = Path(SPECPATH)
mediapipe_datas, mediapipe_binaries, mediapipe_hiddenimports = collect_all("mediapipe")
camera_datas, camera_binaries, camera_hiddenimports = collect_all("cv2_enumerate_cameras")
model_datas = [
    (str(path), "models")
    for path in sorted((project_dir / "models").glob("*.task"))
]

a = Analysis(
    [str(project_dir / "app.py")],
    pathex=[str(project_dir)],
    binaries=mediapipe_binaries + camera_binaries,
    datas=mediapipe_datas + camera_datas + model_datas,
    hiddenimports=mediapipe_hiddenimports + camera_hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["tkinter"],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="VirtualProductionPipeline",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=True,
    version=str(project_dir / "version_info.txt"),
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name="VirtualProductionPipeline",
)
