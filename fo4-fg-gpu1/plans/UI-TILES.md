# Plan: send only the UI tiles of the finished frame (FSR 4.1 mode)

Status: planned 2026-11-07, not started. Do this before [DISPLAY-THREAD.md](DISPLAY-THREAD.md).
New setting, off by default until measured: `bSendUITilesOnly` in `[FrameGeneration]`.

## Why

With FSR 4.1 on the second GPU, the 9060 only needs the finished 4K frame for its UI: the composite
(`SecondaryUpscaleCS.hlsl`, `Composite`) keeps the FSR 4.1 result wherever the finished frame equals
the background the 3090 stretched from the scene, and uses the finished frame only where the UI
changed it. Yet the whole 33.2 MB frame crosses PCIe every frame, and it crosses **last**:

```
2026-11-07 16:12-16:13, CS on, bExtraFrameInFlight = 1, 66.6-68.5 game fps (cap 71)
timeline ms after Present: 3090 copy 3.1-12.3 | 9060 unpack 4.3-14.9 | 9060 list 12.1-15.9
render GPU: idle between frames 2.2-2.6 ms | frame from first command to end 12.0-12.8 ms
render thread blocked: FFX Present 3.6-5.6 ms
```

FFX's Present for frame N waits until FFX's presenter thread has composited frame N-1's real
frame, which comes after N-1's chain (3090 done → copies → 9060 read → composite/FG prepare →
interpolation → paced generated frame). The finished-frame lane is ~2.7 ms of copy plus ~2.5 ms of
read at the tail of that chain. The game's HUD usually covers a small part of the screen, so
sending only the 32x32 tiles that contain UI (plus a small tile map) should cut ~4-5 ms from the
chain, which is about what separates 67-68 fps from the 71 cap. Expected side effect: less PCIe
and less 9060 read bandwidth every frame; no change in latency or image (exact, not lossy).

## Design

### Render GPU (3090, D3D11, at the top of `DX12SwapChain::Present`)

The clean background still exists at Present: `Upscaling::FillFrameBufferFromScene` writes the
bilinear stretch into `sceneBlitTarget` and copies it into the frame buffer; the UI is then drawn
over the frame buffer only. So `swapChainBufferProxy` (finished frame) and `sceneBlitTarget`
(background) are bit-identical (both RGBA8) wherever nothing was drawn after PostDisplay.

New compute shader `UITilesCS.hlsl` (in `Shaders/`), one 32x32 thread group per tile
(120x68 tiles at 3840x2160):
1. each thread compares its pixel of finished vs background (exact `!=` on the RGBA8 values);
2. group OR in groupshared memory;
3. if any pixel differs, thread 0 does `InterlockedAdd(counter, 1, slot)`; `slot < budget` →
   the group writes its 32x32 pixels of the finished frame into `atlas` at slot's block, and
   `tileMap[tile] = slot`; `slot >= budget` → `tileMap[tile] = 0xFFFE` (overflow); no UI →
   `tileMap[tile] = 0xFFFF`.

Resources (per frame slot, like the other lanes; D3D11 shared NT-handle textures opened on the
interop device):
- `tileMap`: R16_UINT, 120x68 (16 KB).
- `atlas`: RGBA8, 4096 x (32 * ceil(budget / 128)) — 128 tile blocks per row. Budget from a
  setting, `iUITileBudgetPercent` (default 15 → 1224 tiles → 4096x320, 5.2 MB).
- `counter`: one-uint UAV buffer (not shared), cleared with `ClearUnorderedAccessViewUint` before
  the dispatch; copied to a staging buffer each frame and read 2-3 frames later with
  `Map(D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT)` for the fallback decision and the stats.

In tile frames the existing `CopyResource(swapChainBufferWrapped[fi], swapChainBufferProxy)` is
skipped too (saves a 33 MB copy on the 3090).

Anything drawn after PostDisplay that is not UI (the README's known risk: effects after the UI
boundary) simply produces more tiles: the method is exact for any change, it only costs bytes.

### Which lanes cross each frame

- Upscaled frame (`frameUpscaled[fi]`), tiles on, fallback not active → `tileMap` + `atlas` lanes.
- Native frame (menus, loading, Pip-Boy: rendered at full resolution) or fallback → the existing
  finished-frame lane (full 33 MB), as today.

Fallback with hysteresis, decided on the CPU from the read-back counter (2-3 frames old): switch to
full frames when the count exceeds 75% of the budget, back to tiles after ~30 frames under 50%.
If a tile frame still overflows (sudden full-screen UI), the overflowed tiles show the FSR 4.1
result without their UI for that frame; count and log it (`overflow frames`). If that shows up in
practice, raise the budget or make overflow frames native (not upscaled) instead.

