# Cast Recorder

Lightweight Windows screen recorder: GPU capture, hardware encode, global hotkeys.

- Live preview, Pause, NVENC / AMF / QuickSync / Software
- 15 / 30 / 60 FPS, quality presets, native / 1080p / 720p, MP4 (MKV/MOV with ffmpeg)
- Press-to-set global hotkeys that work while games have focus
- Dark borderless settings UI; saves to `Videos\Cast Recorder`

**Download:** [latest release](https://github.com/NeusByCast/cast-recorder/releases/latest)

### Build

```
cmake -S . -B build -A x64 && cmake --build build --config Release
ctest --test-dir build -C Release
iscc /DAppVersion=<ver> /DBinDir=..\build\Release installer\CastRecorder.iss
```

Version lives in `src/win/version.h`. Known limits: primary monitor only, no audio.

## Diagnostics

Everything (settings load/save, startup encoder warm-up/probe cache, every recording's encoder/path/audio
stats, the press-to-first-frame latency breakdown, crashes) goes to one file:
`%APPDATA%\Cast Recorder\CastRecorder.log` — click **Log** in the title bar to open it (the path is also copied).
The file is trimmed to the newest ~1 MB once it passes 2 MB.
