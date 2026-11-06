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
| `overlay/include/ffx2/` | FSR SDK 2.4 API headers (unmodified, MIT), used for both FSR runtimes |
| `fg-gpu1.patch` | The same changes as a diff against `3e237a4`, for review (without the vendored SDK headers and licence text) |
| `build-local.ps1` | Builds and packages the mod on this PC with the installed Visual Studio |
| `tools/presentmon-capture.ps1`, `tools/presentmon-summary.ps1` | Frame-pacing A/B: record a PresentMon capture of the game, and compare captures (on-screen time, jitter, 1% low, frame types, present mode) |
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
2. On GitHub, open **Actions → FO4 FG on GPU1** and use **Run workflow**. It is manual only; pushes do not start a build.
3. When it's green, download the **FO4-FG-GPU1** artifact. It's a zip laid out as a mod (`Data\F4SE\Plugins\AAAFrameGeneration.dll`, …).

## Install

1. In Vortex, install the zip as a mod (or copy its `Data` folder into the game folder).
2. Every setting below is also in the Mod Configuration Menu (Frame Generation page); entries marked "(restart)" apply after a game restart, the rest within about a second. Settings come from `Data\MCM\Config\FrameGeneration\settings.ini`. The shipped file has `bFrameGenOnSecondaryGPU = 1` and `iFrameGenType = 0`. If `Data\MCM\Settings\FrameGeneration.ini` exists (MCM writes it), it **overrides** the shipped file, so set the same keys there.

```ini
[Settings]
bFrameGenerationMode = 1
bFrameGenOnSecondaryGPU = 1
sSecondaryGPUName =          ; optional, e.g. 9060 - only needed if auto-selection refuses
bSendHudlessFrame = 1        ; 1 = extra UI-free copy (clean HUD); 0 = ~25% less PCIe, HUD smears
bAsyncUnpack = 1             ; 1 = 9060 reads on a copy queue, lane by lane (default); 0 = original serial path
bEarlyTransfer = 1           ; 1 = depth/MVs/HUD-less cross during the frame (default); 0 = all at Present
bEarlyUnpack = 1             ; 1 = the 9060 reads them as they land, before Present (default); 0 = reads queued at Present
iHalfResDepthMV = 1          ; 0 = full res; 1 = half-res depth, rebuilt on the 9060 (default); 2 = half-res depth + MVs
iFrameGenType = 0            ; ignored in GPU1 mode (always FSR3)
iFSRRuntime = 1              ; 0 = FidelityFX SDK 1.1.4 (FSR 3.1.4); 1 = FSR SDK 2.4, newest provider offered (default)
iFSRSwapChainProvider = 0    ; SDK 2.4: 0 = newest swap chain (4.0.0 on RX 9000); 1 = newest 3.x (3.1.7), for A/B
iFSRFrameGenProvider = 0     ; SDK 2.4: 0 = newest interpolator (4.0.2 ML on Win11 + RX 9000, else 3.1.6); 1 = 3.x
```