### Bridge (`CrossAdapterBridge`)

Today every lane crosses every frame and `Unpack` fails if one did not
(`"Unpack (a lane was not transferred for this frame)"`). Add a per-frame lane selection:
- `SetLaneActive(int lane, bool active)` for the frame being assembled, called before the lane's
  phase transfers (the three lanes involved are all `kPresent` timing, decided at Present).
- `Transfer`/`TransferGroup` skip inactive lanes; `SubmitUnpackLanes`, `RecordUnpack`,
  `WaitForLaneReads` skip them instead of failing; `laneFrame`/`laneUnpackFrame` stay untouched
  for skipped lanes.
- Stats: `MB/frame` from the bytes actually sent this frame, and a per-10 s line with tiles sent
  (avg/max), tile frames vs full frames, fallbacks, overflows.

### Display GPU (9060, `SecondaryUpscale`)

- Mirrors of `tileMap` and `atlas` per slot (normal `AddLane(..., createMirror=true)`).
- New shader entry `CompositeTiles` in `SecondaryUpscaleCS.hlsl`: per pixel, `slot =
  TileMap.Load(pixel / 32)`; `0xFFFF` → output the FSR 4.1 result; `0xFFFE` → same (overflow,
  counted); otherwise `final = Atlas.Load(blockOrigin(slot) + pixel % 32)` and the existing
  composite formula (background rebuilt with `SceneBilinear`, coverage, blend).
- Descriptor table grows from t0-t2 to t0-t4 (scene, finished, FSR output, tile map, atlas);
  `kDescriptorsPerSlot` 4 → 6. `RecordPresent(..., bool a_tiles)` picks the PSO.
- Writes the display format (HDR-aware as now): no change to the conversion logic.

### Settings / MCM

`[FrameGeneration]` `bSendUITilesOnly = 0` (restart: the lanes are created at startup) and
`iUITileBudgetPercent = 15` (restart). MCM: Second GPU page, next to `bExtraFrameInFlight`.
Only meaningful with FSR 4.1 on the second GPU (`iUpscaleMethodPreference = 3`); log once and
ignore otherwise.

## Steps

1. **Measure first (cheap):** add only the 3090 tile pass + counter readback, no lane changes, and
   log tiles with UI per frame (avg, p95, max) in normal play, menus excluded. Confirms the budget
   and that the comparison really is exact (an untouched scene must give ~0 tiles: check with the
   HUD hidden, `tm` console command).
2. Bridge per-frame lane selection (`SetLaneActive`), with the finished-frame lane always active
   (no behaviour change); build and run once to prove nothing regressed.
3. Tile and map lanes + `CompositeTiles` + per-frame switch + fallback + stats, behind
   `bSendUITilesOnly`.
4. Test in game (below), PresentMon A/B (`tools/presentmon-capture.ps1`) at the same spot, tiles
   on/off, with and without `bExtraFrameInFlight`.

## Tests and acceptance

- Log: `tiles sent avg/max`, payload MB/frame drops from 73.8 to ~45 at 4K Quality on tile frames,
  `3090 copy` end and `9060 unpack` end move ~2-5 ms earlier, `render thread blocked` and
  `render GPU: idle between frames` drop; base fps up (target 71 with CS on).
- Visual: HUD (compass, health, ammo, crosshair), subtitles, quest/XP notifications, hit markers,
  dialogue choices, scope overlays, power armor HUD, the HUD fading in/out, Pip-Boy (native frames:
  must look as before), loading screens, the console. No missing or stale UI, no seams at tile
  edges.
- Fallback: open a full-screen UI that is still an upscaled frame (if any) or lower the budget to
  1% to force it; must switch to full frames without artifacts and back.
- Off: `bSendUITilesOnly = 0` must behave exactly as today.

## Risks / open questions

- Exactness relies on the frame buffer being a straight copy of `sceneBlitTarget` until the UI;
  verify in step 1. If some engine pass writes the frame buffer between PostDisplay and the UI in
  a way that is not UI, tiles grow; still correct.
- D3D11 shared R16_UINT textures: if sharing fails for that format, use R32_UINT.
- Tile size: 32x32 is the D3D11 group maximum (1024 threads). 16x16 would fit text tighter (more
  tiles, smaller each); try if step 1 shows many mostly-empty tiles.
- Does not help native frames (menus) and needs nothing else to change for HDR.
