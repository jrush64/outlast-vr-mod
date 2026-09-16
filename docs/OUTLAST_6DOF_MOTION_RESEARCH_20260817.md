# Outlast VR: 6DOF + motion controls research (2026-08-17)

Source snapshot before any of this work: `backups/src-20260817-095232-PRE-6DOF-7354e5a6`
(all .cpp/.h/.def/.bat + deployed d3d9.dll + release outlastvr.ini). Git HEAD 7354e5a6 on
`codex/ffxv-mono-vr`, pushed to outlast-vr master already. Only `builds/d3d9.dll` + `.map` are
dirty in the Outlast tree.

This doc is about what is NEXT for 6DOF and motion controls.

## 1. Where 6DOF actually stands (it is closer than the menu suggests)

Positional tracking is NOT missing. It is built, wired end to end, and switched off.

| Piece | Where | State |
|---|---|---|
| Head position in the recenter frame | `ol_xr.cpp:1504-1506` `relP = QConj(g_baseQ) * (centre - g_baseP)` | live every frame |
| Published to the game thread | `ol_xr.cpp:1533` (inline) / `ol_xr.cpp:754` (worker, frozen at publish) | live |
| Applied to the game camera | `ol_ue3.cpp:730-740` in `Hook_GetViewPoint`, base (gamepad) frame, `WorldToMeters` uu/m | gated on `g_headPos` |
| Switch | `[Stage3] HeadPosition=1` in outlastvr.ini, `OLUE3_SetHeadPos` | default 0, menu control removed 2026-08-04 |
| Scale | `[Stage3] WorldToMeters` (5..500, default 50) | inert while HeadPosition=0 |
| Declared pose | `g_headPoseApp.position = relP` | already carries the real position |

Why it was turned off (handoff section 4.18, and the long comment at `ol_ue3.cpp:716`): the
baseline is captured on the FIRST VALID POSE (`g_recenterReq` starts at 1, `ol_xr.cpp:85`).
That fires while the game is still loading and the headset is on the desk, so every later
position is measured from desk height = camera ~1 m too high, looking down at Miles's head.
The three fixes described in 4.18 (rebaseline on FOCUSED, drift watchdog, +/-1 m clamp) are
NOT in the current tree - grep finds none of them; they went with the ee909098 wholesale
revert. So the mechanism has never been judged with a sane baseline.

Also true: the game camera is BONE-DRIVEN (`OLGame.ini [OLGame.OLHeroCamera]
CameraBoneName=Hero-Camera`). Whatever `GetViewPoint` returns already contains the animated
bob/sway/climb motion; the positional offset adds on top. That is the same as today in 3DOF, not new.

### 1a. What "proper 6DOF" needs - all small

1. **Baseline discipline.** Positional stays INERT until a trustworthy baseline exists:
   - the boot recenter (first valid pose) does NOT arm positional;
   - a manual recenter (G / double-tap pad / menu button) arms it;
   - one-shot auto recenter on the `XR_SESSION_STATE_FOCUSED` transition (the [XRSTATE]
     handler already names every state, `ol_xr.cpp:590`), THEN a settle check: arm only after
     the head has moved < ~3 cm over ~1.5 s. That covers "put the headset on, then it goes".
2. **Clamp** the applied offset: horizontal radius ~0.6 m, vertical -0.9..+0.4 m. Nothing a
   wrong baseline can do puts the camera in orbit again. Log `head=(x,y,z)m` on the periodic
   line so a desk baseline is visible in one glance (a seated human stays within ~0.2 m).
3. **Scale sanity.** `WorldToMeters` MUST match the shipped stereo. Shipped HalfEyeUU=4.5
   (9 uu between eyes). At 50 uu/m that declares 18 cm eyes = 2.6x the measured 6.97 cm IPD
   (hyperstereo, deliberate, world reads small). Real-scale positional at 50 uu/m will feel
   sluggish against a world that reads 2.6x smaller. Matching value = 2*4.5/0.0697 = ~129 uu/m.
   Ship WorldToMeters DERIVED from HalfEyeUU by default (one dial), with the ini override kept.
4. **Menu control back**, one line: "Room-scale (lean/duck)" on/off. It was removed because it
   was inert; once armed properly it is not.
