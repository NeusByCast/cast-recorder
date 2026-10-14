## 1.0

- Borderless 760×560 window with live preview, dropdowns (FPS / Quality / Encoder / GPU / Resolution / Container), and Pause
- Stop and Exit never hang; a stuck Finalize is abandoned safely (prefer leak over UAF)
- Soft start/stop click; zero-copy GPU path when available
