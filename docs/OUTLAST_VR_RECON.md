# Outlast (2013) — VR Lane RECON

Status: **Stage 1 disk recon DONE 2026-07-21** — installed at
`E:\SteamLibrary\steamapps\common\Outlast`. Nothing built, nothing verified in-process yet.
Method reference: `Docs/UE3_VR_PORTING_PLAYBOOK.md`, `Docs/MELE_SAMEFRAME_STEREO_METHOD.md`,
`Docs/UE3_STEREO_CODE_PORTING_GUIDE.md`.

## 1. What the game is — VERIFIED ON DISK

| Fact | Value | How verified |
|---|---|---|
| Engine | Unreal Engine 3, licensee build from `D:\Perforce\outlast\UnrealEngine3\Development\Src\` | exe file-path strings |
| Target exe | `Binaries\Win64\OLGame.exe`, **x64 (PE32+, machine 0x8664)** | PE header dump |
| Launch line | launcher runs `Binaries\Win64\OLGame.exe -installed -seekfreeloadingpcconsole` (Win32 fallback exists) | `OutlastLauncher.exe` UTF-16 strings |
| Renderer | **D3D9.** `d3d9.dll` is a **static import**; `d3d11.dll` is NOT imported | PE import table |
| DX11 RHI | linked in (`D3D11Drv\Src\D3D11Device.cpp`, SM4/SM5 profiles) but **unusable** — only `GlobalShaderCache-PC-D3D-SM3.bin` / `RefShaderCache-PC-D3D-SM3.upk` are cooked, no SM5 | CookedPCConsole listing |
| DRM | Steam only (`steam_api64.dll`), no Denuvo → clean Ghidra target | import table |
| Config | `Documents\My Games\Outlast\OLGame\Config\OLEngine.ini` + `OLGame.ini` | web + shipped Default*.ini |
| Camera | first-person, camcorder, `DefaultFOV=90`, `RunningFOV`, `CamcorderMin/MaxFOV` in `OLGame.ini` | web |
| Community 3D | Helix Mod DX9 fix (2013); vorpX **G3D geometry profile** | web |
| Native stereo flag | `AllowNvidiaStereo3d=False→True` in `OLEngine.ini` | web (no matching exe string found — treat as unconfirmed) |

### ★ The find: `OLGame_R.exe` is a symbol oracle
`Binaries\Win64\OLGame_R.exe` (89 MB, also x64, also d3d9) is a **non-shipping UE3 build** — it
imports the wxWidgets set (`wxmsw28u_*`) and `mscoree.dll`, i.e. the UnrealEd/editor toolchain, and
it retains engine **source-file-path strings in `check()`/`appErrorf` sites**: 42 hits on
`UnPlayer.cpp` (where `ULocalPlayer::CalcSceneView` lives in UE3) and 48 on `SceneRendering.cpp`.
Shipping `OLGame.exe` has none of these.

This collapses Stage 3. Instead of behavioral vtable-sweeping (the Dishonored slog), find
CalcSceneView in `OLGame_R.exe` by its `UnPlayer.cpp` + line-number check strings, then pattern-match
the same function into shipping `OLGame.exe`. The launcher can start `_R` too, so it may even be
directly runnable as a dev target.

The full UE3 dev toolchain also ships in `Binaries\` (UnrealFrontend, CookerSync, ShaderKeyTool,
UE3ShaderCompileWorker) — noted, not yet useful.

**vorpX G3D + a Helix DX9 fix both existing = geometric stereo on this title is proven possible by
construction.** The question is not whether, but which mechanism to use.

## 2. The two mechanisms, and which one is chosen

### Path A (recommended) — engine-level same-frame split, ported from ME2/ME3 + Dishonored Phase A
Hook `ULocalPlayer::CalcSceneView`, call it twice per frame with the LocalPlayer fractional
viewport rect rewritten (L={0,0,0.5,1}, R={0.5,0,0.5,1}) and the view location offset ±halfEye
along the view-right vector. Engine renders both eyes SBS in one frame; the proxy splits and
submits per eye.

Why this one:
- It is the **only stereo method confirmed working in shipped builds** (ME1/ME2/ME3).
- Outlast is the same engine generation as Dishonored, whose CalcSceneView/Draw layout was fully
  mapped statically in Ghidra — the *procedure* for finding those functions is written down and
  repeatable (`DISHONORED_PROPER_STEREO_PLAN.md` Phase A).
- It is renderer-agnostic. D3D9 only matters at the submit end.

Cost: the Dishonored offsets are x86 and useless here. All addresses must be re-derived for
Outlast's **x64** `OLGame.exe`. That's a Ghidra job, not a live-poking job.

### Path B (fallback) — D3D9 vertex-shader-constant stereo (the Helix/3D-Vision way)
Find the view-projection matrix constant register in `SetVertexShaderConstantF`, apply the classic
`clip.x += sep * (clip.w - conv)` per eye, render the frame twice. This is what Helix's fix does.
Keep it as fallback if the engine-level split proves unworkable — but it inherits every 3D-Vision
artifact (shadows, decals, water at wrong depth), the same tail encountered in Witcher 3.

**Do not** start with Path B just because it looks quicker.

## 3. The submit end (D3D9 → OpenXR)

Reuse `Games/Dishonored/probe/DishonoredFlatXR` — it already solves D3D9→D3D11→OpenXR:
`d3d9_proxy.cpp` (proxy `d3d9.dll` next to the exe), `dish_xr.cpp` (D3D11 bridge device + XR session
+ two swapchains), `dish_stereo.cpp`.

Two deltas vs Dishonored:
1. **x64.** Good news: all of ME1's OpenXR types port unchanged — none of the BioShock x86 ABI
   pain (`__stdcall` PFNs, 64-bit handle truncation). The D3D9 proxy just recompiles x64.
2. **Bridge cost.** Dishonored's bridge is a CPU readback
   (`GetRenderTargetData` → SYSTEMMEM → upload to D3D11). It works but it's the slow path. Upgrade
   worth trying on Outlast: the proxy owns `Direct3DCreate9`, so it can hand the game a
   **D3D9Ex** device and use `D3DUSAGE_RENDERTARGET` **shared surfaces** opened by D3D11 via
   `OpenSharedResource` — zero CPU copy. Try readback first (known-good), then upgrade.

## 4. Stage plan

- **Stage 1 — Recon on disk** (do the moment the download finishes)
  - Confirm bitness of both `OLGame.exe`s; confirm which one the launcher runs; confirm the game
    imports `d3d9.dll` (and check for `d3d9_x43.dll` in Win64 — a shipped helper that may need
    proxying too).
  - Dump `OLEngine.ini` / `OLGame.ini`; note FOV keys, `bSmoothFrameRate`, res keys.
- **Stage 2 — Flat XR** — port the Dishonored proxy to x64, boot Outlast to a headset quad,
  mono. Gate: game runs, log shows presents climbing at real framerate, image in headset.
- **Stage 3 — Ghidra static map** — GObjects/GNames, `ULocalPlayer::CalcSceneView`,
  `UGameViewportClient::Draw`, the LocalPlayer fractional-rect field offsets (x64 layout:
  expect the Dishonored `+0x64..+0x70` block to shift — re-derive, don't assume).
- **Stage 4 — Ownership proof** — hook CalcSceneView, nudge the view location by a big value,
  confirm real scene pixels move every frame. STOP and rethink if only UI moves.
- **Stage 5 — Same-frame split** — double-call + rect rewrite → SBS in one frame → split + submit
  per eye with pose tagging. Then IPD/world-scale (Outlast uses UE3 units, ~50 uu/m start).
- **Stage 6 — polish** — HUD/camcorder overlay per eye, head tracking into `ControlRotation`
  (ramped — never step it in one frame, per the ME1 storm lesson), comfort options.

## 5. Known traps carried in from other lanes

- Declared FOV to the compositor must **exactly** match what was rendered — Outlast's FOV is
  ini-driven and changes at runtime (running FOV, camcorder zoom). Read the live value per frame,
  never assume 90.
- Never vtable-hook Present; wrap the device/swapchain (BioShock lesson). D3D9 proxy already does this.
- Outlast's camcorder/night-vision is a full-screen post pass — expect it to need per-eye handling.
- Heavy head-bob + scripted camera grabs are a comfort problem, not a rendering one. Plan a toggle.

## 6. Open questions — status after Stage 1

1. ~~Is the Win64 exe D3D9?~~ **Answered: yes, static `d3d9.dll` import, x64.** Proxy `d3d9.dll`
   dropped in `Binaries\Win64\` is the injection vector.
2. ~~Could the DX11 renderer be forced and ME1 code reused directly?~~ **Answered: no.** D3D11Drv is
   compiled in, but only SM3 shader caches shipped — nothing to run on it, and no `-d3d11` switch
   string. Don't chase it.
3. Does `AllowNvidiaStereo3d=True` reach an engine stereo view path that can be hijacked? No matching
   string found in either exe — still worth one ini experiment, low priority.
4. Does `OLGame_R.exe` actually boot the game (and does it expose the UE3 console)? Untested —
   this is a one-minute test worth doing before Stage 3.
