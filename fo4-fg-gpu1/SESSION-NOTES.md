# FO4 FG-GPU1 — session notes, 2026-11-06

Handoff for a fresh session. Read this first; it assumes no memory of the previous conversation.

## Where we are

The prototype **works end to end**. Fallout 4 renders on the RTX 3090, the RX 9060 XT runs FSR3
frame generation and presents to the monitor, and the two are bridged over PCIe with cross-adapter
heaps and fences.

Measured result at **2560×1440**: **70.9 base fps → 141.8 presented** (50% generated), exactly the
71/142 goal, with the extra UI-free frame enabled (no HUD smearing).

At **3840×2160** the same build gives **~48 base → ~96 presented**. The 9060 has become the
bottleneck there: it reads the 126.6 MB payload and then runs FSR3, serially, on one queue. Reducing
3090 graphics settings no longer helps — the 3090 is no longer the limit.

The agreed next optimisation (deliberately postponed, see "Next to-dos"): move the 9060-side unpack
to a **copy queue** so it overlaps with FSR3 of the previous frame. Expected to lift 4K to ~71/142
without changing quality.

## Goal

- 71 base fps on the render GPU, doubled to ~142 on the display GPU (144 Hz panel).
- Prefer image quality: keep the extra HUD-less frame on. The no-HUD-less mode saves ~25% PCIe but
  the user finds the HUD smearing unacceptable at 4K.
- 4K output is the ideal; 1440p output is the working fallback.

## Hardware and topology

| | |
|---|---|
| Render GPU | NVIDIA RTX 3090, **no display attached**, PCIe 3.0 x16 |
| Display / FG GPU | AMD RX 9060 XT, drives the only monitor (AOC 32M2V, 3840×2160 @ 144 Hz) |
| CPU / platform | Intel i9-7940X, X299 — PCIe 3.0 throughout |
| Measured transport | ~12 GB/s in both directions (~10.1–10.6 ms for 126.6 MB) |
| OS | Windows 10 19045, Visual Studio 18.11 / MSVC 14.52, CMake 4.3.1 |

Fallout 4 is forced to the 3090 with the Windows "High performance" GPU preference
(`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`), but **the game ignores it** — it enumerates
adapters and takes the display GPU. The plugin therefore creates the game's D3D11 device on the
headless GPU itself (see `PickRenderAdapter`).

## Architecture as built

Two D3D12 devices:

- **interop device** on the render GPU: opens the game's D3D11 shared textures and the producer
  fence; owns the transfer copy queue.
- **FG device** on the display GPU: owns the FSR3 context, the swap chain, the present, and the
  direct queue that unpacks and generates.

Per frame, four lanes are copied (each double-buffered, two slots):

| lane | 4K size | 1440p size |
|---|---|---|
| final frame (with HUD, unpacked into the back buffer) | 33.18 MB | 14.75 MB |
| HUD-less colour (extra UI-free frame, optional) | 33.18 MB | 14.75 MB |
| depth (R32) | 33.18 MB | 14.75 MB |
| motion vectors (RG16) | 33.18 MB | 14.75 MB |
| **total** | **126.6 MB** | **56.2 MB** |

Synchronisation chain: D3D11 renders → signals producer fence → render-GPU copy queue waits and
copies into the cross-adapter heap → signals transfer fence → display-GPU direct queue waits and
unpacks → FSR3 → present → signals consumed fence → render GPU may reuse the slot. The render GPU
only ever waits for the copy of the frame *before* the one it is working on.

## Measured results (all this session)

| Resolution | Lanes | Payload | Copy | Base | Presented | Generated |
|---|---|---|---|---|---|---|
| 2560×1440 | 4 (extra frame on) | 56.2 MB | 4.53 ms | 70.2–70.9 | 140.4–141.8 | 50% |
| 3840×2160 | 4 (extra frame on) | 126.6 MB | 10.06–10.33 ms | 44.1–48.1 | 88.1–96.2 | 50% |
| 3840×2160 | 3 (extra frame off) | 94.9 MB | 7.55–7.74 ms | 40.8–44.2 | 81.5–88.4 | 50% |

`max lag` stayed at 2 and `cpu waits` at 0 in every run. Representative log:

