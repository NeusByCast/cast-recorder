First release of Cast Recorder, a lightweight Windows screen recorder.

- GPU capture (Windows Graphics Capture, with DXGI Desktop Duplication as the fallback)
- Hardware H.264 via NVENC / AMF / QuickSync (Media Foundation), with a software fallback
- 15/30/60 FPS, quality presets, MP4 (MKV/MOV when ffmpeg is present)
- Press-to-set global hotkeys (any combo) with conflict warnings
- Dark borderless settings window, tray icon, Start menu entry "Cast Recorder"

Download `CastRecorder-Setup-*.exe` and run it. Windows SmartScreen may warn because the installer isn't code-signed: choose "More info" and then "Run anyway".