`iFSRRuntime` picks the FSR runtime; see [FSR SDK 2.4](#fsr-sdk-24-ifsrruntime) below. It applies
in both GPU1 and single-GPU FSR3 mode.

`bSendHudlessFrame` is the "extra UI-free frame" switch. On (default) the 3090 sends a second copy
of each frame with the HUD removed; FSR3 uses it so the HUD is not interpolated. Off removes that
copy from the PCIe budget (~25% less traffic at 4K) but the HUD is then interpolated with the scene
and smears a little during camera movement.

`bAsyncUnpack` changes how the 9060 reads what arrives. On (default) it reads on its own copy
queue and starts on each buffer (final frame, HUD-less, depth, motion vectors) as soon as that
buffer has crossed, while the 3090 is still sending the next one; the FSR3 queue only waits for
the read to finish. Off is the original path: wait for all four, then read them on the FSR3 queue.
Use it as an A/B switch; both log the same timing lines.

`bEarlyTransfer` changes *when* the buffers cross. On (default) depth and motion vectors start
crossing as soon as the world pass has finished them, and the HUD-less frame as soon as it has
been captured; only the finished frame waits for Present. The 3090's copy at Present drops to one
lane (~2.6 ms at 4K instead of ~10.3 ms), and the 9060 can start reading the earlier lanes while
the finished frame is still crossing. The copy engine is still serial and shares the PCIe link, so
the gain is bounded by how early in the frame those hooks run. Off is the pre-optimisation
behaviour: all four lanes cross at Present.

`bEarlyUnpack` does the same for the 9060's side. With it off, the 9060's reads were only queued
when Present was called, so lanes that had crossed 3–6 ms earlier sat unread until then. On
(default), each group's read is queued at the same hook as its copy, so the 9060 reads depth,
motion vectors and the HUD-less frame while the game is still finishing the frame, and only the
finished frame is read after Present. Needs `bAsyncUnpack = 1` and `bEarlyTransfer = 1`. It is
re-read every ~60 frames, so it can be flipped while the game runs for an A/B in one session.

`iHalfResDepthMV` sends depth (and optionally motion vectors) at half resolution. Only the
generated frames use them; real frames and the HUD-less colour stay full resolution.
- `1` (default): depth crosses at half resolution (the nearest depth of each 2×2 block) and the
  9060 rebuilds it to full resolution: bilinear on smooth surfaces, the block's own sample at
  object edges, so no blended depth appears on silhouettes. FSR3 keeps working at full resolution
  and still reads every motion vector; only its depth tests are coarser at edges. The rebuild
  starts as soon as the depth lane lands and runs while the other lanes are still crossing.
  Saves 23.7 MB/frame (~1.9 ms) at 4K.
- `2`: motion vectors too (the one of the nearest-depth sample of each block). FSR3 then works
  at half resolution, as it does behind "Performance" upscaling. Saves 47.5 MB/frame (~3.9 ms).
  There is no "motion vectors only" option: FSR3 works at the depth resolution, so half-res
  motion vectors with full-res depth is not a layout it accepts.
- `0`: both at full resolution, as before.

The 3090 shrinks the buffers in the same pass that already fills the shared buffers, so mode 1
and 2 add no pass there; mode 1 adds one ~0.1–0.2 ms compute pass on the 9060, off the critical
path. If anything in the setup fails, the log says so and the plugin uses mode 0.

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
[FG-GPU1] bridge ready: render GPU copy queue -> cross-adapter heap -> display GPU copy queue, lane by lane (bAsyncUnpack=1)
[FG-GPU1] lane[0] 'final frame' 3840x2160 ... timing=present  lane[1] 'depth' ... timing=world  lane[2] 'motion vectors' ... timing=world  lane[3] 'HUD-less colour' ... timing=hudless
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

Three more lines every 10 s show where a frame's time goes:

```
[FG-GPU1] display GPU: unpack (copy queue) avg 10.40 ms max 11.2 ms | direct list avg 1.30 ms max 2.0 ms
[FG-GPU1] timeline ms after Present: 3090 copy 1.2-11.5 | 9060 unpack 4.0-14.1 | 9060 list 14.2-15.5
[FG-GPU1] render thread blocked: FFX Present avg 6.10 ms max 9.0 ms | latency wait avg 0.00 ms max 0.0 ms | D3D11 gate pending 0/480
```

(The numbers above are illustrative, not measured.)

- **unpack:** how long the 9060 needs to read the payload. **direct list:** the 9060's FSR3
  queue work per frame (FSR3's prepare pass; the interpolation itself runs on FFX's own async
  compute queue and is not in this number).
- **timeline:** one frame's path on a single clock (both GPUs are calibrated to the CPU's QPC).
  `3090 copy` starting late means the 3090 was still rendering that frame when the CPU called
  Present. With `bAsyncUnpack = 1` the 9060 unpack should *start* a few ms after the 3090 copy
  starts, not after it ends. With `bEarlyTransfer = 1` the `3090 copy` start is negative: the
  first lanes cross before Present. With `bEarlyUnpack = 1` the `9060 unpack` start is negative
  too; its end is the finished frame's read. Both copy times sum only the phases that ran
  (world, HUD-less, Present), not the idle time between them.
- **render thread blocked:** time the game's render thread spends inside the FFX swap chain's
  Present. That call returns only when the previous frame has been put on screen, so while it
  waits the 3090 receives no new work. A large value here means the 3090 is being held back by
  the transfer → 9060 → present chain, not by its own rendering.
- **D3D11 gate pending:** how often the 3090's next frame had to queue behind a copy that was
  still running. Should be ~0.

With `iHalfResDepthMV = 1` one more line appears:

```
[FG-GPU1] depth rebuild on display GPU avg 0.15 ms max 0.30 ms | done 3.0 ms after Present
```

`done` should be earlier than the `9060 unpack` end in the timeline; then the rebuild costs the
chain nothing.

If anything fails during setup, the plugin logs why and falls back to running FG on the 3090. Failures that happen after the swap chain exists are logged as `CRITICAL`.

## FSR SDK 2.4 (`iFSRRuntime`)

