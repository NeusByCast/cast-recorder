# Cast Recorder v1.1: UI spec

Built on v2 (`../v2/`, left unchanged). **This file replaces v2 where they differ.** All px are logical at 96 DPI, multiplied by `dpi/96` (Per-Monitor V2, `WM_DPICHANGED`). Tokens are in `tokens.json`: themes `dark` and `light`, plus `accents` and `motion`.
Source offers Full screen, each monitor, and windows.

Mockups (2200×1400, 2x): `mockup-main.png` (idle), `mockup-recording.png`, `mockup-replay-toast.png`, `mockup-finished-toast.png`, `mockup-light.png`, `mockup-settings.png`, `mockup-recordings.png`.

## Window and chrome
- Default size **1100 × 700**, minimum **960 × 620**. Borderless, with **rounded corners everywhere**: window 12, cards 12, controls and buttons 8, icon buttons and the Record button 12, toasts 14, pills fully round.
- Title bar h40 holds the app icon, "Cast Recorder", the tabs, and **Minimize / Maximize-Restore / Close** (46×40 each; return `HTMAXBUTTON` for Snap).
- Tabs, each h26 r7: **Record · Recordings · Settings**. The active tab has a `surface2` background.

## Record view
| Area | Rect | Content |
|---|---|---|
| Toolbar | y56, h36 | **Source** dropdown (x20, w360; Full screen, Monitor N, divider, search, windows, same as v2). FPS (w104) and Quality (w110) on the right |
| Preview | 20,104 · 1060×496, r12 | Letterboxed 16:9 with "Live preview" and resolution/fps pills. Recording adds a 2 px accent ring and turns the chip dot red |
| Bottom bar | y616, h64 | Detailed below |

**Bottom bar** (centre line y648, nothing wraps):
- **x20, status:** Idle shows a green dot and "Ready". Recording shows a pulsing accent dot, the timer `00:01:24` (Semibold) and the file size `48 MB` (muted, updated every second).
- **x170, replay pill** (138×30): `replay` icon, "Replay · 30s", and a green dot that pulses while the buffer is live (`replayPillDot`). The pill only shows when the replay buffer is on; clicking it saves a replay.
- **x420 / x570, audio chips** (136×40): Mic and System, each with an icon and meter. Clicking the icon mutes; clicking the chip body opens the v2 mic popover.
- **x904, Pause** (44, disabled when idle). (v1.1 removed the Screenshot button and its hotkey.)
- **x958, Record** (122×44, accent fill, white dot). While recording it becomes **Stop**: `surface2` fill, 2 px accent outline, accent `stop` glyph, soft glow halo.

## Toasts
Each toast is its **own top-level layered window** (`WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED`). Call **`SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`** on it (Win10 2004+) so it never shows up in a recording or replay. Position: in-app toasts sit 16 px inside the top-right of the preview; on-screen toasts (`WS_EX_TRANSPARENT`, click-through) go to the top-right of the **primary** monitor's work area. Toasts stack downward with a 8 px gap. Hover pauses auto-dismiss (in-app only). If capture exclusion fails while a capture is live, the on-screen toast is skipped.
On-screen status toasts — "Last N seconds saved", "Recording started", "Recording stopped" — hold **1 s**, then glide out (260 ms, +28 px x, fade).

| Toast | Size | Content | Auto-dismiss |
|---|---|---|---|
| Replay saved | 284×64 | 32 px accent-tinted circle with the `replay` icon; **"Last N seconds saved"** (Semibold 13; N = the saved length, e.g. "Last 30 seconds saved", "Last 2 minutes saved"); file name (muted 12) | **1 s**, then glide out |
| Recording finished | 320×116, **green** 1 px border at `#22C55E` 55% | Green `check-circle`; "Recording saved"; `name · 128.4 MB` (size in MB to one decimal, or GB above 1024 MB); buttons **Open** (`play`, 84) and **Show in folder** (`folder`, 148, `SHOpenFolderAndSelectItems`) | 6 s in-app; 1.5 s on screen |

Recordings are written once, straight to a **fragmented MP4** (crash-safe: a crash, kill or stuck Finalize still leaves a playable file), so the finished toast appears as soon as the file is saved. If the container is MKV (and Auto-remux is off), the MKV is a background stream copy made afterwards; the toast's buttons switch to the `.mkv` once it is done. A saved file is never deleted by a timeout.