```
[FG-GPU1] presented 141.8 fps | game 70.9 fps | generated 50%          (1440p)
[FG-GPU1] 70.9 game fps | 56.2 MB/frame = 3.90 GB/s | copy 4.53 ms (~12.13 GB/s link) | max lag 2 | cpu waits 0

[FG-GPU1] presented 96.2 fps | game 48.1 fps | generated 50%           (4K)
[FG-GPU1] 48.1 game fps | 126.6 MB/frame = 5.95 GB/s | copy 10.25 ms (~12.06 GB/s link) | max lag 2 | cpu waits 0
```

### Why 4K is capped at ~48

The 9060 does two things per input frame, serialised on its direct queue:

```
per-frame cost ≈ payload / 12 GB/s  +  FSR3 (~9.4–10.5 ms)
```

- 1440p: 4.7 + 9.4 = 14.1 ms → 70.9 fps ✔ (matches the log)
- 4K: 10.5 + 10.5 = 21.0 ms → 47.6 fps ✔ (matches the log)

That is why reducing 3090 settings from base 46.6 to 48.1 barely moved it: the 3090 stopped being
the limit and the 9060 took over. The copy itself is only ~47% duty at 4K, and the render thread
never blocks (`cpu waits 0`).

## What changed this session

Relative to the original Claude prototype (`fo4-fg-gpu1/` at the start of the session):

### Tooling
- `build-local.ps1`:
  - **Fixed the crash that stopped the first run**: `git rev-parse HEAD` writes to stderr in a
    fresh repo, and Windows PowerShell 5.1 turns that into a terminating `NativeCommandError`
    under `$ErrorActionPreference = Stop` (even with `2>$null`). Now uses
    `git rev-parse --verify --quiet HEAD`.
  - **Self-relaunches under PowerShell 7** when started from 5.1 (`pwsh` on PATH), preserving
    parameters and exit code. If `pwsh` is missing it warns and continues on 5.1.
  - `$ProgressPreference = 'SilentlyContinue'` (the 250 MB FidelityFX download is much faster).
  - VS lookup checks that `vswhere` returned a path before calling `.Split()`.
- Workflow moved to `.github/workflows/fo4-fg-gpu1.yml` (the project copy was removed).
- `CLAUDE.md` added with the "use pwsh" convention.
- `README.md` updated: build on this PC, role selection, `bSendHudlessFrame`, performance budget.
- `fg-gpu1.patch` regenerated from the built tree; `git apply --check` passes against a pristine
  `3e237a4` checkout.

### Plugin
- `Upscaling.cpp`:
  - Fixed the compile error: `dx12SwapChain` was declared inside the resource loop but used after
    it; hoisted to function scope.
  - `bSendHudlessFrame` (renamed from `bTransportHudLess`; the old key is still read as a fallback):
    when off, the HUD-less lane is not registered, `HUDLessBufferShared12` is nulled (FSR3 then
    uses the back buffer and smears the UI), and `PostDisplay` skips the now-unused 4K D3D11 copy.
  - Logs the setting and the lane total.
- `CrossAdapterBridge.h/.cpp`:
  - `OwnsMonitor` now requires `AttachedToDesktop` and breaks on enumeration errors.
  - `HasAttachedOutput` + a one-shot per-adapter output dump (`game window monitor`, `output[i]
    "\\.\DISPLAY1" attached=… monitor=…`, `"<gpu>" outputs: N`).
  - `SelectSecondaryAdapter` picks: name filter → adapter that owns the window monitor → adapter
    with an attached display → only other hardware adapter. The `[display]` marker is new.
  - `PickRenderAdapter`: if the game picked the display GPU and exactly one hardware adapter has
    no display attached, render on that one. Guarded so it cannot invert roles if detection fails.
- `DX11Hooks.cpp`: uses `PickRenderAdapter` to choose the adapter passed to `D3D11CreateDevice`
  (logs `the game picked the display GPU; rendering on "…" instead`).
- `DX12SwapChain.h/.cpp`: every 10 s logs
  `presented X fps | game Y fps | generated Z%` using `GetLastPresentCount()` deltas.
- `settings.ini`: `bFrameGenOnSecondaryGPU = 1`, `sSecondaryGPUName =`,
  **`bSendHudlessFrame = 1`**.

### Current machine state (important)

- The **installed** game DLL is the older build (14:51, still uses `bTransportHudLess`). The zip in
  `D:\fgb\FO4-FG-GPU1.zip` is newer and contains the rename plus the same logic; install it before
  the next test to get `bSendHudlessFrame`.
- The game currently runs **4K, fullscreen**, `bTransportHudLess = 1`; MCM override has
  `bFrameLimitMode=0`, `bDisableInMenus=0`, `bEnableDebugLogging=1`, `iFrameGenFrames=0`.
