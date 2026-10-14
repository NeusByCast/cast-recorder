## 1.0.1

- Hardened ffmpeg lookup and DLL loading (no cwd hijack; CFG)
- Validated CLI (`--help`, exit code 2 on bad args); recordings match the requested duration
- Stricter settings JSON; remux cleanup and safer uninstaller
- Nbc QA edge-case suite in CI