The package ships two FSR runtimes in `Data\F4SE\Plugins\FrameGeneration\FidelityFX\`:

| `iFSRRuntime` | DLLs | what you get |
|---|---|---|
| `0` | `amd_fidelityfx_dx12.dll` (FidelityFX SDK 1.1.4) | FSR 3.1.4: what this mod was developed and measured with |
| `1` (default) | `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_framegeneration_dx12.dll` (FSR SDK 2.4) | the newest frame generation the driver and Windows offer |

With 2.4, AMD's runtime offers different parts depending on the machine. The plugin takes the newest
of each unless you pin one:

| part | Windows 10, RX 9060 XT, Adrenalin 26.10.1 (probed) | Windows 11, RX 9000 |
|---|---|---|
| swap chain (pacing) | **4.0.0**, or 3.1.7 | 4.0.0, or 3.1.7 |
| interpolator | **3.1.6** only (analytical, same kind as 3.1.4) | **4.0.2** (FSR 4 ML), or 3.1.6 |

So on Windows 10 the image-quality upgrade (ML frame generation) is **not available**; AMD's runtime
does not even list it. What 2.4 can still bring there is the 4.0.0 swap chain's
**driver-assisted frame pacing**, where the AMD driver times the presents instead of a user-mode
busy-wait. AMD enables it on its own, with no API flag and no report, when all of these hold:
RX 9000-series GPU, Adrenalin 26.10.1+, swap chain 4.0.0+, fullscreen or a borderless window
covering the display, VSync off, and presenting inside the display's VRR range (AMD says it is tuned
for Windows 11 with hardware-accelerated GPU scheduling). The log reports the two conditions the
plugin can see: whether the window covers the display, and VSync. To see whether pacing improved,
compare frame-time variance (e.g. PresentMon's `msBetweenPresents`) against `iFSRSwapChainProvider = 1`,
which keeps everything else the same but uses the 3.1.7 swap chain and its own pacing.

What the plugin does differently with 2.4 (the 1.1.4 path is untouched):
- adds the version descriptors 2.x needs to enable its newer API;
- dispatches `PrepareV2` with the camera's world position (`posAdjust`) and its up/right/forward
  vectors (FSR 4 ML needs them; FSR 3.1.6 accepts them), the vertical FOV from the projection
  matrix instead of a fixed 1 rad, and `reset` when generation restarts after a menu or loading;
- declares the HUD-less copy's RGBA8 format when the back buffer is something else;
- logs which providers were offered and which are used, where the DLLs were loaded from, FSR's own
  error/warning messages, and failed generated-frame dispatches.

If the 2.4 DLLs are missing or a context cannot be created, the plugin logs why and runs that
session on 1.1.4. The 2.4 loader looks for `amd_fidelityfx_framegeneration_dx12.dll` next to
`Fallout4.exe` *before* its own folder, so a copy left there by another mod (OptiScaler, for
example) wins; the log warns when that happens.

```
[FSR] runtime: FSR SDK 2.4 (Data\F4SE\Plugins\FrameGeneration\FidelityFX\amd_fidelityfx_loader_dx12.dll)
[FSR] swap chain providers offered: 4.0.0, 3.1.7
[FSR] swap chain provider: 4.0.0 (driver-assisted frame pacing capable)
[FSR] frame generation providers offered: 3.1.6
[FSR] FSR 4 (ML) frame generation is not offered on this system. AMD requires Windows 11 and an RX 9000-series GPU for it; ...
[FSR] in use: swap chain 4.0.0 + frame generation 3.1.6
[FSR] window 3840x2160 at (0,0), display 3840x2160 at (0,0): covers the display
[FSR] generated frames in the first 10 s: 690 dispatched, none failed
```

(Expected lines, not a capture; the provider lists are what the Windows 10 probe returned.)

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
| 3840×2160 | 4, `iHalfResDepthMV = 1` | 102.8 MB | ~8.5 ms (estimate) |
| 3840×2160 | 4, `iHalfResDepthMV = 2` | 79.1 MB | ~6.6 ms (estimate) |
| 3840×2160 | 3 (no extra UI frame) | 93.4 MB | ~7.8 ms |
| 2560×1440 | 4 (all) | 56.3 MB | ~4.7 ms |
| 2560×1440 | 3 (no extra UI frame) | 42.2 MB | ~3.5 ms |

The copy runs at ~12 GB/s whichever GPU drives it, so those are the practical numbers. The
display GPU then reads the same payload back out of system memory (~the same time again).

What limits the frame rate is less each step's duration than the chain they form. The FFX swap
chain has two back buffers, so its Present for frame N returns only once frame N-1 is on screen.
Until then the game's render thread, and therefore the 3090, gets no new work. Everything between
"3090 finished frame N-1" and "frame N-1 presented" (copy, read, FSR3, pacing) is on the critical
path. That is why `bAsyncUnpack` streams the buffers lane by lane: the 9060's read overlaps the
3090's copy instead of following it. See SESSION-NOTES.md for the numbers.

## Known risks / first things to check

- **FSR SDK 2.4 has had one in-game run.** At 4K it used swap chain 4.0.0 + FSR 3.1.6, doubled
  64 → 128 fps with no errors and felt smooth, but it has not yet been A/B-tested against 1.1.4 or
  the 3.1.7 swap chain at the same settings. If 2.4 misbehaves (no generated frames,
  `[FSR] generating a frame failed`, stutter, a crash), set `iFSRRuntime = 0` to go back to
  exactly the previous behaviour.

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
