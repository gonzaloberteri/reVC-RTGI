# Comparison captures

Raster-vs-ray-traced A/B captures used by the top-level [README](../../README.md).
Each scene has a matched pair from an identical camera pose:

- `<slug>_raster.png` — RTGI off (`enabled=0`), a 1920×1080 still of
  pixel-identical vanilla reVC.
- `<slug>_rtgi.gif` — RTGI on (`enabled=1`), a short loop. The GIF exists to
  show the *dynamic* behaviour a still can't: traffic sweeping through the
  wet-road reflections, rain, rippling sea, neon bounce on moving cars.

## Regenerating them (deterministic)

[`capture.ps1`](capture.ps1) drives a built `reVC.exe` through a fixed scene
manifest and writes both sides for every scene. The camera pose, hour, weather
and resolution are all pinned in the script, so the capture is repeatable:
**make a rendering change, re-run the script, and compare against the committed
set to see exactly what moved.**

```powershell
# Full sweep (all scenes, both sides). Point -GameDir at your local install.
pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game

# Re-shoot a single scene (e.g. if traffic blocked the camera):
pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game -Scenes rain

# Refresh only the ray-traced GIFs, keeping the raster stills:
pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game -GifOnly

# Static rtgi PNGs instead of GIFs (no ffmpeg/Python needed):
pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game -NoGif
```

Requirements:

- A built `reVC.exe` + Vice City assets (**not** part of this repo — build per
  [RTGI.md](../../RTGI.md) and point `-GameDir` at your install).
- For GIF assembly, `ffmpeg` (winget: `winget install Gyan.FFmpeg`) — the script
  finds it on PATH or in the WinGet install location. If ffmpeg is missing it
  falls back to Python + Pillow ([`assemble_gif.py`](assemble_gif.py)).

Runs launch a small window minimized, so they stay in the background.

## What is and isn't deterministic

Fixed by the manifest: player position, camera aim, time of day, weather,
resolution. With `enabled=0` the renderer is byte-for-byte vanilla, which is
what makes the A/B honest.

**Not** fixed: ambient traffic and pedestrians spawn randomly, so each run
differs in which cars and peds appear — that variety is the point of the GIF
side. If a vehicle parks in front of the camera, just re-run that scene.

## How the GIF capture works

The game's harness dumps a BMP every `shotframes` rendered frames into four
rotating slots (`rtgi_shot_0..3.bmp`, ~15/sec minimized). Two hard-won details:

- **Dumping is armed late.** Dense `glReadPixels` dumps during the volatile
  load/teleport phase livelock the frame loop, so the script boots with
  `shotframes=0` and flips it on via the game's config hot-reload (the harness
  polls `rtgi_config.txt`'s mtime) once the scene has settled.
- **Frames are deduped by content hash.** NTFS refreshes directory timestamps
  lazily for files rewritten in place, so hashing the tail bytes is the only
  reliable new-frame detector when polling the slots.

The script keeps every Nth distinct dump and assembles the sequence with ffmpeg
(`palettegen`/`paletteuse`, per-clip palette, rectangle diff mode) into a
~720px loop.

## Scene manifest

| Slug | Location | Hour | Weather | What it shows |
|---|---|---|---|---|
| `rain` | Ocean Drive `250,-1283,12` facing N | 14 | rainy | Wet asphalt mirrors palms, buildings, passing traffic |
| `night` | Ocean Drive `250,-1283,12` facing W | 2 | sunny | Emissive neon GI washes the street |
| `noon` | Ocean Drive `250,-1283,12` facing N | 12 | sunny | Daylight AO + sky GI |
| `sunset` | Beach `400,-1385,9` facing W | 19 | sunny | Golden-hour sun bounce, beach → skyline |
| `dawn` | Lighthouse channel `430,-1670,10` facing E | 7 | sunny | Sea reflections |

Heading is degrees (0 = north, counter-clockwise, so 90 = west, 270 = east) and
snaps the camera behind the player. Coordinates are proven-good; don't teleport
into water — Tommy can't swim.