5. **Cinema screen / injW.** Positional already rides `injW` (`ol_ue3.cpp:731`), so it fades
   with rotation when the screen is up. Nothing to do.

### 1b. Known trades to state up front (do not solve in the first pass)
- **Walls.** Leaning through geometry. Every flat2VR mod accepts this; the clamp bounds it. A
  fade-to-black on penetration needs an engine trace - later, only if requested.
- **Crouch mismatch.** Physically ducking moves the camera, not the pawn: enemies still see a
  standing Miles and you cannot slide under a bed by ducking. Cheap fix once the XInput merge
  (section 2) exists: head drops > ~0.35 m below baseline -> hold the game's crouch (B on pad).
  Physical crouch = hide is a genuinely good Outlast VR feature; make it an option.
- **Head mesh.** Ducking a real 0.5 m may look up into the character's own head/neck if the
  head is not hidden in first person. Test, do not theorise.

### 1c. Zero-code test to run FIRST (answers "is only the baseline wrong?")
1. `HeadPosition=1` in the game folder outlastvr.ini, `WorldToMeters=129`.
2. Launch, put the headset on, wait for gameplay, press G once (recenter = position baseline).
3. Lean, duck, peer round a corner. If it feels right and only the pre-recenter height was
   off, section 1a is the whole job. If it feels wrong AFTER a manual recenter, the mechanism
   itself is at fault (axis sign at `ol_ue3.cpp:732-739`, or the yaw-only baseline frame) and
   that gets fixed before any of the discipline work.

## 2. Motion controls - what it means for THIS game

Outlast has no weapon and no free hands. Interactions are contextual (Use), the camcorder is a
full-screen viewfinder mode, not a held object. So "motion controls" decomposes into:

**Tier 1 - VR controllers ARE the gamepad (the real deliverable).** OpenXR action set ->
merged into XInput state -> the game plays exactly as with a pad, no gamepad needed, controller
glyphs appear in the UI. Adds the two things a VR player actually needs on top:
- **Head-relative locomotion.** Today stick-forward walks along the BODY yaw (the pad aim,
  `g_baseViewRot`); turn your head 90 deg and push forward and you strafe relative to your
  view. Rotate the left stick vector by the head yaw delta (`g_olYawRad`, already known) in
  the merge and forward = where you look. Sign to be tested, one variable.
- **Snap turn** (right stick flick): mod-side yaw offset added to the injected yaw AND to the
  stick rotation. Body yaw untouched, so nothing else in the game moves. Smooth turn is
  free (right stick -> aMouseX already).
- Menu navigation and recenter with the VR controllers for free, if the mod's own two XInput
  readers (`ol_menu.cpp:205`, `d3d9_proxy.cpp:987`) read the merged state.

**Tier 2 - hand-driven bits that fit Outlast.**
- Camcorder / NV light follows the RIGHT HAND aim instead of the head. `Hook_SpotSetRotation`
  (`ol_ue3.cpp:629`) already offsets the light by a head delta; swap in the controller's aim
  delta and the light is a hand torch. Small, high presence value.
- Physical crouch -> game crouch (section 1b).
- Camcorder raise gesture (right hand above head -> toggle camcorder): gimmick, skip unless asked.

**Tier 3 - NOT realistic from a d3d9 proxy: hands/arms following the controllers.** Outlast's
first-person arms are a UE3 skeletal mesh attached to the camera bone, animated. Driving them
from controller poses means IK on the engine skeleton with no script access. Say no.

### 2a. Facts measured today that shape the input build
- `OLGame.exe` imports **XINPUT1_3.dll by ORDINAL** (2 = XInputGetState, 3 = XInputSetState),
  nothing else from it - no XInputGetCapabilities/Enable. Loads the game-folder copy
  (`Binaries\Win64\xinput1_3.dll`, 107368 bytes = the genuine June 2010 redist). So:
  MinHook the EXPORT (GetProcAddress by ordinal 2), not an IAT-by-name patch (CP2077's
  `InstallXInputHook` at `Games/Cyberpunk2077/src/vr/core/vr_core.cpp:7325` matches by name -
  that would miss here). The mod's own dll links `xinput.lib` = XInput1_4, a different module,
  so its readers are unaffected by the hook and must be pointed at the merged state explicitly.
