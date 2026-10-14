# Cast Recorder

Lightweight Windows screen recorder: GPU capture, hardware encode, global hotkeys.

- GPU capture (Windows Graphics Capture, DXGI fallback) and NVENC / AMF / QuickSync encode
- 15 / 30 / 60 FPS, quality presets, MP4 (MKV/MOV with ffmpeg)
- Press-to-set global hotkeys that work while games have focus
- Dark tray settings UI; saves to `Videos\Cast Recorder`
- Start menu installer; single static exe, no Electron

**Download:** [latest release](https://github.com/NeusByCast/cast-recorder/releases/latest)

### Build

```
cmake -S . -B build -A x64 && cmake --build build --config Release
ctest --test-dir build -C Release
iscc /DAppVersion=<ver> /DBinDir=..\build\Release installer\CastRecorder.iss
```

Version lives in `src/win/version.h` (1.0, 1.1, …). Unit tests also build on Linux. Known limits: primary monitor only, no audio, MKV/MOV need ffmpeg on PATH.