- The repo is still uncommitted: `fo4-fg-gpu1/`, `.github/`, `CLAUDE.md`, `research/` are untracked.

## Build and install

Build (2 min incremental; ~7 min from a cold vcpkg cache):

```powershell
pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-dlss\fo4-fg-gpu1\build-local.ps1
```

- Work tree: `D:\fgb\FO4Upscaling` (pinned `3e237a44269a8dc8cab54aab4ef0aae83c284a43`).
- vcpkg: `D:\fgb\vcpkg` at `b472291f295551b7127359ea38fdd2ca092f6f1b`.
- Output: `D:\fgb\package\Data\...`; installable zip `D:\fgb\FO4-FG-GPU1.zip`.
- `-Clean` forces a CMake rebuild.
- Build is clean: 0 compiler warnings under `/W4 /WX`; DLL exports
  `F4SEPlugin_Load/Query/Version`.

Install: Vortex → install the zip over the existing mod. Game folder:
`C:\SteamLibrary\steamapps\common\Fallout 4`. Log:
`C:\Users\PA\Documents\My Games\Fallout4\F4SE\AAAFrameGeneration.log` (truncated per run).

## Test protocol

1. Install the current `D:\fgb\FO4-FG-GPU1.zip`.
2. Monitor must be on the **9060**; keep ReShade **off** (`dxgi.dll` renamed) for now — AutoHDR
   changes the back-buffer format and produces the known blue/purple garbage.
3. Launch through F4SE, windowed/borderless, play 30–40 s.
4. Send the `[FG-GPU1]` lines. The useful ones: `game window monitor`, adapter lines with
   `[display]`, `SELECTED`, `the game picked the display GPU`, `swap chain created`,
   lane lines, and the 10 s `game fps` / `presented` pairs.

Quick A/B to prove the 3090 is not the limit: set `bFrameGenerationMode = 0` and
`bFrameGenOnSecondaryGPU = 0`, run at 4K. Base should return to ~71 (the pre-mod figure).

## Next to-dos, in order

### 1. Overlap the 9060's unpack with FSR3 (the main one, postponed on purpose)

Idea: the 9060 currently reads the staging buffers into its mirrors on the **direct queue**, then
FSR3 runs on the same queue — 21 ms serialised at 4K. Move the unpack to a **copy queue on the FG
device** and fence it; the copy for frame N then runs while FSR3 computes frame N-1.

Expected: per-frame cost becomes `max(read ~10.5 ms, FSR3 ~10.5 ms)` plus sync ≈ 11–13 ms, so the
9060 allows ~80–90 fps and the 3090 becomes the limit again. At the user's reduced 4K settings that
should be **71 base / 142 presented with the extra UI-free frame on**.

Sketch:
- Add to `CrossAdapterBridge`: `copyQueueB`, per-slot copy allocators, one copy command list,
  `unpackFenceB` + per-slot values.
- Replace `RecordUnpack`/`RecordUnpackTo` with `BeginUnpack(slot, finalLane, backBuffer)`: record
  barriers + copies on the copy list, execute on `copyQueueB`, signal `unpackFenceB`.
- In `DX12SwapChain::Present`, call `BeginUnpack` and then `commandQueue->Wait(unpackFenceB, value)`
  before the FSR3 list is executed.
- Move `SignalConsumed` to `copyQueueB` (the staging slot is free once the copy has read it).
- Before resetting a slot's copy allocator, CPU-wait on its previous `unpackFenceB` value (same
  pattern as the existing `WaitCpu(copyDoneFenceA, previous)`).
- Watch: cross-queue resource states on the mirrors and the back buffer, the swapchain present
  ordering, `max lag`, `cpu waits`, and whether DMA + compute actually overlap on the 9060.
- Keep a setting to fall back to the serialised path if it regresses.

### 2. Upscaling on the 3090 (alternative route to 4K/142)

Render 1440p (or 1080p) and upscale to 4K with FO4Upscaling's `Upscaling.dll` (DLSS/FSR), then FG
on the 9060. Payload becomes 4K final frame + render-res depth/MV/HUD (~58–77 MB, 5–6.4 ms) and
FSR3 runs its optical flow at render resolution.

Work: make the FG plugin and the upscaler share the same D3D11 device and agree on
`kMain`/`kFrameBuffer` sizes; verify the lane factory handles mixed resolutions (it already sizes
each lane independently). Do **not** use doodlum's `Upscaling.dll` — take the matching one from the
same `3e237a4` tree. Watch for Community Shaders conflicts.

