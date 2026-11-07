# FO4 FG-GPU1 — session notes, 2026-11-06

Handoff for a fresh session. Read this first; it assumes no memory of the previous conversation.

**Next session: start with [plans/UI-TILES.md](plans/UI-TILES.md)** (send only the finished frame's
UI tiles in FSR 4.1 mode, new setting `bSendUITilesOnly`), then measure; only then
[plans/DISPLAY-THREAD.md](plans/DISPLAY-THREAD.md) (FFX Present on its own thread, `bDisplayThread`).
Both aim at the last 3-4 fps to the 71 cap with Community Shaders on (66.6-68.5 now with
`bExtraFrameInFlight = 1`). The user asked for both as optional settings.

## Latest (2026-11-07, 16:1x): bExtraFrameInFlight measured: +4–5 base fps with CS

Logs saved in the session scratchpad (`runs/MGPUScaling-2slots.log`, `-3slots.log`). CS on
(telemetry too), same build. Clean 10 s gameplay windows:

| | 2 frames in flight (16:08) | 3 frames in flight (16:12–16:13) |
|---|---|---|
| game / presented fps | 62.9 / 125.8 | 66.6–68.5 / 133–137 |
| 3090 idle between frames | ~3.2 ms (window had a loading outlier, removed by hand) | 2.2–2.6 ms |
| 3090 first command → frame end | ~12.6 ms | 12.0–12.8 ms |
| FFX Present block | 6.9 ms | 3.6–5.6 ms |
| timeline after Present | copy 5.8–12.2, list end 15.8 | copy 3.1–5.9 → 9.3–12.3, list end 12.9–15.9 |

User: "a noticeable improvement... I think". The 3090 still idles ~2.4 ms per frame. The rest of the
block is FFX itself (`FrameInterpolationSwapchainDX12.cpp:2188`): Present first waits on
`compositionFenceCPU` for every frame already sent, i.e. until the presenter thread composited the
previous *real* frame, which it does only after pacing out the generated one (`:744–795`). That is
independent of the buffer count (the buffer wait at the end of Present is what 3 buffers removed).
So the remaining coupling is the chain length: 3090 done ~+5 → copies until ~+11.4 (the 33 MB
finished frame is last) → 9060 read done ~+13.9 → composite/FG prepare ~+15 → interpolation → paced
generated frame → real-frame composite. Options: (A) shorten it — send only the UI tiles of the
finished frame in FSR 4.1 mode (it is only needed for the UI; ~5 ms of copy + read at the tail);
(B) decouple — run the display GPU side (unpack, composite, FFX configure/prepare, FFX Present) on
its own thread so the game thread never waits in FFX Present (bridge B-side state then needs
locking; per-frame camera/jitter captured on the game thread). Timer now leaves out samples over
100 ms (loading) and says how many.

## Earlier (2026-11-07, 16:0x): 3090 idle timer + bExtraFrameInFlight (built 16:01)

**Question:** 61–63 base with CS on (71 without CS, capped; 71 with CS and no MGPU, no FG). From the
15:37 log: copy 5.95 ms on the 3090's copy engine, `D3D11 gate pending 0/633`, `cpu waits 0` (the
copy never stalls the 3090); timeline `3090 copy 5.0-11.3 | 9060 unpack 6.2-13.8 | 9060 list
14.1-14.9` vs 5.3–8.2 ms list end without CS (20:32 run); `FFX Present` block 5.9–6.6 ms avg vs
0.2–3.1 without CS. With 2 FFX buffers, Present(N) waits for N-1 on screen, so the cycle is roughly
(chain latency + CPU time) / 2; CS makes the 3090 finish ~5 ms after Present and the whole chain
moves out. Not yet known whether the 3090 is saturated or idling, hence:
- **`RenderGpuTimer`** (DX12SwapChain.h/.cpp): D3D11 timestamps (+ disjoint) on the game's context
  at the top of Present (frame end), at the last line of Present (first command of the next frame,
  executed only when the GPU gets that work) and at the next frame's end; ring of 8, read with
  DONOTFLUSH. New 10 s line: `[FG-GPU1] render GPU: idle between frames avg … | frame from its first
  command to its end avg …`. Idle includes frame-limiter sleep (at the cap it is headroom).
- **`bExtraFrameInFlight`** ([FrameGeneration], MCM Second GPU page, restart, second-GPU mode only):
  `FrameSlots.h` `kMaxFrameSlots = 3`; `DX12SwapChain::frameSlots` set in DX11Hooks before the
  devices (3 with the option); FFX `BufferCount`, command lists, D3D11 shared back-buffer copies,
  FG lanes (`HUDLessBufferShared*`, depth, MVs, UIColorAlpha), bridge (`Initialize(..., a_slots)`,
  `SlotCount()`, `AddLane` takes an array, heap size/mirrors/depth rebuild per slot) and
  `SecondaryUpscale` (`Slots()`, outputs/descriptors) all follow it. Arrays sized 3, only the used
  slots created. XeSS/DLSS-G keep 2.