## Instant replay
- Settings → **Instant replay** card: **Replay buffer** toggle (default off) and **Length** dropdown: 15 s, 30 s (default), 1 min, 2 min, 5 min, plus **10 min / 15 min only when they fit in the memory cap** at the current quality, and **Custom…**. Custom swaps the dropdown for a seconds field (whole seconds, 5–600, Enter commits).
- Memory: the buffer is capped at **min(25 % of RAM, 2 GB)**. Below the Length field a caption shows "Uses about N MB of RAM", or, when the cap is hit, the warning (amber) "RAM cap 2.0 GB: keeps 4 min 10 s". When the cap is reached the oldest seconds are dropped; memory never grows past it.
- While the buffer is on, the encoder runs continuously into a ring buffer of encoded packets in RAM, trimmed to start at a keyframe (1 s keyframe interval; the save starts at the keyframe nearest to "N seconds ago", so the length is within ±0.5 s of N). The **Save replay** hotkey (or a click on the pill) muxes the last N seconds to `replay_HH-MM-SS.mp4` and shows the replay toast. This also works while a normal recording is running.
- The buffer-active pill is shown on the Record view (see the bottom bar).

## Recordings tab
- Toolbar (y56): search field (w320, `search` icon), then on the right **Auto-remux** label + toggle (the same setting as in Settings), and a **Remux all** button (w132, `remux` icon) that converts every MKV.
- List card 20,104 · 1060×496, r12. Column headers (11 Semibold, muted) at y130, laid out from the right edge R = W − 20 so the actions never overlap the badge: actions [R − 150, R], FORMAT R − 200, DATE − 150, SIZE − 90, DURATION − 90 (at 1100 px: NAME x40, DURATION x560, SIZE x650, DATE x740, FORMAT x880). The name takes the remaining width and uses a middle ellipsis.
- Rows h56, 62 px apart. Each row has:
  - a 64×36 thumbnail (r6; first frame, generated on save; replays get a `replay` overlay),
  - the name (13/500) and a detail line (12, muted),
  - duration, size and date,
  - a format badge (44×22 pill: MP4 green 16%, MKV amber 16%),
  - row actions: `play` and `folder` icons. MKV rows show a **Remux** button instead (84×32). While remuxing it shows a progress bar inside the button.
- Remux uses a stream copy (no re-encode), so it is fast. Row hover: `hover` bg, r10, 90 ms. Double-click opens the file. Right-click menu: Open, Show in folder, Remux, Rename, Delete (to Recycle Bin, with confirmation).
- The list comes from scanning the output folder (`.mp4`, `.mkv`), sorted newest first. A `ReadDirectoryChangesW` watcher refreshes it.

## Settings view (3 columns, 20 px gaps)
Column width = clamp((W − 80) / 3, 280, 340): 340 at the default 1100 px, 280 at the 960 px minimum; wider windows centre the three columns. Rows are laid out relative to the column (label at +18, control at +118 with width cw − 136, toggles right-aligned), so nothing runs past the card edge at any width or DPI.

**Long values:** every single-line text that can be user- or system-supplied (device and GPU names, window titles, hotkeys, paths, file names, status) is fitted with an ellipsis instead of being clipped; paths and file names use a middle ellipsis so the end (folder / date / size) stays visible. Hovering any truncated text in the main window shows the full string in a tooltip.
| Card | Rect | Controls |
|---|---|---|
| Video | 20,60 · 340×268 | **Encoder**: lists only what the selected GPU supports (see below). **GPU**. **Resolution** (Native/1440p/1080p/720p). **Container** (MKV default, MP4). **Auto-remux to MP4** toggle |
| Instant replay | 20,340 · 340×122 | Replay buffer toggle, Length dropdown (with Custom) |
| Appearance | 20,474 · 340×126 | **Theme** segmented control (Dark `moon` / Light `sun`; follows the system on first run). **Accent**: 6 swatches (r8, 32 px apart); the selected one gets a 2 px ring at r12 |
| Audio | 380,60 · 340×540 | System audio (toggle + device + meter) · divider · Microphone (toggle + device + meter) · Volume slider · Noise suppression · Noise gate slider · Monitoring · Push-to-talk (key field + toggle) · Channels Mono/Stereo · divider · **Separate audio tracks** toggle, with the caption "Track 1 mix · 2 system · 3 mic" |
| Hotkeys | 740,60 · 340×260 | Press-to-set fields (x+132, w cw − 150) for **Start/Stop** (Ctrl+Shift+F9), **Pause** (F10), **Mic mute** (F11), **Save replay** (Ctrl+Shift+F8). Fields are 50 px apart; a combo too wide for the field drops the spaces around "+" and then ellipsizes, and a field that is capturing gets an accent border and shows "Press a key…". A conflict shows a red border and the tooltip "Already used by …" |
| Output | 740,332 · 340×268 | Folder path (middle ellipsis) + Browse / Log, **File name** pattern (`cast_date_time`, …) |

