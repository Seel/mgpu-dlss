# FO4 FG-GPU1: frame generation on the second GPU

Fallout 4 keeps rendering on the **RTX 3090**. FSR3 frame generation and the final present run on the **RX 9060 XT**, which is the card the monitor is plugged into. Nothing is copied back to the 3090.

This is a modified `AAAFrameGeneration.dll` from [northaxosky/FO4Upscaling](https://github.com/northaxosky/FO4Upscaling) (pinned at `3e237a4`). The upstream plugin creates its D3D12 device on the game's GPU. This version creates two devices:

| | GPU | Does |
|---|---|---|
| render GPU | 3090 (not the monitor) | The game renders here. A **copy queue** copies each frame into cross-adapter heaps. |
| display GPU | 9060 XT (monitor) | Unpacks the copies, runs FSR3 FG, owns the swap chain and presents. |

The roles are picked automatically, not from a setting. The display GPU is the hardware adapter with
a display attached (the 9060). Fallout 4 is a 2015 app: it enumerates adapters and takes the display
one, and the Windows "High performance" GPU preference does not change that. So when the game starts
on the display GPU, the plugin creates its D3D11 device on the headless GPU (the 3090) instead and
logs `the game picked the display GPU; rendering on "NVIDIA GeForce RTX 3090" instead`.

Each game frame crosses PCIe once, in one direction. The payload is the finished frame, the HUD-less frame, depth and motion vectors (about 4 B/px each; ~133 MB at 4K if the motion-vector target is 4 B/px). Without this mod, Windows already copies the finished frame from the 3090 to the 9060 XT so you can see it. The new copies are HUD-less, depth and motion vectors.

Synchronisation never makes the 3090 wait for the copy of the frame it just finished. Its next frame waits only for the copy of the frame *before* that.

## Files

| Path | What |
|---|---|
| `overlay/` | Full copies of the changed/new upstream files. The build copies them over the pinned checkout. |
| `fg-gpu1.patch` | The same changes as a diff against `3e237a4`, for review |
| `build-local.ps1` | Builds and packages the mod on this PC with the installed Visual Studio |
| `SESSION-NOTES.md` | Session handoff: measurements, findings, next steps |
| `../.github/workflows/fo4-fg-gpu1.yml` | GitHub Actions build (seen from this folder) |

The changes:
- `CrossAdapterBridge.h/.cpp` are new: adapter choice, cross-adapter heaps and fences, the copy queue, unpacking, stats.
- `DX11Hooks.cpp`, `DX12SwapChain.*`, `Upscaling.*` and `settings.ini` are small edits.

## Build on this PC

Visual Studio 2026 (or 2022) with the C++ workload and Git are enough; the script loads the
developer environment itself. Run it from an ordinary PowerShell window:

```powershell
pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-dlss\fo4-fg-gpu1\build-local.ps1
```

`pwsh` is PowerShell 7. Windows PowerShell 5.1 (`powershell.exe`) still works — the script detects
it and re-runs itself under `pwsh` when that is installed — but 7 is what it is developed and tested
on. `powershell.exe` is permanently 5.1; installing 7 does not change what that name means.

It clones FO4Upscaling at the pinned commit into `D:\fgb`, applies `overlay/`, builds
`AAAFrameGeneration.dll` in Release (`/W4 /WX`) and writes a ready-to-install folder to
`D:\fgb\package`, including the FidelityFX runtime. First run takes 20–40 min because vcpkg
compiles the dependencies once; later runs take a few minutes. `-Clean` forces a rebuild.

Verified on this machine with VS 18.11 / MSVC 14.52 / CMake 4.3.1: configure, build and
package complete with 0 warnings, and the resulting DLL exports the three F4SE entry points.

## Build on GitHub

1. Commit and push; `.github/workflows/fo4-fg-gpu1.yml` is already in place.
2. On GitHub, open **Actions → FO4 FG on GPU1**. It runs on push, or use **Run workflow**.
3. When it's green, download the **FO4-FG-GPU1** artifact. It's a zip laid out as a mod (`Data\F4SE\Plugins\AAAFrameGeneration.dll`, …).

## Install

1. In Vortex, install the zip as a mod (or copy its `Data` folder into the game folder).
2. Settings come from `Data\MCM\Config\FrameGeneration\settings.ini`. The shipped file has `bFrameGenOnSecondaryGPU = 1` and `iFrameGenType = 0`. If `Data\MCM\Settings\FrameGeneration.ini` exists (MCM writes it), it **overrides** the shipped file, so set the same keys there.

```ini
[Settings]
bFrameGenerationMode = 1
bFrameGenOnSecondaryGPU = 1
sSecondaryGPUName =          ; optional, e.g. 9060 - only needed if auto-selection refuses
bSendHudlessFrame = 1        ; 1 = extra UI-free copy (clean HUD); 0 = ~25% less PCIe, HUD smears
iFrameGenType = 0            ; ignored in GPU1 mode (always FSR3)
```

`bSendHudlessFrame` is the "extra UI-free frame" switch. On (default) the 3090 sends a second copy
of each frame with the HUD removed; FSR3 uses it so the HUD is not interpolated. Off removes that
copy from the PCIe budget (~25% less traffic at 4K) but the HUD is then interpolated with the scene
and smears a little during camera movement.

3. **First test with the fewest moving parts:**
   - Disable ReShade's `AutoHDR.addon64`. An HDR swap chain changes the back-buffer format, and the plugin then refuses to copy (it logs this).
   - Ideally run the first test without ReShade at all: rename `dxgi.dll` temporarily.
   - ENB is off on your install (`d3d11.dll` is absent), which is required. GPU1 mode doesn't support ENB yet.
4. Launch through F4SE. Borderless/windowed (the plugin only engages for windowed swap chains, as upstream). The monitor must be connected to the 9060 XT; the plugin logs which GPU it renders on and which one presents.

## What to look for

The log is usually `Documents\My Games\Fallout4\F4SE\AAAFrameGeneration.log`. All new lines start with `[FG-GPU1]`:

```
[FG-GPU1] game window monitor 0x10001 "\\.\DISPLAY1"
[FG-GPU1] adapter[0] "NVIDIA GeForce RTX 3090" ... [game]
[FG-GPU1]   "NVIDIA GeForce RTX 3090" outputs: 0
[FG-GPU1] adapter[1] "AMD Radeon RX 9060 XT" ... [game window's monitor] [display]
[FG-GPU1]   output[0] "\\.\DISPLAY1" attached=1 monitor=0x10001
[FG-GPU1] SELECTED "AMD Radeon RX 9060 XT" (owns the monitor the game window is on)
[FG-GPU1] bridge ready: render GPU copy queue -> cross-adapter heap -> secondary GPU
[FG-GPU1] lane[0] 'final frame' 3840x2160 ...  lane[1] 'HUD-less colour' ...  lane[2] 'depth' ...  lane[3] 'motion vectors' ...
[FG-GPU1] 38.1 game fps | 126.6 MB/frame = 4.70 GB/s | copy on render GPU avg 10.10 ms max 11.67 ms (~12.24 GB/s link) | submit 0.086 ms | max lag 2 | cpu waits 0
[FG-GPU1] presented 74.8 fps | game 38.1 fps | generated 49%
```

The stats lines repeat every 10 s — the bridge's line and the presented/game line have separate
timers, so they appear next to each other. Watch these numbers:
- **copy … ms:** how long the render GPU's copy engine needs per frame. It must stay well under one frame time. It is ~10.5 ms for 126.6 MB (~12 GB/s) whichever GPU runs the copy queue — that is the cross-adapter heap path's ceiling, not one GPU's PCIe link.
- **max lag:** should be 1–2.
- **cpu waits:** should stay at or near 0.
- **presented vs game fps:** `presented` should be close to double `game` (generated ≈ 50%). If `generated` is ~0%, FSR3 accepted the frame but did not insert one — the display GPU is not keeping up.
- **lane count:** with `bSendHudlessFrame = 0` the extra UI-free copy is skipped: three lanes instead of four, 93.4 MB at 4K or 42.2 MB at 1440p.

If anything fails during setup, the plugin logs why and falls back to running FG on the 3090. Failures that happen after the swap chain exists are logged as `CRITICAL`.

## Frame generation is not upscaling

GPU1 mode renders the game at whatever resolution the game asks for, on the 3090, and doubles the
presented frames on the 9060. It does **not** upscale. If the game renders 4K natively, the 3090's
frame rate is the ceiling: 38 game fps becomes ~75 presented fps, not 100+. To raise the base frame
rate, add an upscaler (FO4Upscaling's own `Upscaling.dll`, or lower the game's resolution) so the
3090 renders fewer pixels; FSR3 FG then interpolates the upscaled output.

## Performance budget

The payload and copy time scale with resolution and with the number of lanes (one lane per buffer,
two slots each):

| target | lanes | payload | copy |
|---|---|---|---|
| 3840×2160 | 4 (all) | 126.6 MB | ~10.5 ms |
| 3840×2160 | 3 (no extra UI frame) | 93.4 MB | ~7.8 ms |
| 2560×1440 | 4 (all) | 56.3 MB | ~4.7 ms |
| 2560×1440 | 3 (no extra UI frame) | 42.2 MB | ~3.5 ms |

The copy runs at ~12 GB/s whichever GPU drives it, so those are the practical numbers. For a
71 fps base (14.1 ms per frame) the display GPU also has to read the payload and run FSR3 inside
that frame: 4K does not fit (~10 ms read + ~8 ms FG), 1440p does (~5 ms + ~5 ms). That is why
2560×1440 is the first thing to try for a 71 base / ~142 presented target; dropping the extra
UI-free frame adds headroom on top.

## Known risks / first things to check

- **Cross-adapter heap 3090 → 9060 XT.** MGPU Bridge proved AMD → NVIDIA. This direction should work the same way, and if it fails the code tries the reverse automatically. If both fail, the log shows the HRESULT and frame generation stays on the 3090.
- **A failure after startup is not recovered.** Once the bridge is up, the first failed HRESULT — including the 2 s timeout waiting for the previous frame's copy if the secondary GPU's queue stalls — disables the transport for the rest of the session. `secondaryGPU` stays selected, so the game keeps running but no new frame reaches the screen (`CRITICAL [FG-GPU1] transport is down`). Restart the game; there is no automatic single-GPU fallback after this point.
- **Format mismatch freezes the picture instead of falling back.** If the swap chain format changes under the plugin (HDR/AutoHDR), the finished-frame copy is skipped and FSR3 presents stale data. That is why the first test should run without ReShade/AutoHDR.
- **Doodlum's `Upscaling.dll` together with FO4Upscaling's FG plugin.** They hook some of the same render functions. If the image or motion is wrong, try FO4Upscaling's own `Upscaling.dll` (same source tree) as a matched pair.
- **FO4 Community Shaders** says doodlum's `Upscaling.dll` below 1.2 is incompatible and disables all of CS while it's present. That's independent of this mod, but worth knowing.
- **PCIe budget at 4K.** Start at 2560×1440 if the copy time is too high. The log prints the exact payload per lane; colour, HUD-less colour and depth are 4 B/px, but if the game's motion-vector target is RGBA16F (8 B/px) the total is ~166 MB/frame at 4K rather than the 133 MB quoted above. Trimming the depth/motion copies to the render resolution is the next optimisation (not done yet).
- **DLSS-G / XeSS runtime DLLs are not in this package.** GPU1 mode forces FSR3 and never loads Streamline or XeSS, so this is fine. Switching `bFrameGenOnSecondaryGPU = 0` and `iFrameGenType = 1/2` would need those DLLs from the upstream package.
- **Frame cap.** `bFrameLimitMode = 1` caps the base frame rate to about half the refresh rate (≈72 fps at 144 Hz) when VSync is off. That's upstream behaviour.

## Licence

Upstream FO4Upscaling is GPL-3.0 with the exceptions in its `EXCEPTIONS.md`. The README there says MIT, but `LICENSE`/`COPYING` are GPL-3.0. The files under `overlay/` and the patch are derived from it and carry the same licence.