**CS resolution question (answered from source, not measured):** CS sizes passes as its own
full-size texture × the engine's dynamic-resolution ratio (`GetDynamicWidthRatio`); telemetry
`working=3840x2160` is only the allocation. Our upscaler sets the ratio to 0.667 for the frame and
forces 1.0 only around the call at `DrawWorld::DeferredComposite+0x915` (with 1440p proxy targets,
so that pass runs at 1440p). CS's dispatches run before it (pre-composite at the function entry, SSS
at the t15 bind `+0x812`), so they should already be at 1440p — same as FO4Upscaling on one GPU.
User-side check: toggle SSGI/SSS live and watch base fps.

**Next test:** CS on, same spot. (1) `bExtraFrameInFlight = 0`: read the `render GPU: idle` line.
(2) `= 1`, restart: base fps, idle line, `FFX Present` block, feel of input latency. Expect
`[FG-GPU1] 3 frames in flight (bExtraFrameInFlight) ...` at startup; the lane lines show a larger
total only through the heaps (they print per-frame bytes, unchanged).

## Earlier (2026-11-07): MGPU Scaling = one DLL; HDR/AutoHDR works in game; CS crash solved

**Verified in game (15:19, the 15:15 build, AutoHDR on, Community Shaders off, doodlum's leftover
`Upscaling.dll` removed):** user: "works perfectly". Log: `[FSR] the swap chain was created as
format 28 but is format 24`, `[FG] back buffers: format 24`, `[FG-GPU1] ... R10G10B10A2_UNORM
(HDR10) ... every frame is converted`, `[FSR4] ... composited in R10G10B10A2_UNORM (HDR10)`; menus
706 frames native (Copy pass), then gameplay 708–711 upscaled / 0 native per 10 s, FSR 4.1.2 avg
3.9–4.0 ms, UI composite + copy 0.52–0.58 ms (was 0.40–0.51 at 8-bit), **70.8–71.0 / 141.6–142.0,
50% generated**, copy 5.92 ms, no errors. Settings migration ran on the first start (15:11, log
overwritten since): `MGPUScaling.ini` holds the 9 frame-generation and 3 upscaling keys from the old
files, no BOM. User confirms AutoHDR cannot be toggled without restarting ReShade/the game anyway, so
no runtime format check is needed.