- Mouse is DirectInput8 (`DirectInput8Create` import; the menu-freeze comment in ol_ue3
  already knew). Irrelevant to controllers, head does the looking.
- Pad map (OLInput.ini, live): LX/LY move, RX/RY look (aMouseX 15 / aMouseY -12), A jump,
  B crouch toggle, X use, Y reload(battery), LB run, RB camcorder, R3 night vision,
  L3 unbound, LT/RT axes = analog lean, DPad up/down zoom, DPad L/R recording/evidence menus,
  Back tab menu, Start pause. **Recenter is double-tap L1 = OLA_Run** - with a VR controller
  that becomes double-grip or similar; revisit the binding once the merge exists.
- The submit thread owns ALL OpenXR calls ([SUBMITTHREAD], `ol_xr.cpp:767`). Action sync
  (`xrSyncActions`, `xrGetActionState*`, `xrLocateSpace` for the hands) goes on the worker
  right after `xrWaitFrame` with the predicted time, and publishes a small pad struct under
  `g_pubLock` for the game thread. Same shape as the pose. Remember the [POSEFREEZE] lesson:
  anything the game reads must be promoted once per game frame, not at 90 Hz.
- `ol_xr_types.h` is a hand-rolled OpenXR header (no openxr.h). Actions need ~12 more
  function pointers and ~10 structs (ActionSet/Action create infos, suggested bindings,
  attach info, sync info, action states bool/float/vec2, action space create, space location,
  xrStringToPath). Mechanical, ~150 lines; keep the `_VALUE` suffix convention.
- Ready-made reference for the action set + 5 interaction profiles (Touch, Index, Vive, WMR,
  simple): `Games/Cyberpunk2077/src/vr/openxr/openxr_manager.cpp:566-845` and the per-frame
  sync at `openxr_frameloop.cpp:641-913`. Copy the shape, not the file.

### 2b. Open question worth checking before building the merge
UE3's WinDrv polls XInputGetState every tick and treats ERROR_SUCCESS as "connected"; some
UE3 builds throttle polling of a pad that was disconnected at boot. Cheapest proof: hook
first, count game-thread calls/sec with NO pad plugged in, log it. If it polls, the merge is
enough. If it does not, return ERROR_SUCCESS from the very first call so it never marks the
slot dead.

## 3. Suggested order (each step is one deployable build, each has a yes/no test)

0. Zero-code test (1c). Answers whether the positional mechanism is right.
1. **6DOF proper**: baseline discipline + clamp + derived WorldToMeters + menu toggle + head
   log line. Test: recenter once, lean/duck; camera height correct, no drift; menu toggle
   returns it to today's behaviour byte-for-byte.
2. **XInput merge with a stub source** (fake a constant stick from the ini) - proves the hook
   drives the game before any OpenXR input exists. Test: Miles walks with no pad plugged in.
3. **OpenXR action set on the worker** -> real controllers into the merge. Test: play a
   corridor with Touch controllers only, all pad functions reachable, glyphs switch.
4. **Head-relative locomotion + snap turn** (options, default head-relative ON, snap OFF).
5. **Physical crouch -> game crouch** (option, default ON), NV light follows right hand
   (option, default OFF until confirmed in headset).

Steps 1 and 2 are independent - one session each. 3 is the biggest (header + action set +
threading), one to two sessions. 4 and 5 are afternoons once 3 lands.

## 4. Things NOT to do (already learned)
- Do not re-baseline automatically on a timer or on "offset looks big" while playing -
  ducking under a bed is a big offset. Only FOCUSED-once + manual.
- Do not tie positional to the OLD launch-time baseline again; that is exactly what got the
  feature removed.
- Do not touch the stereo model, convergence, or the shipped separation values while doing
  this. Positional and stereo share ONE dial (WorldToMeters derived from HalfEyeUU) and that
  is the only stereo-adjacent change.
- Do not put OpenXR calls on the game thread; the worker owns the session.
- No hotkeys ship. Every new control goes through the menu + ini, safe for a stranger to press.