### 3. Polish / risks

- **Runtime transport failure has no recovery.** `CrossAdapterBridge::Fail` sets `active = false`
  but `secondaryGPU` stays true, so `Present` skips both the bridge and the single-GPU path: the
  screen freezes and only a restart recovers. At minimum document it; optionally attempt a degraded
  present.
- Expose `bSendHudlessFrame` (and the GPU1 mode) in `package/MCM/Config/FrameGeneration/config.json`
  so it is discoverable in MCM instead of hand-editing the ini.
- Commit and push the repo (workflow included). The push will trigger the GitHub build.
- Consider measuring the 9060's FSR3 cost directly (timestamps around the direct-queue work) to
  validate the overlap estimate before building it.
- Optional: log the 9060's unpack time separately, so "read vs FG" is not inferred.

## Gotchas

- **`powershell` is always Windows PowerShell 5.1; `pwsh` is 7.** `build-local.ps1` re-launches
  itself under `pwsh`, but ad-hoc commands should use `pwsh`.
- **FO4 ignores the Windows GPU preference.** The plugin forces the D3D11 device onto the headless
  GPU. If that log line disappears, the game is rendering on the 9060 and the pipeline is inverted.
- **Monitor detection**: `DXGI_OUTPUT_DESC.Monitor == HMONITOR` produced false positives in the game
  process (once marking the headless 3090 as the display GPU). Current code uses attached-output
  counting and still logs the raw output table for diagnosis. If roles ever flip, read those lines.
- **MCM override wins**: `Data\MCM\Settings\FrameGeneration.ini` overrides the shipped
  `Data\MCM\Config\FrameGeneration\settings.ini` per key. A missing key falls through to the shipped
  file. MCM may rewrite its override on exit, so prefer editing the shipped file for one-off tests.
- **`iFrameGenFrames` is irrelevant to FSR3** (2× fixed); it only applies to DLSS-G MFG.
- The log is truncated every launch, so grab it after the run you care about.
- Reinstalling the mod replaces the shipped `settings.ini`; re-apply manual edits.

## File map

| Path | What |
|---|---|
| `fo4-fg-gpu1/overlay/` | Modified/new upstream files copied over the pinned checkout |
| `fo4-fg-gpu1/build-local.ps1` | Local build + package script (self-relaunches under pwsh 7) |
| `fo4-fg-gpu1/fg-gpu1.patch` | Full diff against `3e237a4`, applies cleanly |
| `fo4-fg-gpu1/README.md` | User-facing docs, performance budget, settings |
| `fo4-fg-gpu1/SESSION-NOTES.md` | This file |
| `.github/workflows/fo4-fg-gpu1.yml` | GitHub Actions build (not yet committed) |
| `CLAUDE.md` | Agent note: use `pwsh` |
| `D:\fgb\package` | Built mod tree |
| `D:\fgb\FO4-FG-GPU1.zip` | Installable zip (newest build) |
| `D:\fgb\FO4Upscaling` | Pinned upstream checkout with the overlay applied |
| `C:\Users\PA\Documents\My Games\Fallout4\F4SE\AAAFrameGeneration.log` | Plugin log |

## Key log line reference

```
[FG-GPU1] game window monitor 0x10001 "\\.\DISPLAY1"
[FG-GPU1] adapter[0] "AMD Radeon RX 9060 XT" ... [game window's monitor] [display]
[FG-GPU1]   output[0] "\\.\DISPLAY1" attached=1 monitor=0x10001
[FG-GPU1] adapter[1] "NVIDIA GeForce RTX 3090" ... [game]
[FG-GPU1]   "NVIDIA GeForce RTX 3090" outputs: 0
[FG-GPU1] the game picked the display GPU; rendering on "NVIDIA GeForce RTX 3090" instead
[FG-GPU1] SELECTED "AMD Radeon RX 9060 XT" (owns the monitor the game window is on)
[FG-GPU1] interop device on "NVIDIA...", frame generation + present on "AMD..."
[FG-GPU1] lane[N] '<name>' 3840x2160 ... (bytes, running total)
[FG-GPU1] <fps> game fps | <MB>/frame = <GB/s> | copy on render GPU avg <ms> max <ms> | max lag N | cpu waits N
[FG-GPU1] presented <fps> | game <fps> | generated <percent>
```