**Encoder filtering:** at startup and whenever the GPU changes, probe each codec × vendor pair: NVENC (`NvEncGetEncodeGUIDs`), AMF and QSV (Media Foundation `MFTEnumEx` hardware encoders for H.264/HEVC/AV1), plus x264 in software. Only codecs that are present are listed: **H.264**, **HEVC**, **AV1**, each written as `codec · vendor`. Unsupported ones are left out. The default is **Auto**, which picks the best supported codec in the order AV1 > HEVC > H.264 for MKV, or H.264 for maximum compatibility in MP4.
- AV1 can't go into MP4 with every player, so warn when the user picks it: a muted caption under Container saying "AV1 plays best in MKV".

**Separate audio tracks:** when this is on, the file gets three audio tracks: 1 = mix, 2 = system, 3 = mic. Each track is named in the container metadata. Requires MKV or MP4; both support multiple tracks.

## Themes and accent
- Two complete palettes are in `tokens.json → themes`. Borders are `borderBase` drawn at an alpha (0.08 / 0.12 / 0.2), which keeps the same draw code working for both themes.
- In light theme the preview background is `#E3E6EB`, while the captured image stays as it is. The app icon tile stays dark in both themes (brand).
- The accent recolours: the Record button, toggles (on), slider fill, focus ring, selected list row (14% alpha), capturing hotkey field, recording ring and Stop outline. Success and warn colours never change.
- Switching theme or accent crossfades the whole client area over 200 ms: snapshot the old frame to a bitmap, then draw the new frame with the snapshot fading out on top. Settings save right away.

## Micro-animations (Win32-implementable)
| Interaction | Spec |
|---|---|
| Hover (buttons, rows, chips, tabs) | 90 ms ease-out overlay 0 → 4% white (dark theme) or 4% black (light) |
| Press | 70 ms: scale 1 → 0.97 about the centre, overlay 8%. Release: 160 ms overshoot `cubic-bezier(.34,1.56,.64,1)` back to 1 |
| Record press | as above, plus the glow halo blooms (alpha .25 → .45, 160 ms) |
| Toggle | knob slides 140 ms; track colour crossfades |
| Dropdown / popover | same as v2: 160 ms fade + 6 px slide |
| Toast | in: 220 ms ease-out-expo, translateY −8 → 0 + fade. Out: 160 ms fade |
| Replay pill dot | 1.6 s alpha loop |
| Recording dot | 1.2 s pulse ring |

Implementation: Direct2D + DirectComposition (`IDCompositionAnimation` for opacity, scale and translate), or a 16 ms timer and redraw with a float `t` per element. Scaling a press only needs a D2D transform; no HWND resize happens. Respect `SPI_GETCLIENTAREAANIMATION`: when it's off, every change is instant.

## Icons (`res/icons/`, 26 glyphs)
24 grid, round caps and joins, white SVG. `scripts/gen_icons.py` pre-renders every glyph at each device size the UI draws (16/20/24 px × 100–250 % DPI: 16, 20, 24, 25, 28, 30, 32, 35, 40, 48, 50, 60) as PNGs that are converted to premultiplied-alpha (PARGB) bitmaps on load, with the stroke tuned per device size (max(1.5, 0.075·px) rounded to ¼ px) so weight looks the same at every scale. At draw time the exact-size bitmap is placed on whole device pixels (no resampling); any other size uses the next larger bitmap with high-quality bicubic. v1.1 removed `screenshot`.

## States
- Idle, Recording, Paused (amber status, Pause becomes resume).
- Replay saving (the pill shows a spinner for up to 300 ms).
- Remuxing (row progress bar).
- Error: red status text plus a toast with Open log.
- Window source closed: recording stops and the status reads "Window closed".