**CS crash solved (15:2x A/B by the user): `frame_annotations = true` crashes after loading a save;
telemetry alone works** (15:28 run: CS on, telemetry on, annotations off, 59.9/107 fps with CS's
features; CS's telemetry lines are in its log). Cause, proven with a standalone test
(`scratchpad/detourtest`, the plugin's own `include/detours/Release/detours.lib`): CS's annotations
detour (Microsoft Detours, at CS's Load) `DrawWorld::Forward` and `DrawWorld::Imagespace` among
others; without annotations CS does not touch those entries (15:28 CS log has only the "Forward
cloud group call" call-site hook). FO4Upscaling's `stl::detour_thunk` (Nukem's
`Detours::X64::DetourFunction`) then detours the same two entries — frame generation's
`DrawWorld_Forward` (kPostPostLoad), the upscaler's `DrawWorld_Imagespace` (kPostLoad; Upscaling.dll's
Load before the merge, also after CS) — and **Nukem copies the entry's `jmp rel32` into its
trampoline without relocating it**: the test's trampoline jump landed at `...C70008` instead of
`...C71000` and crashed (0xC0000095). These run only in world frames, hence "after loading a save".
Not caused by the merge or AutoHDR; the old two-DLL build had it too. Fix (built 15:34, not yet run):
`overlay/include/PCH.h` (upstream file, now in the overlay) — `stl::detour_thunk` checks the entry
first (`detail::HookedEntryTarget`: `E9 rel32` or `FF 25 [rip+disp32]`) and, if another plugin already
hooked it, writes `jmp` to its thunk through CommonLib's trampoline (`write_jmp<5>`) and uses the old
jump's destination as `func`, so the other hook still runs and then the game's function; otherwise
Nukem as before. Logged: `[HOOK] ... was already hooked by another plugin ...; chained behind it`.
Standalone test of the chaining: both jump forms OK, unhooked entries untouched. **Verified in game
(15:37, CS on, `frame_annotations = true`): no crash**; log has `[HOOK] struct up::DrawWorld_Imagespace
... chained behind it` and `[HOOK] struct DrawWorld_Forward ... chained behind it`, 61–63 / 122–126,
50% generated (CS features + annotations on the 3090). The check runs once, when the hooks are
installed at startup; CS's annotation setting also needs a restart, so nothing has to be re-checked.

**Renamed and merged.** The FG-GPU1 `AAAFrameGeneration.dll` and its matching `Upscaling.dll` are
now one F4SE plugin, **MGPU Scaling**, `AAAMGPUScaling.dll` (target `MGPUScaling`,
`overlay/plugins/MGPUScaling`). The frame-generation sources stay in
`overlay/plugins/FrameGeneration/src`; FO4Upscaling's Upscaling plugin moved to
`overlay/plugins/MGPUScaling/src/upscaler` in `namespace up` (its `Upscaling`, `FidelityFX`,
`Streamline`, `Buffer.h`, `DX11Hooks` clash by name, and `Buffer.h` differs, so without the namespace
inline members would silently merge). Package: `D:\fgb\MGPU-Scaling.zip` — `Data\F4SE\Plugins\AAAMGPUScaling.dll`,
`...\MGPUScaling\Shaders\*.hlsl`, `...\MGPUScaling\FidelityFX\*`, `MCM\Config\MGPUScaling\` (one page
set, `[FrameGeneration]` / `[Upscaling]` sections), log `MGPUScaling.log`. It shares no file with
doodlum's Upscaling or FO4Upscaling, so those mods can simply be disabled. Details:
- Entry `src/XSEPlugin.cpp` keeps the old hook order: frame generation's D3D11 IAT hook at Load
  (must be innermost — it creates the device and does not chain — hence still "AAA"), the
  upscaler's IAT + render hooks at **kPostLoad** (Upscaling.dll installed them in its own Load,
  i.e. after Community Shaders'), frame generation's render hooks at kPostPostLoad, upscaler data
  at kGameDataReady. Trampoline 2 KiB (1 + 1).
- The cross-DLL handshake is gone: `FGGPU1_SecondaryUpscalerReady` / `FGGPU1_SetSecondaryUpscale`
  are plain functions the upscaler binds directly; `SecondaryUpscale::PlanUpscale` checks
  `mgpu::UpscalerActive()` instead of `GetModuleHandle("Upscaling.dll")` + version export.
- Conflicts: `AAAFrameGeneration.dll` loaded → CRITICAL + message box, plugin returns false;
  another `Upscaling.dll` → upscaler (and FSR 4.1 on the 9060) off, frame generation runs.
- Settings migration (`mgpu::MigrateSettings`): first start without `MCM\Settings\MGPUScaling.ini`
  copies the known keys from the old `FrameGeneration.ini` / `Upscaling.ini` `[Settings]` into it
  (no BOM), logs `[INIT] carried N settings over`. The user's Upscaling.ini also holds doodlum's
  keys (DLSSNR, iFrameGenerationMode, ...); only ours are taken.
- `build-local.ps1` now resets the pinned checkout's source folders before applying the overlay,
  builds only `MGPUScaling`, packages and zips. CI workflow (display name "MGPU Scaling", artifact
  `MGPU-Scaling`) builds the same target. `fg-gpu1.patch` regenerated (`git diff -C
  --find-copies-harder`, so the upscaler shows as copies of `plugins/Upscaling/src`).
- Build: 0 warnings (`/W4 /WX`); exports only the F4SE entry points plus the static FSR 3.1 SDK's
  (Upscaling.dll exported those too). No inline FFX helper is defined in both FFX header sets.
- Repo folder is still `fo4-fg-gpu1` and log prefixes still say `[FG-GPU1]`/`[FSR4]` (not renamed).

**HDR / ReShade AutoHDR** (DeepSeek's `research/RESHADE-AFTER-FG-REPORT.md`, Option A, checked
against the FFX 2.4 source: FFX builds its replacement back buffers from the *real* chain's
`GetBuffer(0)->GetDesc()`, so with AutoHDR they are R10G10B10A2 while the mod copied RGBA8 into them;
the old RGBA8 guard in `SecondaryUpscale::Initialize` was handed the game's format, so it never fired;
`RecordLaneCopy` compared only size/pitch, which 8- and 10-bit share). ReShade itself was already
after FSR 4.1 and FG (it hooks the FFX chain on the 9060). Changes:
- `FidelityFX::CreateContexts` reads `GetDesc1` after the swap-chain context exists →
  `displayFormat`; FG context `backBufferFormat` = it, HDR flag off (SDR in a 10/16-bit container),
  HUD-less still declared RGBA8 (SDK 2.4 only; 1.1.4 logs an error).
- `SecondaryUpscale`: composite in `displayFormat` (typed-UAV-store support checked); native frames
  go through new `Copy` entry (`SecondaryUpscaleCS.hlsl`) + CopyResource; with FSR 4.1 off it runs as
  "conversion only" (final lane gets a mirror, `OwnsPresent()`/`Converting()`). RGBA8 chain: no change.
  FSR 4.1 outputs stay RGBA8.
- `DX12SwapChain`: `displayFormat` from `swapChainBuffers[0]->GetDesc()`; `SecondaryUpscale::Initialize`
  moved from `CreateSwapChainFSR3` to `CreateSwapChain`, after `GetBuffer`, so it gets that format;
  `DXGISwapChainProxy::GetDesc` reports the game's RGBA8 (matches `GetBuffer`).
- `CrossAdapterBridge::RecordLaneCopy`: copy-family check.
- fxc: Copy/Composite/Blit compile at 1440p and 4K render sizes.

**First in-game run (15:12): black screen, with and without CS.** Two causes in `MGPUScaling.log`:
(1) `CRITICAL [INIT] another Upscaling.dll is loaded` — doodlum's Upscaling 1.1.1 (`Upscaling.dll`
2025-12-26, link count 1, i.e. not a Vortex hardlink, so disabling the mod did not remove it, plus
`Upscaling\*.hlsl`) is still in `Data\F4SE\Plugins`; the upscaler was off, so no FSR 4.1. (2) No
`[FSR] the swap chain was created as format 28 but is format 24` line although AutoHDR made it 24:
**ReShade's `DXGISwapChain::GetDesc1` reports the app's requested format when an add-on modified the
descriptor** (`_is_desc_modified`, `reshade-src/source/dxgi/dxgi_swapchain.cpp:491`), and FFX's
`GetDesc1` forwards to it. So no conversion was set up, and the new copy-family check refused every
finished-frame copy (`lane 'final frame' is ... format 28 but its destination is ... format 24;
skipping the copy`) → black. Fix (built 15:15, verified at 15:19, see above): `FidelityFX::CreateContexts` and
`DX12SwapChain::CreateSwapChain` read the format from the back buffer resource (FFX's replacement
buffers are created from the real buffer's desc); the GetDesc1-based runtime check was removed (it
could never fire under ReShade).

**Community Shaders "emit telemetry" crash after loading a save** — first triage (superseded by the
A/B above: it was `frame_annotations`, not telemetry or AutoHDR).
No crash dump or WER event exists (Windows logged game starts at 14:27, 14:28, 14:31). The 14:31
run *also* had AutoHDR loaded (re-enabled since the 22:28 test: `ReShade.ini` 14:17, not in
`DisabledAddons`; ReShade log: chain → R10G10B10A2 at creation), and FSR 4.1 + FG only start on the
first gameplay frame (Upscaling log 14:31:35.3 first `OverrideRenderTargets`), i.e. exactly when the
8→10-bit copies began. AAAFrameGeneration.log (flushes every line) stops at 14:31:22 with 24–39 ms
`FFX Present` blocks, like DeepSeek's AutoHDR run. CS telemetry emits its first line 5 s after the
first post-composite tick, and the CS log has none. Telemetry also forces CS's GPU profiler on
(`KeepActive`, D3D11 timestamp queries). `frame_annotations = true` is on too. Most likely AutoHDR;
**A/B needed**: (1) old build or new build, AutoHDR in `DisabledAddons`, telemetry on → load save;
(2) the new build with AutoHDR on.

**Next test (new build):** disable the FG-GPU1 mod (and doodlum's Upscaling) in Vortex, install
`D:\fgb\MGPU-Scaling.zip`. Check `MGPUScaling.log`: `[INIT] carried N settings over`, `[INIT]
Installing upscaling hooks`, `[UPSCALE] FSR 4.1 ... handled by the frame-generation part`, the usual
`[FSR4]`/`[FG-GPU1]` lines, 71/142. Then AutoHDR on: expect `[FSR] the swap chain was created as
format 28 but is format 24`, `[FG-GPU1] the swap chain is R10G10B10A2_UNORM (HDR10) ... converted`,
`[FSR4] ... composited in R10G10B10A2_UNORM`, all frames upscaled, ~50% generated, HDR picture.

## Latest: FSR 4.1 upscaling on the 9060 (first run 20:32 works; FOV fix built 20:40, not yet run)

**First run (20:32, 4K, Quality → 2560×1440, user's 110° FOV):** worked first try. User: image
quality "quite good, maybe slightly worse than DLSS"; some grain on thin tree branches (also seen
with DLSS); "a little hitching here or there", not attributable yet. Log, steady windows:

| | value |
|---|---|
| base / presented | 70.0–71.0 / 140.0–142.0, 50% generated (dips 64.9/68.2 = pause menu and a heavier area) |
| frames upscaled | 650–710 per 10 s, 0 at full resolution, 0–1 history resets |
| FSR 4.1.2 on the 9060 | avg 4.0–4.3 ms, max 4.3–7.2 ms (probe alone: 3.3 ms; here it shares the GPU with FG and the copy engine, and `bEnableDebugLogging = 1` turned on FSR's API checks) |
| UI composite + copy | avg 0.40–0.51 ms |
| payload / 3090 copy | 73.8 MB, avg 5.9 ms (was 102.8 MB / 8.45 ms) |
| 9060 list end after Present | 5.3–8.2 ms (10.5 in a 3090-bound area) |
| FFX Present block | avg 0.2–3.1 ms, **max 8–13 ms in most windows** (the 17:48 native-4K run had max 4.1–5.6) |

The occasional 8–13 ms render-thread block is the candidate for the hitches; 10 s aggregates cannot
tell whether it is this pipeline or FO4 (streaming, HighFPSPhysicsFix). Next: PresentMon A/B at the
same spot, `iUpscaleMethodPreference = 3` vs `0` (`tools/presentmon-capture.ps1`), with
`bEnableDebugLogging = 0`.

**Bug found and fixed (20:40): the camera FOV given to FSR was ~178°.** The first upscale logged
`fovY 3.115`: `camViewData.projMat[1][1]` reads ~0.013 in this game, not 1/tan(fovY/2) (~1.25 for
110° horizontal at 16:9). The same value went to FSR 3.1.6 frame generation's `PrepareV2` since the
SDK 2.4 work. `FidelityFX::VerticalFov` now rebuilds the projection as `inverse(view) *
viewProjUnjittered` (the DLSS-G path's method), accepts it only if `P[0][0] * aspect == P[1][1]`
(±3%), tries the other multiplication order next, else uses 1 rad (what FSR 3.1 always got). It logs
the source once: `[FSR] camera vertical FOV ... from ...` — check that line next run (expect ~1.35 rad
/ ~78° at 110° FOV). Used by both FSR 4.1 and FG.

**Second run (20:46, the 20:40 build):** user: trails reduced, "barely noticeable". But the log
(1.2 MB) shows the FOV source flipping every frame: at `PostDisplay` (FSR 4.1) the renderer's
camera state holds a 2D camera (P[1][1] 0.0131, rejected → 1 rad), at Present (FG) a 24° camera
(P[1][1] 4.7046, `projMat` and `inverse(view) * viewProj` agree, so `projMat` is a real projection;
the problem is *which* camera is current). Neither is the world camera (~78°). Fixed in the next
commit: sample in the world pass, latch, log changes sparingly.

**FOV latch (built 20:51, not yet run):** `FidelityFX::SampleWorldCameraFov` reads `projMat` in the
world-pass hooks (`PostAlpha`, `CopyBuffersToSharedResources`), accepts it only if `P[0][0] * aspect
== P[1][1]` (±3%), and latches it; `VerticalFov()` returns the latch (1 rad until the first sample)
for FSR 4.1 (`BeginFrame`) and FG (`PrepareV2`). Logged: first sample, each change > 1° (aiming,
1st/3rd person), the first rejected sample, at most 40 lines, each with `PlayerCamera worldFOV` /
`firstPersonFOV` for comparison. The user's INI has 110 and 98 (1st / 3rd person, per the user):
at 16:9 that is ~78.4° and ~65.8° vertical, if the setting is horizontal. If the world-pass sample
shows something else (e.g. the 24° camera), the world camera is not current there either; next
candidates: `UpdateDynamicResolution` (Upscaling.dll's hook at the start of the world render) or
deriving it from `PlayerCamera`.

**Third run (20:54, 3rd then 1st person):** everything else fine — 98-line log, every gameplay
frame upscaled (FSR 4.2 ms), 70.9/141.8 steady, no errors. But the world-pass sample **after**
the call `DrawWorld_Reticle` wraps also saw the 2D camera (P[1][1] 0.0131, rejected), so the FOV
stayed at the 1 rad default all run. `PlayerCamera` read `worldFOV 98, firstPersonFOV 98` in 3rd
person. **Change (built 20:58, not yet run):** sample in `PreAlpha` instead (just before that call);
the log line now also prints what `worldFOV` gives as vertical FOV if it is horizontal at the display
aspect and if at 4:3, which settles the convention. Rejections are logged once per hook. Not merged
to `main` yet: the user asked to merge once the logs check out.

**Fourth run (20:59):** `PreAlpha` also sees the 2D camera (P[1][1] 0.0131); the current camera
state is never the world camera at our hooks. `worldFOV 98` → 65.8° (horizontal at 16:9) or 81.6°
(at 4:3); the web has no authoritative answer on FO4's convention. **Change (built 21:02, not yet
run):** read `State::cameraDataCache` (every camera of the frame) and take the widest entry with
`useJitter` and a perspective projection for the display aspect: the world camera is the one drawn
with the upscaler's jitter. The cache is listed in the log once (each entry: jitter flag, P[1][1],
FOV), and the chosen FOV is printed next to both `worldFOV` conversions.

**Fifth run (21:04) — FOV fixed, merged to `main`.** The cache had 5 cameras: a jittered and an
unjittered 24° pair (a UI/3D-model camera), one jittered non-perspective one, and the world camera
as a jittered/unjittered pair at **65.8°**, exactly `worldFOV 98` taken as horizontal at the display
aspect. Switching to 1st person: **77.6°** = `worldFOV 110` the same way. So FO4's FOV setting is
horizontal at the display aspect (not 4:3). Single transitional readings (75.5°, 85.6°) during the
view switch. The latch went to 24° only at the very end (quitting; no further stats lines): with no
world camera in the list the 24° camera is the widest left. Those frames are not upscaled and get no
FG, and it switches back when the world is drawn again. Everything else unchanged: 70.4–70.7 / 141,
FSR 4.1–4.2 ms, 111-line log, no errors.

Goal: free the 3090 for heavier graphics mods. With `Upscaling.dll`'s
`iUpscaleMethodPreference = 3`, the 3090 renders the 3D scene at the quality mode's resolution
(Quality = 2560×1440 at 4K) and the 9060 upscales it with **FSR 4.1.2** (SDK 2.4, ML, works on
Windows 10) before frame generation. The UI is still drawn at 4K on the 3090. Package:
`D:\fgb\FO4-FG-GPU1-fsr41.zip` (ships `AAAFrameGeneration.dll` **and** the matching `Upscaling.dll`).
README section "FSR 4.1 upscaling on the second GPU" has the user-facing description.

Background: DeepSeek's `research/FSR41-UPSCALE-GPU1-REPORT.md` (Option A) and its probe
(`D:\fsr-probe\upscale\`: 4.1.2 offered on the 9060 under Win10, 3.3 ms 1440p→4K, 317.5 MB). One
correction to that report and to what this session first said: the "HUD-less buffer sized from
`kMain` but copied from `kFrameBuffer`" mismatch is **not** a real bug with FO4Upscaling's
`Upscaling.dll`: it keeps every game render target at full size and renders into the top-left
render-resolution corner (proxies are swapped in only around a few passes), so `kMain` stays 4K.

How it works (code):
- **`Upscaling.dll` (overlay `plugins/Upscaling/`)**: new `UpscaleMethod::kSecondaryGPU` (= 3). Same
  render scale / jitter / TAA-off / mip bias / dynamic-resolution fixes as FSR/DLSS, but `Upscale()`
  returns early. Each frame `GetUpscaleMethod` asks `FGGPU1_SecondaryUpscalerReady(w, h)` and falls
  back to native (`kDisabled`) when false; `UpdateUpscaling` calls
  `FGGPU1_SetSecondaryUpscale(active, w, h, jitterX, jitterY, fSharpness)`. Exports
  `FO4Upscaling_SecondaryGPUApiVersion` (= 1). `LoadSettings` now reads `MCM\Config` defaults then
  `MCM\Settings` (upstream read only the latter); the shipped default is 3.
- **FG plugin, startup** (`SecondaryUpscale::Initialize`, from `CreateSwapChainFSR3`): reads the same
  two `Upscaling.ini` files; needs method 3, the FG-GPU1 `Upscaling.dll` export, bridge +
  `bAsyncUnpack`, RGBA8 back buffer, SDK 2.4 and a **4.x** upscaler provider (pinned explicitly).
  Render size = `(uint)(display * (1 / ratio))`, the same float arithmetic as `Upscaling.dll`.
  `FidelityFX::CreateUpscaler`: flags `NON_LINEAR_COLORSPACE | AUTO_EXPOSURE` (+`DEBUG_CHECKING` when
  `bEnableDebugLogging`), messages to the log. Sets `Planned()`; the lanes are then sized for it.
- **Lanes** (`Upscaling::CreateFrameGenerationResources`): scene (the HUD-less lane), depth, MVs at
  render size; `iHalfResDepthMV` forced 0, HUD-less forced on; the **final frame lane gets a mirror**
  instead of being unpacked into the back buffer (`CreateInterop`). `PostAlpha` /
  `CopyBuffersToSharedResources` copy only the render-size corner.
- **3090 at `PostDisplay`** (end of imagespace, either hook order works since `Upscale()` is a no-op):
  copy the corner into the scene lane, `Blit` (`SecondaryUpscaleCS.hlsl`, plain bilinear with
  `precise` maths) into a 4K texture, `CopyResource` into the frame buffer (RTVs unbound first, as
  `Upscaling.dll` does), then the UI draws over it. `frameUpscaled[frameIndex]`.
- **9060**: `CrossAdapterBridge` signals a new `readFenceB` after each lane read;
  `SecondaryUpscale::BeginFrame` (at `PostDisplay`, right after the scene lane's early transfer/read)
  queues `Wait(readFence)` + its own direct list with the FSR 4.1 dispatch on the FFX game queue, so
  the upscale overlaps the 3090's UI pass and the final frame's copy. If the reads are not queued yet
  (`bEarlyUnpack = 0`), `FinishSubmit` does it at Present after `Unpack`.
  Present: `RecordPresent` runs `Composite` into a 4K texture and copies it to the back buffer
  (native frames: plain copy of the final mirror), then FSR3 FG with `FrameOverride` (HUD-less = FSR
  output, render size + jitter from `Upscaling.dll`). FG only on upscaled frames.
- **Composite**: rebuilds the 3090's 8-bit stretch from the same scene, `alpha` = smallest UI coverage
  explaining `final - background`, `out = final + (upscaled - background) * (1 - alpha)`.
  **Verified on this PC** (scratchpad harness, real shader on both GPUs): 3090 and 9060 stretches
  differ by ≤1/255 in 3.4% of values; without UI the composite returns the FSR image ±1/255; with
  synthetic UI (opaque, 50%, 25%, 75%, 10% fade) mean error ≤1.4/255, worst ~15/255 inside opaque UI
  over a similar-coloured background (FSR detail showing through).
- Logs: `[FSR4] ...` in `AAAFrameGeneration.log` (providers, context + MB, lanes bound, first
  upscale with jitter/near/far/fov, every 10 s: frames upscaled/native, resets, FSR GPU ms, composite
  ms); `[UPSCALE] FSR 4.1 on the second GPU is ready/not ready` in `Upscaling.log`.

Build: `/W4 /WX` 0 warnings; both DLLs export their handshake functions; the shader compiles with
`fxc /Ges` for Quality/Balanced/Performance/native sizes; `fg-gpu1.patch` (20 files) applies to a
pristine `3e237a4`. CI workflow builds `Upscaling` too and fetches the upscaler DLL
(`aad4fccc…`, same commit, verified).

**First test**:
1. In Vortex, install `FO4-FG-GPU1-fsr41.zip` over the FG-GPU1 mod and let its `Upscaling.dll` win the
   conflict with the other Upscaling mod (that one is a different, incompatible build).
2. `Data\MCM\Settings\Upscaling.ini`: `iUpscaleMethodPreference = 3`, `iQualityMode = 1`; FrameGeneration:
   `iFSRRuntime = 1` (the MCM override had 0 from the 3.1.4 A/B).
3. Play 60 s at the 4K settings, then grep `[FSR4]`, `[UPSCALE]`, `[FG-GPU1]`, `CRITICAL`. Check: `lanes
   bound`, `first upscale`, upscaled ≈ all frames, FSR ms ~3.3, presented/game 142/71, `FFX Present`
   block and the timeline's `9060 list` end, payload ~77 MB.
4. Look at: UI text sharpness, HUD over sky/foliage (composite), Pip-Boy (native frames), scopes,
   foliage/weapon ghosting (FO4 motion vectors; `MotionVectorFixes` exists upstream), jitter shimmer
   (sign/scale of the jitter is the usual suspect: FSR gets `-jitter`, Upscaling.dll's convention).
5. Then raise graphics settings: the 3090 render time at 1440p is the headroom this buys.

Open risks: FO4 effects drawn after the UI boundary that are not alpha-blended UI; a camera cut does
not reset FSR history (only native frames / >250 ms gaps do); changing quality mode mid-game falls
back to native until restart (logged).

## FSR SDK 2.4 runtime option (built; first in-game run OK)

**First run, 2026-11-06 18:30, 4K, raised graphics settings, `iFSRSwapChainProvider = 0`:** swap
chain **4.0.0** + frame generation **3.1.6**, both DLLs from the plugin folder, window covers the
display, no VSync line, no FFX messages, no failed generated-frame dispatches. Steady
**63.7–64.9 game → 127.5–129.8 presented (50% generated)**, below the 71 cap: the 3090 is GPU-bound
at these settings (`3090 copy` starts 5–6 ms *after* Present, i.e. it is still rendering).
`FFX Present` block avg 7.9–8.8 ms, max 10–11.7 ms. Subjectively very smooth. Not yet comparable
with 1.1.4: the graphics settings changed, and driver-assisted pacing is not observable in the log.
Next: same settings and spot, `iFSRSwapChainProvider = 1` and `iFSRRuntime = 0`, ideally with
PresentMon. (The one-time "first 10 s" tally fired during loading, so it read 1 dispatch.)

**PresentMon A/B, 18:44–18:55** (`fo4-fg-gpu1/tools`, captures in `D:\fgb\presentmon\captures`,
60 s each, ~125 fps presented, 0 dropped, VSync off, tearing allowed):

| swap chain | sd ms | p99 ms | 1% low | jitter ms (p95) | frames >10 ms / >12 ms | max ms | mode |
|---|---|---|---|---|---|---|---|
| **4.0.0** (SDK 2.4) | **0.27** | **8.94** | **112** | **0.20 (0.49)** | **15 / 1** | 15.0 | Hardware Composed: Independent Flip |
| 3.1.7 (SDK 2.4), run 1* | 0.36 | 9.29 | 108 | 0.26 (0.73) | 38 / 7 | 16.5 | Hardware: Independent Flip |
| 3.1.7 (SDK 2.4), run 2 | 0.37 | 9.23 | 108 | 0.26 (0.67) | 40 / 11 | 16.5 | Hardware: Independent Flip |
| 3.1.4 (SDK 1.1.4, 19:01) | 0.39 | 9.51 | 105 | 0.28 (0.65) | 56 / 11 | 15.7 | Hardware: Independent Flip |

All ~125–126 fps presented (62.5–63.5 base), 0 dropped. 4.0.0: real vs generated frames on screen
8.05 / 7.88 ms (no double step); none of the runs shows one. **Verdict: the 4.0.0 swap chain cuts
the long frames 2.5–4× and the frame-to-frame jitter ~30% against 1.1.4**, at the same frame
rate; 3.1.7 sits between. Its switch to an overlay plane fits driver-assisted pacing, though the API
does not confirm it. One 60 s run per config (two for 3.1.7, which agree), same spot. *Run 1's
settings are inferred (log overwritten, no frame types, numbers match run 2); the 18:53 capture
labelled `fsr114` was really 3.1.7 per its log. The capture script now saves the config the game
really used (`*.config.txt`, from the plugin log) and the summary shows it.

`iFSRRuntime` (default **1** in the shipped settings.ini) switches FSR frame generation from
FidelityFX SDK 1.1.4 / FSR 3.1.4 to FSR SDK 2.4. `0` restores exactly the previous behaviour; the
1.1.4 code path was not changed. Background: DeepSeek's `research/FSR41-FG-PORT-REPORT.md` (sound on
the SDK facts; two corrections — there is no HUD-less/back-buffer format mismatch here, both are
RGBA8, and `posAdjust`, not the inverse view matrix, is the camera's world position because FO4
renders camera-relative) and the README section "FSR SDK 2.4".

- **What 2.4 can give on this PC (Windows 10):** the **4.0.0 swap chain** (driver-assisted frame
  pacing, if the driver engages it) with the **3.1.6** analytical interpolator. FSR 4 ML frame
  generation (4.0.2) is not offered at all on Windows 10: the provider query returns only 3.1.6, with
  or without a device (`D:\fsr-probe\inbox\probe.exe`), so there is no ID to force.
- **Driver pacing conditions on this setup:** `iPresentInterval=0` (VSync off), 4K window covering
  the display (`bFull Screen=1` in the prefs; the plugin's own swap chain is windowed and covers
  the monitor), 71 → 142 inside the 144 Hz VRR range. Not reported by the API; compare frame-time
  variance against `iFSRSwapChainProvider = 1` (3.1.7, software pacing).
- **Code:** `FidelityFX.h/.cpp` load either runtime (`LoadFFX`), create both contexts with the 2.x
  version descriptors, optional provider pins and the HUD-less format descriptor (only chained when
  the back buffer is not RGBA8 — here it is format 28, RGBA8, so it is not), and dispatch
  `PrepareV2` with camera position (`posAdjust`), up/right/forward, FOV from `projMat[1][1]` and
  `reset` after menus. Failed creation under 2.4 falls back to 1.1.4 for the session.
  Generated-frame dispatch failures are counted and logged every 10 s.
- **Headers:** SDK 2.4 API headers vendored unmodified in `overlay/include/ffx2/` (MIT). The
  upstream 1.1.4 headers in `include/` are no longer used by the plugin; the struct type IDs and
  layouts the 1.1.4 path uses are identical in 2.4 (checked).
- **Runtime DLLs:** `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_framegeneration_dx12.dll`
  from commit `7539ae8` (tag v2.4.0), fetched per file with pinned SHA-256 (the release has no
  binary asset). Licence text shipped as `FidelityFX/LICENSE-FSR-SDK-2.4.txt`.
- **Build:** clean (`/W4 /WX`, 0 warnings). Package `D:\fgb\package`, zip
  `D:\fgb\FO4-FG-GPU1-fsr24.zip` (the older `D:\fgb\FO4-FG-GPU1.zip` is the previous build).
- **First in-game test:** install the fsr24 zip, play 30–60 s at 4K, then grep the log for `[FSR]`:
  providers offered/used, `window ... covers the display`, `generated frames in the first 10 s`, any
  `generating a frame failed` / `runtime:` lines, and compare `presented/game` and
  `render thread blocked: FFX Present` with the 1.1.4 numbers below. Then repeat with
  `iFSRSwapChainProvider = 1` for the pacing A/B (PresentMon for frame-time variance).

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

**Update (latest, 17:49):** at 4K with the UI-free frame on, `iHalfResDepthMV = 1` plus early
unpack holds **71.0 base / 142.0 presented** (three steady windows, low test settings). Not yet
checked at the settings actually played. See "third session" below for the diagnosis and changes.

**Earlier update:** that optimisation, plus the early transfer of depth/MVs/HUD-less, is implemented,
built and measured — see "First measured run" and "Third change" below. At 4K the chain was then
13.3 ms: 65–70 base / 123–137 presented, 50% generated (the second session attributed this to the
3090's render time; the third session showed it was the chain).

