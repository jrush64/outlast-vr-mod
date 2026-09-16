// ol_xr.cpp - Outlast FlatXR: D3D9 -> (CPU readback) -> D3D11 -> OpenXR.
// =============================================================================
// Stage 2 of the Outlast lane: get the game's frame onto the headset, mono/flat.
// No camera work here - that's Stage 3+ (engine-level same-frame split).
//
// Per D3D9 frame, all on the game's Present thread (which owns the D3D11 context created here):
//   D3D9 GetBackBuffer -> CreateOffscreenPlainSurface(SYSTEMMEM) -> GetRenderTargetData
//   -> LockRect -> UpdateSubresource into a D3D11 BGRA texture (a locally created device)
//   -> xrAcquire/Wait -> CopyResource into the XR swapchain image -> xrRelease
//   -> submit ONE layer: head-locked quad (default) or a projection layer (P).
//
// Ported from Games/Dishonored/probe/DishonoredFlatXR/dish_xr.cpp, stripped to the
// parts that are known-good, and rebuilt x64. Outlast's Win64 OLGame.exe is x64, so
// none of the x86 OpenXR ABI corrections matter here - but the header is shared-safe.
//
// OpenXR is loaded dynamically (x64 openxr_loader.dll next to the game exe); no SDK link.
// =============================================================================

#include "ol_xr_types.h"
#include "ol_xr.h"
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <math.h>

using namespace OLXR;

static void XLOG(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
    OLProxyLog(buf);
}

// ---- state ----
static bool g_dead = false;      // init failed hard -> never retry
static bool g_inited = false;
static bool g_running = false;   // between xrBeginSession and STOPPING

static HMODULE g_loaderDll = nullptr;
static PFN_xrGetInstanceProcAddr g_getProc = nullptr;
static XrInstance  g_instance = 0;
static XrSystemId  g_system = 0;
static XrSession   g_session = 0;
static XrSwapchain g_swapchain = 0;
static XrSpace     g_viewSpace = 0;    // head-locked (quad layer)
static XrSpace     g_localSpace = 0;   // fixed world space (pose sampling)
static XrSpace     g_appSpace = 0;     // world-anchored space for the projection layer
static Functions   g_fn;

static ID3D11Device*        g_d3d11 = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11Texture2D*     g_uploadTex = nullptr;
static std::vector<ID3D11Texture2D*> g_xrImages;
// ---- per-eye (SBS) path ----
// When the engine-side split is running, the backbuffer holds two half-width views. Each half
// goes to its own eye swapchain, and the layer is a real projection layer with the FOV the
// engine actually rendered.
static XrSwapchain g_scL = 0, g_scR = 0;
static std::vector<ID3D11Texture2D*> g_imgL, g_imgR;
static UINT g_eyeW = 0, g_eyeH = 0;
static bool g_eyeInitTried = false;
// AER holds the eye NOT rendered this frame. A swapchain hands back a rotating image index,
// so the previous frame's contents cannot be relied on - a separate copy per eye is kept and
// re-blit BOTH into their swapchains every frame. Sized to g_eyeW/g_eyeH, so a mode change
// invalidates these too.
static ID3D11Texture2D* g_eyeHold[2] = { nullptr, nullptr };

static IDirect3DSurface9*   g_sysSurf = nullptr;   // D3D9 SYSTEMMEM readback surface
static UINT g_w = 0, g_h = 0;
static long long g_frame = 0;

// ---- head pose (exported; nothing consumes it yet) ----
volatile float g_olYawRad = 0.0f, g_olPitchRad = 0.0f, g_olRollRad = 0.0f;
volatile float g_olHeadX = 0.0f, g_olHeadY = 0.0f, g_olHeadZ = 0.0f;
volatile long  g_olPoseValid = 0;
volatile float g_olDeclFovXDeg = 0.0f, g_olDeclFovYDeg = 0.0f;
volatile long  g_olUseProjection = 0;

static XrQuaternionf g_baseQ = { 0, 0, 0, 1 };
static XrVector3f    g_baseP = { 0, 0, 0 };
static XrPosef       g_headPoseApp;
static volatile long g_recenterReq = 1;      // recenter on the first valid pose
static volatile long g_centered = 0;

// [HEADPOS] POSITION BASELINE DISCIPLINE. The boot recenter fires on the first valid pose,
// while the game is still loading and the headset is usually on the desk. A yaw baseline
// taken there is harmless (one G press fixes it). A POSITION baseline taken there is not: every
// later head position is measured from desk height, the camera sits about a metre above the
// character and looks down at his own head. That is why lean/duck was removed from the menu.
// So position is only APPLIED once the baseline is trusted ("armed"):
//   - a manual recenter (key / pad / menu button) arms it on the spot - the player is
//     wearing the headset and facing forward by definition;
//   - otherwise, arm automatically ONCE: the headset must first MOVE well away from the boot
//     baseline (picked up off the desk), then hold still for a moment (on a head, reading the
//     menu). Position zero is taken at that still pose. A desk is dead still from the start
//     and never crosses the pickup threshold, so it can never arm; a head already wearing the
//     headset at launch never crosses it either and simply waits for G, as before.
// Rotation is untouched by all of this: yaw/pitch keep the shipped behaviour exactly.
static volatile long g_recenterManual = 0;   // the pending recenter was asked for by the player
static volatile long g_posArmed = 0;         // position baseline trusted: lean/duck may apply
static long          g_posAutoDone = 0;      // the automatic arm happens once per session
static long          g_posPickedUp = 0;      // moved > kPickupM from the boot baseline
static XrVector3f    g_settleRef = { 0, 0, 0 };
static XrTime        g_settleSince = 0;
static const float   kPickupM = 0.25f;       // desk -> head is 0.5-1.0 m; a lean is less
static const float   kStillM  = 0.02f;       // "holding still" radius
static const XrTime  kStillNs = 1000000000;  // for one second
extern "C" int OLXR_PosArmed() { return g_posArmed ? 1 : 0; }

// ---- menu-tunable stereo/tracking knobs -------------------------------------------------------
// CONVERGENCE slides the two eye images horizontally at SUBMIT, which moves the depth plane that
// sits at screen depth without touching the render. It is done here rather than by shearing the
// projection because shearing breaks fusion - a known dead end on this project.
// SMOOTHING is an exponential filter on the head angles; 0 keeps the raw, lowest-latency reading.
// CONVERGENCE, in ME1's units (2026-07-23). The slider is an "inward shift" in -3..+3 and ONE
// UNIT = 8% of the eye's width (ME1 sbs_splitter.cpp: conv = shift * 0.08). The old knob was a
// fixed +/-48 px - 2.6% of an 1832 px eye - so the ENTIRE Outlast range was smaller than ME1's
// DEFAULT (0.5 -> 4%), which is exactly why the slider read as "convergence doesn't work".
// Pixels are derived from the fraction at submit, so the feel survives resolution changes.
static const float kConvUnit = 0.08f;      // fraction of eye width per slider unit - ME1's number
static volatile float g_convShift = 0.5f;  // ME1's default inward shift

// ---- [STEREOMODEL] two complete stereo models, switchable live ---------------------------------
// Built 2026-08-04 to settle by experiment which model this game wants, rather than by argument.
// The two differ in exactly two behaviours and nothing else; everything upstream of here (the
// split, the camera offsets, head tracking) is shared.
//
// MODEL 0, "Outlast" - what this build has always shipped:
//   CONVERGENCE reserves SLACK. Each eye image is cut narrower by the shift amount so a plain box
//   copy can slide without running off the edge, and the declared frustum is narrowed to match.
//   Honest, but raising convergence visibly eats the left and right edges of your view.
//   SWAP flips the CAMERA only (ol_ue3), leaving the routing alone, so it genuinely inverts the
//   stereo: near objects get divergent disparity and cannot be fused. It is a rescue switch for a
//   backwards setup, not a quality control.
//
// MODEL 1, "Mass Effect" - how ME1/ME2 do it:
//   CONVERGENCE samples a SHIFTED window through a clamping sampler, so nothing is reserved and
//   no width is lost at any setting; the declared frustum stays the full symmetric half and the
//   small angular lie IS the toe-in. Sign is bolted to the HALF, not the eye.
//   SWAP flips the camera AND the routing, which cancels out for depth (left eye still sees the
//   left viewpoint, so no inversion and no near-object doubling) and instead INVERTS the
//   convergence plane, because the convergence sign rides the half. That inversion is the whole
//   visible effect of ME's swap toggle.
// DEFAULT IS 1, the Mass Effect model (chosen 2026-08-04 after a live A/B). Model 0 is kept
// because it is the whole history of this lane and reverting to it is one ini key, but it is no
// longer reachable from the menu: it costs field of view on every convergence change and its
// Swap inverts the stereo, and neither is something to hand a player.
static volatile long g_meModel = 1;        // 0 = Outlast, 1 = Mass Effect
extern "C" int  OLXR_GetStereoModel() { return g_meModel ? 1 : 0; }
extern "C" void OLXR_SetStereoModel(int v) { InterlockedExchange(&g_meModel, v ? 1 : 0); }
static int g_cropLx = 0, g_cropRx = 0;     // where each eye's window actually landed this frame
static float g_rtFovX = 0.0f, g_rtFovY = 0.0f;   // the HEADSET's own FOV, as reported by the runtime

// ---- [XRRUNTIME] runtime quirks, ported from ME1 (2026-08-04) -----------------------------------
// Two PC runtimes do not treat a declared projection FOV the way Virtual Desktop does, and both
// failures were found the hard way in the Mass Effect lane rather than reasoned about:
//  * META (Quest Link / Air Link, runtime name "Oculus" or "Meta") composites a projection layer
//    using ITS OWN per-eye asymmetric frustum from xrLocateViews and ignores a declared FOV that
//    differs. This mod's is symmetric, so each eye lands several degrees off in OPPOSITE directions:
//    permanent, unfusable doubling in every mode. VDXR honours the declared FOV, which is exactly
//    why this build has always fused here and would not have on a Link cable.
//  * STEAMVR shows NOTHING at all for a projection view declared WIDER than its own per-eye
//    frustum. endFrame succeeds, the session reports focused, the compositor stays void.
// One cure for both: declare the INTERSECTION of the declared window and the runtime's frustum,
// and crop the submitted rect to exactly that intersection, in tan space. No extra rendering, and
// "declared FOV must match what was rendered" stays true because the rect shrinks with it.
// Where the declared window is NARROWER than the runtime's, the intersection is that window and
// it no-ops.
static bool g_isOculusRuntime = false;
static bool g_isSteamVrRuntime = false;
static XrFovf g_rtEyeFov[2] = {};          // the runtime's own per-eye frustum, from xrLocateViews
static bool   g_rtEyeFovValid = false;
static XrPosef g_rtEyePose[2] = {};        // [XRSUBMIT] the runtime's own per-eye viewpoints
static float   g_rtEyeSep = 0.0f;          // metres between them, measured from xrLocateViews
extern "C" float OLXR_GetRtEyeSep() { return g_rtEyeSep; }
// [XRSUBMIT] the last declaration actually handed to the compositor, for the periodic dump.
static XrFovf  g_dbgDeclFov[2] = {};
static int32_t g_dbgRect[2][4] = {};       // x, y, w, h per eye
static XrPosef g_dbgDeclPose = {};
// [STEAMVRSCENE] a 64px opaque-black swapchain, cleared once, submitted as a placeholder
// PROJECTION layer under quad-only frames so SteamVR's waiting room does not take the screen.
static const int32_t kBlackSceneSize = 64;
static XrSwapchain g_blackSc = 0;
static std::vector<ID3D11Texture2D*> g_blackImgs;
static bool g_blackTried = false;
static bool g_blackAllowed = true;         // [XR] BlackSceneLayer=0 stands the placeholder down
static bool g_perEyePose = false;          // resolved from g_perEyeMode once the runtime is known
static int  g_perEyeMode = -1;             // [XR] PerEyePose: -1 auto, 0 off, 1 on
static bool g_fitFrustum = false;          // [FITFRUSTUM], resolved once the runtime is known
static int  g_fitMode = -1;                // [XR] FitRuntimeFrustum: -1 auto, 0 off, 1 on

// [SUBMITTHREAD] SteamVR parks its environment grid at ~1.0 alpha over any scene app it still
// considers "loading", and it clears that state only when the app delivers frames at the
// compositor's own cadence - measured live 2026-08-04 (grid pinned at 0.99 for whole sessions,
// actively re-asserted against a forced FadeGrid; Home clears it in ~3s). This game tops out
// near 45fps with culling force-disabled, so a submit loop driven by the game's Present can
// NEVER satisfy a 90Hz compositor and the grid never lifts - the "game is transparent over the
// SteamVR environment" report. Virtual Desktop's runtime has no such gate, which is why the
// same code always looked fine on VDXR.
// The fix is to decouple: the game thread only PUBLISHES frames (D3D9 readback -> CPU buffer,
// paired with the pose that frame was rendered with), and a worker thread owns every OpenXR
// call, heartbeating waitFrame/endFrame at the compositor's rate and re-submitting the held
// frame whenever the game has not produced a new one. [XR] SubmitThread=0 reverts to the old
// inline submit.
static bool            g_useWorker = true;
static HANDLE          g_workerThread = nullptr;
static volatile LONG   g_workerStop = 0;
static SRWLOCK         g_pubLock = SRWLOCK_INIT;
static std::vector<uint8_t> g_pubPixels;   // tightly packed BGRA rows, g_pubW*4 pitch
static UINT            g_pubW = 0, g_pubH = 0, g_pubPitch = 0;
static uint64_t        g_pubSeq = 0;       // bumped once per published game frame
static XrPosef         g_pubPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
static LONG            g_pubPoseValid = 0;
static uint64_t        g_conSeq = 0;       // last seq the worker uploaded into g_uploadTex
static XrPosef         g_conPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
static LONG            g_conPoseValid = 0;
// [FOVLATCH] The rendered FOV must travel WITH the image, for the same reason the pose does.
// Outlast changes its camera FOV constantly (DefaultFOV 90, RunningFOV 100, camcorder zoom to
// 15), and the worker re-submits a HELD image on the compositor frames where the game has not
// produced a new one. Reading the live FOV on those frames re-declares the same pixels at a
// different angle, so the image visibly resizes between one compositor frame and the next -
// seen in the log as eyeFov flipping 88.0x94.7 <-> 87.8x94.5 mid-session, and felt as flicker
// while moving. Inline mode never had this: capture and FOV read happened in one Present.
static float           g_pubFovX = 0.0f, g_pubFovY = 0.0f;
static float           g_conFovX = 0.0f, g_conFovY = 0.0f;
// [POSEFREEZE] The worker locates a new head pose 90x/second, but the GAME reads that pose
// mid-frame, at whatever moment each camera call happens. Two failures come straight out of
// that, and both are "jitter only while the head is moving":
//   - Outlast builds its two eye views one after the other, so with a live pose stream the two
//     eyes can render from DIFFERENT head poses - the pair no longer fuses cleanly.
//   - The image's true pose is then no single value, so whatever the submit declares is wrong
//     by a variable amount, and the compositor's reprojection error changes every frame.
// Cure: the game only ever sees a pose FROZEN for the whole frame. The worker writes into the
// pending slot; the game thread promotes pending -> live exactly once per frame, at publish,
// after tagging the finished image with the live value it was actually rendered with. Both
// eyes identical by construction, and declared == rendered exactly.
static XrPosef         g_pendPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
static float           g_pendYaw = 0.0f, g_pendPitch = 0.0f, g_pendRoll = 0.0f;
static LONG            g_pendValid = 0;
static bool RunXrFrame(IDirect3DDevice9* dev, bool preCaptured);
static DWORD WINAPI WorkerMain(LPVOID);
// DEFAULT 0.4 (2026-07-23), matching ME1 (vr_config.h headLookSmoothing=0.4). At 90 fps and
// stable frame time raw tracking was fine, but exclusive-fullscreen 4K runs slower and with more
// frame-time jitter, and raw pose then reads as "not smooth". This is the smoothing STRENGTH fed
// to a speed-adaptive quaternion filter below (not the old per-axis Euler EMA): heavy at rest,
// disengaging to raw on fast turns, so it adds no lag when you actually look around.
static volatile float g_smoothing = 0.4f;
// Frame-to-frame angle past which the filter fully disengages (ME1 kSnapSpeedDeg). A real head
// turn easily exceeds this, so turns are raw; a still head sits well under it, so jitter is
// filtered. Held per-eye-independent (one filter on the centre pose).
static const float kSnapDeg = 0.4f;
static XrQuaternionf g_smoothQuat = { 0,0,0,1 };
static bool g_smoothInit = false;
// [HEADPOS] Position is deliberately NOT smoothed. An earlier attempt (2026-08-17) ran it
// through this same filter to cure a reported clash with the rotation, and it read as worse;
// ME1's reference model filters the quaternion only and uses the raw view centre for lean.
extern "C" float OLXR_GetConvergence() { return g_convShift; }
extern "C" void  OLXR_SetConvergence(float v) {
    if (v < -3.0f) v = -3.0f; if (v > 3.0f) v = 3.0f;
    g_convShift = v;
}
extern "C" float OLXR_GetSmoothing() { return g_smoothing; }
extern "C" void  OLXR_SetSmoothing(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 0.9f) v = 0.9f;
    g_smoothing = v;
}

// CINEMA SCREEN. Menus (~33 deg/eye) and scripted/cutscene cameras (~58 deg/eye) render a much
// narrower FOV than gameplay (~88 deg/eye after the OLGame.ini levers). The projection layer
// must declare exactly what was rendered, so those frames honestly occupy a small box in the
// middle of a ~94 deg headset - the "menu is tiny" complaint. A QUAD makes no claim about world
// geometry, so it is the one place magnification is legitimate: below this threshold the two eye
// images move onto a pair of per-eye quads (one image per eye - stereo survives, and no
// side-by-side double image, which is what sank the reverted whole-backbuffer attempt) sized to
// fill the headset. Camcorder-up stays projection (88 deg > threshold); only a deliberate deep
// camcorder zoom crosses it, and a zoom that fills the view is what a zoom is for.
// Threshold is in rendered per-eye HORIZONTAL degrees; 0 disables. Hysteresis avoids flapping
// while FOVApproach sweeps through the boundary.
// DEFAULT OFF (2026-07-23). A flat 2D screen overlay breaks presence - it uses the same large
// screen the menu uses, and reads as looking through a flat screen rather than being IN the
// world, which is the goal even when the game renders a narrow frame. Worse, with the
// screen enabled it silently ATE the failure of the [CINEFOV] widening: the fix did nothing and
// the screen made that look deliberate. Off means every 3D frame goes down the projection path
// with real stereo and real head tracking. The slider stays for anyone who wants it.
static float         g_screenBelowDeg = 0.0f;
static volatile long g_screenOn = 0;
// WORLD-LOCKED screen anchor (2026-07-23). An earlier attempt hung the screen in VIEW space -
// head-locked - while the camera injection was frozen, so NOTHING on screen responded to the
// head at all: read as head tracking not working in cutscenes. A cinema screen has to be a THEATER
// screen: fixed in the room (LOCAL space), planted where you were looking the moment it
// engaged, upright (yaw only). Turning your head then pans across and off the screen exactly
// like looking around a room - that is what reads as head tracking during cutscenes.
static XrPosef       g_screenPose = { {0,0,0,1}, {0,0,0} };
static long          g_screenPoseValid = 0;
static XrQuaternionf g_lastLocalQ = { 0,0,0,1 };   // raw LOCAL head pose from the last locate
static XrVector3f    g_lastLocalP = { 0,0,0 };
static long          g_lastLocalValid = 0;
// Read by the camera hook (ol_ue3.cpp): head rotation must NOT be injected while the screen is
// up - panning the picture inside a head-locked screen is the "warpy cutscene" look. ME1's cine
// answer is a tracking-off 3D picture; the injection weight ramps there, never steps.
extern "C" int   OLXR_ScreenActive() { return g_screenOn ? 1 : 0; }
extern "C" float OLXR_GetScreenBelow() { return g_screenBelowDeg; }
extern "C" void  OLXR_SetScreenBelow(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 85.0f) v = 85.0f;
    g_screenBelowDeg = v;
}

// ---- [XRINPUT] motion controls: the OpenXR action set ------------------------------------------
// Step 1 proved the game reads a fabricated pad (empty slot 0, polled ~63/s, never gives up). This
// is the other half: read the VR controllers and fill that invented pad.
//
// THREADING, which is where this project keeps getting hurt: the submit worker owns EVERY OpenXR
// call, so xrSyncActions and the state reads happen THERE, right after xrWaitFrame (its predicted
// display time is what the pose locate wants). The game thread never touches OpenXR - it reads the
// published snapshot below under the same lock as the head pose. And the [POSEFREEZE] rule
// applies: the worker runs at 90 Hz against a ~60 fps game, so the game must see ONE coherent
// state per frame, not a value that changes mid-frame. g_padPub is written whole under the lock.
// OLXrPad / OLXR_BTN_* live in ol_xr.h - d3d9_proxy.cpp consumes them. A custom bitmask rather
// than XInput's, so the mapping to XInput lives in ONE table over there and a rebind never has
// to come back into this file.
static XrActionSet g_actionSet = 0;
static XrAction g_aMove = 0, g_aLook = 0, g_aTrig = 0, g_aGrip = 0, g_aPose = 0, g_aAim = 0;
static XrAction g_aClick = 0, g_aPrimary = 0, g_aSecondary = 0, g_aMenu = 0;
static XrPath   g_handPath[2] = { 0, 0 };
static XrSpace  g_handSpace[2] = { 0, 0 };
static XrSpace  g_aimSpace[2]  = { 0, 0 };

// ---- [HANDCAM] the camcorder is the camera, so aiming the camera IS aiming the camcorder -------
// Hold the controller up and you are looking
// through the camcorder you are holding. The mechanism is the one already proven for the head -
// ol_ue3 injects whatever is in g_olYawRad/Pitch/Roll and g_olHeadX/Y/Z, and ol_xr builds the
// DECLARED orientation from those same numbers. So handing the camcorder to the right hand needs
// no new camera code at all: while it is active, those globals carry the HAND's pose instead of
// the head's. Declared == rendered by construction, exactly as it is for head tracking.
// The AIM pose is used, not the grip pose: aim has a runtime-stable forward (-Z out of the
// controller, where you point it), which is what a camera lens follows. Grip is palm-relative and
// mirrored between hands.
// Hand tremor is real, so the hand orientation gets the same speed-adaptive filter the head uses,
// with its own state.
static volatile long  g_handCamOn = 1;        // [Input] HandCamcorder
static volatile long  g_handCamActive = 0;    // set by the grip, from the XInput hook
static volatile float g_handCamStrength = 1.0f;
static XrQuaternionf  g_handSmoothQ = { 0, 0, 0, 1 };
static bool           g_handSmoothInit = false;
static XrQuaternionf  g_handRelQ = { 0, 0, 0, 1 };   // aim orientation in the recenter frame
static XrVector3f     g_handRelP = { 0, 0, 0 };      // aim position in the recenter frame
static long           g_handRelValid = 0;
extern "C" void OLXR_SetHandCamActive(int on) { InterlockedExchange(&g_handCamActive, on ? 1 : 0); }
extern "C" int  OLXR_GetHandCam() { return g_handCamOn ? 1 : 0; }
extern "C" void OLXR_SetHandCam(int on) { g_handCamOn = on ? 1 : 0; }
extern "C" float OLXR_GetHandCamStrength() { return g_handCamStrength; }
extern "C" void  OLXR_SetHandCamStrength(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    g_handCamStrength = v;
}
static bool     g_inputReady = false;       // action set built AND attached
static long     g_inputEnabled = 1;         // [Input] MotionControls, ini
static OLXrPad  g_padPub = {};              // published snapshot, guarded by g_pubLock
static long     g_padPubValid = 0;
static long     g_inputLoggedBind = 0;

// The quaternion helpers live further down (they belong with the pose code); the hand-pose
// transform here needs them, so declare rather than reorder a working file.
static XrQuaternionf QNormalize(XrQuaternionf q);
static XrQuaternionf QConj(XrQuaternionf q);
static XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b);
static XrVector3f QRotate(XrQuaternionf q, XrVector3f v);

static XrPath XrPathOf(const char* s) {
    XrPath p = XR_NULL_PATH_VALUE;
    if (g_fn.stringToPath) g_fn.stringToPath(g_instance, s, &p);
    return p;
}

// Build the action set. Called once, after the session exists and BEFORE the first xrSyncActions:
// attach is a one-shot per session and every action must exist before it. A failure anywhere here
// is non-fatal by design - g_inputReady stays false, the pad publisher never runs, and the mod is
// exactly the build that works today.
static void BuildActionSet() {
    if (!g_inputEnabled || g_inputReady) return;
    if (!g_fn.createActionSet || !g_fn.createAction || !g_fn.attachSessionActionSets ||
        !g_fn.suggestInteractionProfileBindings || !g_fn.stringToPath) {
        XLOG("[OLVR][XRINPUT] this runtime did not provide the input entry points - motion controls off");
        return;
    }
    XrActionSetCreateInfo asci = {}; asci.type = XR_TYPE_ACTION_SET_CREATE_INFO_VALUE;
    strcpy_s(asci.actionSetName, "gameplay");
    strcpy_s(asci.localizedActionSetName, "Gameplay");
    asci.priority = 0;
    XrResult r = g_fn.createActionSet(g_instance, &asci, &g_actionSet);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XRINPUT] xrCreateActionSet r=%d", r); return; }

    g_handPath[0] = XrPathOf("/user/hand/left");
    g_handPath[1] = XrPathOf("/user/hand/right");

    // Every action is subaction-pathed to both hands, so one action serves left and right and the
    // state reads pick a side. Fewer actions, and the bindings stay symmetric.
    auto mk = [&](XrAction& out, XrActionType t, const char* name, const char* loc) -> bool {
        XrActionCreateInfo aci = {}; aci.type = XR_TYPE_ACTION_CREATE_INFO_VALUE;
        aci.actionType = t;
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, loc);
        aci.countSubactionPaths = 2; aci.subactionPaths = g_handPath;
        XrResult rr = g_fn.createAction(g_actionSet, &aci, &out);
        if (!XrSucceeded(rr)) { XLOG("[OLVR][XRINPUT] xrCreateAction('%s') r=%d", name, rr); return false; }
        return true;
    };
    bool ok = mk(g_aMove,      XR_ACTION_TYPE_VECTOR2F_INPUT_VALUE, "stick",      "Thumbstick")
           && mk(g_aTrig,      XR_ACTION_TYPE_FLOAT_INPUT_VALUE,    "trigger",    "Trigger")
           && mk(g_aGrip,      XR_ACTION_TYPE_FLOAT_INPUT_VALUE,    "grip",       "Grip")
           && mk(g_aClick,     XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE,  "stickclick", "Thumbstick Click")
           && mk(g_aPrimary,   XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE,  "primary",    "Primary Button")
           && mk(g_aSecondary, XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE,  "secondary",  "Secondary Button")
           && mk(g_aMenu,      XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE,  "menu",       "Menu Button")
           && mk(g_aPose,      XR_ACTION_TYPE_POSE_INPUT_VALUE,     "handpose",   "Hand Pose")
           && mk(g_aAim,       XR_ACTION_TYPE_POSE_INPUT_VALUE,     "aimpose",    "Aim Pose");
    if (!ok) return;
    g_aLook = g_aMove;   // one stick action; the right hand's copy IS the look stick

    struct Bind { XrAction a; const char* path; };
    auto suggest = [&](const char* profile, const Bind* b, int n) {
        XrPath prof = XrPathOf(profile);
        if (!prof) return;
        XrActionSuggestedBinding sb[24];
        int m = 0;
        for (int i = 0; i < n && m < 24; ++i) {
            XrPath p = XrPathOf(b[i].path);
            if (p) { sb[m].action = b[i].a; sb[m].binding = p; ++m; }
        }
        XrInteractionProfileSuggestedBinding ipb = {};
        ipb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING_VALUE;
        ipb.interactionProfile = prof; ipb.countSuggestedBindings = (uint32_t)m; ipb.suggestedBindings = sb;
        XrResult rr = g_fn.suggestInteractionProfileBindings(g_instance, &ipb);
        XLOG("[OLVR][XRINPUT] bindings %s -> r=%d (%d paths)", profile, rr, m);
    };

    // Oculus Touch: X/Y on the left controller, A/B on the right, menu on the left only.
    const Bind touch[] = {
        { g_aPose, "/user/hand/left/input/grip/pose" },   { g_aPose, "/user/hand/right/input/grip/pose" },
        { g_aAim,  "/user/hand/left/input/aim/pose" },    { g_aAim,  "/user/hand/right/input/aim/pose" },
        { g_aMove, "/user/hand/left/input/thumbstick" },  { g_aMove, "/user/hand/right/input/thumbstick" },
        { g_aClick,"/user/hand/left/input/thumbstick/click" }, { g_aClick,"/user/hand/right/input/thumbstick/click" },
        { g_aTrig, "/user/hand/left/input/trigger/value" }, { g_aTrig, "/user/hand/right/input/trigger/value" },
        { g_aGrip, "/user/hand/left/input/squeeze/value" }, { g_aGrip, "/user/hand/right/input/squeeze/value" },
        { g_aPrimary,   "/user/hand/left/input/x/click" }, { g_aPrimary,   "/user/hand/right/input/a/click" },
        { g_aSecondary, "/user/hand/left/input/y/click" }, { g_aSecondary, "/user/hand/right/input/b/click" },
        { g_aMenu, "/user/hand/left/input/menu/click" },
    };
    suggest("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch)/sizeof(touch[0])));

    // Index: A/B on both controllers, squeeze force is a separate path from the analog grip.
    const Bind index[] = {
        { g_aPose, "/user/hand/left/input/grip/pose" },   { g_aPose, "/user/hand/right/input/grip/pose" },
        { g_aAim,  "/user/hand/left/input/aim/pose" },    { g_aAim,  "/user/hand/right/input/aim/pose" },
        { g_aMove, "/user/hand/left/input/thumbstick" },  { g_aMove, "/user/hand/right/input/thumbstick" },
        { g_aClick,"/user/hand/left/input/thumbstick/click" }, { g_aClick,"/user/hand/right/input/thumbstick/click" },
        { g_aTrig, "/user/hand/left/input/trigger/value" }, { g_aTrig, "/user/hand/right/input/trigger/value" },
        { g_aGrip, "/user/hand/left/input/squeeze/value" }, { g_aGrip, "/user/hand/right/input/squeeze/value" },
        { g_aPrimary,   "/user/hand/left/input/a/click" }, { g_aPrimary,   "/user/hand/right/input/a/click" },
        { g_aSecondary, "/user/hand/left/input/b/click" }, { g_aSecondary, "/user/hand/right/input/b/click" },
        { g_aMenu, "/user/hand/left/input/system/click" },
    };
    suggest("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index)/sizeof(index[0])));

    // WMR: trackpad AND stick both exist; bind the stick, it is what people use.
    const Bind wmr[] = {
        { g_aPose, "/user/hand/left/input/grip/pose" },   { g_aPose, "/user/hand/right/input/grip/pose" },
        { g_aAim,  "/user/hand/left/input/aim/pose" },    { g_aAim,  "/user/hand/right/input/aim/pose" },
        { g_aMove, "/user/hand/left/input/thumbstick" },  { g_aMove, "/user/hand/right/input/thumbstick" },
        { g_aClick,"/user/hand/left/input/thumbstick/click" }, { g_aClick,"/user/hand/right/input/thumbstick/click" },
        { g_aTrig, "/user/hand/left/input/trigger/value" }, { g_aTrig, "/user/hand/right/input/trigger/value" },
        { g_aGrip, "/user/hand/left/input/squeeze/click" }, { g_aGrip, "/user/hand/right/input/squeeze/click" },
        { g_aMenu, "/user/hand/left/input/menu/click" },
    };
    suggest("/interaction_profiles/microsoft/motion_controller", wmr, (int)(sizeof(wmr)/sizeof(wmr[0])));

    // Vive wands: no thumbstick at all, the trackpad stands in for it.
    const Bind vive[] = {
        { g_aPose, "/user/hand/left/input/grip/pose" },   { g_aPose, "/user/hand/right/input/grip/pose" },
        { g_aAim,  "/user/hand/left/input/aim/pose" },    { g_aAim,  "/user/hand/right/input/aim/pose" },
        { g_aMove, "/user/hand/left/input/trackpad" },    { g_aMove, "/user/hand/right/input/trackpad" },
        { g_aClick,"/user/hand/left/input/trackpad/click" }, { g_aClick,"/user/hand/right/input/trackpad/click" },
        { g_aTrig, "/user/hand/left/input/trigger/value" }, { g_aTrig, "/user/hand/right/input/trigger/value" },
        { g_aGrip, "/user/hand/left/input/squeeze/click" }, { g_aGrip, "/user/hand/right/input/squeeze/click" },
        { g_aMenu, "/user/hand/left/input/menu/click" },
    };
    suggest("/interaction_profiles/htc/vive_controller", vive, (int)(sizeof(vive)/sizeof(vive[0])));

    // Simple controller: the fallback every runtime must support. Pose + select + menu only,
    // so an unknown controller still gets SOMETHING rather than a dead action set.
    const Bind simple[] = {
        { g_aPose, "/user/hand/left/input/grip/pose" },   { g_aPose, "/user/hand/right/input/grip/pose" },
        { g_aAim,  "/user/hand/left/input/aim/pose" },    { g_aAim,  "/user/hand/right/input/aim/pose" },
        { g_aTrig, "/user/hand/left/input/select/click" },{ g_aTrig, "/user/hand/right/input/select/click" },
        { g_aMenu, "/user/hand/left/input/menu/click" },
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple)/sizeof(simple[0])));

    // Pose spaces, two per hand: GRIP (where the hand is) and AIM (where it points). Non-fatal -
    // without them the sticks and buttons still work, only the hand-held camcorder is lost.
    if (g_fn.createActionSpace) {
        for (int i = 0; i < 2; ++i) {
            XrActionSpaceCreateInfo sp = {}; sp.type = XR_TYPE_ACTION_SPACE_CREATE_INFO_VALUE;
            sp.action = g_aPose; sp.subactionPath = g_handPath[i]; sp.poseInActionSpace = IdentityPose();
            if (!XrSucceeded(g_fn.createActionSpace(g_session, &sp, &g_handSpace[i]))) g_handSpace[i] = 0;
            XrActionSpaceCreateInfo ap = {}; ap.type = XR_TYPE_ACTION_SPACE_CREATE_INFO_VALUE;
            ap.action = g_aAim; ap.subactionPath = g_handPath[i]; ap.poseInActionSpace = IdentityPose();
            if (!XrSucceeded(g_fn.createActionSpace(g_session, &ap, &g_aimSpace[i]))) g_aimSpace[i] = 0;
        }
    }

    XrSessionActionSetsAttachInfo at = {}; at.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO_VALUE;
    at.countActionSets = 1; at.actionSets = &g_actionSet;
    r = g_fn.attachSessionActionSets(g_session, &at);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XRINPUT] xrAttachSessionActionSets r=%d - motion controls off", r); return; }
    g_inputReady = true;
    XLOG("[OLVR][XRINPUT] action set attached - VR controllers are live");
}

// Read one action's state. Every getter is null-checked; a runtime that only half-implements
// input degrades field by field instead of taking the session down.
static float ReadFloat(XrAction a, int hand) {
    if (!g_fn.getActionStateFloat) return 0.0f;
    XrActionStateGetInfo gi = {}; gi.type = XR_TYPE_ACTION_STATE_GET_INFO_VALUE;
    gi.action = a; gi.subactionPath = g_handPath[hand];
    XrActionStateFloat st = {}; st.type = XR_TYPE_ACTION_STATE_FLOAT_VALUE;
    if (!XrSucceeded(g_fn.getActionStateFloat(g_session, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}
static bool ReadBool(XrAction a, int hand) {
    if (!g_fn.getActionStateBoolean) return false;
    XrActionStateGetInfo gi = {}; gi.type = XR_TYPE_ACTION_STATE_GET_INFO_VALUE;
    gi.action = a; gi.subactionPath = g_handPath[hand];
    XrActionStateBoolean st = {}; st.type = XR_TYPE_ACTION_STATE_BOOLEAN_VALUE;
    if (!XrSucceeded(g_fn.getActionStateBoolean(g_session, &gi, &st)) || !st.isActive) return false;
    return st.currentState != 0;
}
static void ReadStick(XrAction a, int hand, float* x, float* y) {
    *x = 0.0f; *y = 0.0f;
    if (!g_fn.getActionStateVector2f) return;
    XrActionStateGetInfo gi = {}; gi.type = XR_TYPE_ACTION_STATE_GET_INFO_VALUE;
    gi.action = a; gi.subactionPath = g_handPath[hand];
    XrActionStateVector2f st = {}; st.type = XR_TYPE_ACTION_STATE_VECTOR2F_VALUE;
    if (!XrSucceeded(g_fn.getActionStateVector2f(g_session, &gi, &st)) || !st.isActive) return;
    *x = st.currentState.x; *y = st.currentState.y;
}

// Sync + publish. WORKER THREAD ONLY, once per xrWaitFrame. The whole struct is built locally
// and copied under the lock in one go, so the game thread can never see a half-updated pad.
static void SyncInput(XrTime displayTime) {
    if (!g_inputReady || !g_fn.syncActions) return;
    XrActiveActionSet aas = {}; aas.actionSet = g_actionSet; aas.subactionPath = XR_NULL_PATH_VALUE;
    XrActionsSyncInfo si = {}; si.type = XR_TYPE_ACTIONS_SYNC_INFO_VALUE;
    si.countActiveActionSets = 1; si.activeActionSets = &aas;
    const XrResult r = g_fn.syncActions(g_session, &si);
    // XR_SESSION_NOT_FOCUSED is normal and frequent (dashboard up, app in the background) - it
    // is not an error, it just means every action reads inactive this frame.
    if (!XrSucceeded(r)) return;

    OLXrPad p = {};
    ReadStick(g_aMove, 0, &p.moveX, &p.moveY);
    ReadStick(g_aLook, 1, &p.lookX, &p.lookY);
    p.trigL = ReadFloat(g_aTrig, 0); p.trigR = ReadFloat(g_aTrig, 1);
    p.gripL = ReadFloat(g_aGrip, 0); p.gripR = ReadFloat(g_aGrip, 1);
    if (ReadBool(g_aPrimary,   0)) p.buttons |= OLXR_BTN_X;   // left primary  = X
    if (ReadBool(g_aSecondary, 0)) p.buttons |= OLXR_BTN_Y;   // left secondary= Y
    if (ReadBool(g_aPrimary,   1)) p.buttons |= OLXR_BTN_A;   // right primary = A
    if (ReadBool(g_aSecondary, 1)) p.buttons |= OLXR_BTN_B;   // right secondary=B
    if (ReadBool(g_aClick, 0)) p.buttons |= OLXR_BTN_LSTICK;
    if (ReadBool(g_aClick, 1)) p.buttons |= OLXR_BTN_RSTICK;
    if (ReadBool(g_aMenu, 0) || ReadBool(g_aMenu, 1)) p.buttons |= OLXR_BTN_MENU;

    // Hand poses, expressed in the SAME recenter frame the head position uses, so anything built
    // on them later (gestures, hand-relative movement) shares one coordinate system with the lean.
    if (g_fn.locateSpace && g_centered) {
        for (int i = 0; i < 2; ++i) {
            if (!g_handSpace[i]) continue;
            XrSpaceLocation loc = {}; loc.type = XR_TYPE_SPACE_LOCATION_VALUE;
            if (!XrSucceeded(g_fn.locateSpace(g_handSpace[i], g_localSpace, displayTime, &loc))) continue;
            if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT_VALUE) ||
                !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT_VALUE)) continue;
            const XrVector3f d = { loc.pose.position.x - g_baseP.x,
                                   loc.pose.position.y - g_baseP.y,
                                   loc.pose.position.z - g_baseP.z };
            const XrVector3f rel = QRotate(QConj(g_baseQ), d);
            const XrQuaternionf q = QMul(QConj(g_baseQ), QNormalize(loc.pose.orientation));
            p.handPose[i].px = rel.x; p.handPose[i].py = rel.y; p.handPose[i].pz = rel.z;
            p.handPose[i].qx = q.x; p.handPose[i].qy = q.y; p.handPose[i].qz = q.z; p.handPose[i].qw = q.w;
            p.handValid |= (1u << i);
        }
    }

    // [HANDCAM] The right hand's AIM pose, in the recenter frame - the pose the camcorder takes
    // over when the grip is held. Captured here (worker, same display time as everything else)
    // and consumed by the pose block below, which is what publishes to the camera.
    g_handRelValid = 0;
    if (g_fn.locateSpace && g_centered && g_aimSpace[1]) {
        XrSpaceLocation loc = {}; loc.type = XR_TYPE_SPACE_LOCATION_VALUE;
        if (XrSucceeded(g_fn.locateSpace(g_aimSpace[1], g_localSpace, displayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT_VALUE) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT_VALUE)) {
            const XrVector3f d = { loc.pose.position.x - g_baseP.x,
                                   loc.pose.position.y - g_baseP.y,
                                   loc.pose.position.z - g_baseP.z };
            g_handRelP = QRotate(QConj(g_baseQ), d);
            g_handRelQ = QMul(QConj(g_baseQ), QNormalize(loc.pose.orientation));
            g_handRelValid = 1;
        }
    }

    AcquireSRWLockExclusive(&g_pubLock);
    g_padPub = p; g_padPubValid = 1;
    ReleaseSRWLockExclusive(&g_pubLock);

    if (!g_inputLoggedBind && (p.handValid || p.buttons || fabsf(p.moveX) > 0.1f ||
                               fabsf(p.moveY) > 0.1f || p.trigL > 0.1f || p.trigR > 0.1f)) {
        g_inputLoggedBind = 1;
        XLOG("[OLVR][XRINPUT] first live controller input seen (hands=%u buttons=0x%X move=%.2f,%.2f)",
             p.handValid, p.buttons, p.moveX, p.moveY);
    }
}

extern "C" int OLXR_MotionControlsReady() { return (g_inputReady && g_inputEnabled) ? 1 : 0; }
// Game-thread read of the published pad. One lock, whole-struct copy - never field by field, or
// the game gets a stick from one sync and a button from the next.
extern "C" int OLXR_GetPad(OLXrPad* out) {
    if (!out || !g_inputReady || !g_inputEnabled) return 0;
    AcquireSRWLockShared(&g_pubLock);
    const long ok = g_padPubValid;
    if (ok) *out = g_padPub;
    ReleaseSRWLockShared(&g_pubLock);
    return ok ? 1 : 0;
}

void OLXR_Recenter() {
    // Every caller of this function is the PLAYER (key, pad, menu button). The boot baseline
    // sets g_recenterReq directly and never comes through here - that is what keeps the two
    // apart for [HEADPOS].
    g_recenterManual = 1;
    g_recenterReq = 1; g_centered = 0; g_olPoseValid = 0;
    g_olYawRad = g_olPitchRad = g_olRollRad = 0.0f;
    g_olHeadX = g_olHeadY = g_olHeadZ = 0.0f;
    g_smoothInit = false;   // re-seed the smoothing filter from the new baseline, don't drift to it
}
// [HEADPOS] the runtime's own eye separation, metres (0 until the first locate). ol_ue3 derives
// the position scale from it so a real-world lean covers the distance the stereo makes it look.
extern "C" float OLXR_GetRtEyeSep();
// What the menu's info row reports: the resolution each eye actually gets and the FOV the engine
// actually rendered. Both are measured, never assumed.
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy);
extern "C" float OLUE3_GetHalfEye();
extern "C" float OLUE3_GetWorldToMeters();
// [WIDEFOV] The headset's own per-eye width, for ol_ue3's FOV widening. Degrees; 0 until the
// first locate has answered.
extern "C" void OLXR_GetHmdFovDeg(float* fx, float* fy) {
    if (fx) *fx = g_rtFovX * 57.2957795f;
    if (fy) *fy = g_rtFovY * 57.2957795f;
}

extern "C" void OLXR_GetEyeInfo(int* w, int* h, float* fovX, float* fovY) {
    if (w) *w = (int)g_eyeW;
    if (h) *h = (int)g_eyeH;
    float fx = 0.0f, fy = 0.0f;
    OLUE3_GetEyeFov(&fx, &fy);
    if (fovX) *fovX = fx * 57.2957795f;
    if (fovY) *fovY = fy * 57.2957795f;
}

void OLXR_ToggleProjection() {
    g_olUseProjection = g_olUseProjection ? 0 : 1;
    XLOG("[OLVR][XR] layer = %s", g_olUseProjection ? "PROJECTION" : "QUAD (head-locked)");
}

// ---- quaternion helpers ----
static XrQuaternionf QNormalize(XrQuaternionf q) {
    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (l > 1e-6f) { q.x/=l; q.y/=l; q.z/=l; q.w/=l; } else { q.x=q.y=q.z=0.0f; q.w=1.0f; }
    return q;
}
static XrQuaternionf QConj(XrQuaternionf q) { q.x=-q.x; q.y=-q.y; q.z=-q.z; return q; }
static XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return QNormalize(r);
}
// ROTATE A VECTOR. â›” This was built on QMul, and QMul NORMALIZES its result - correct for
// composing two orientations, catastrophic here: q*(v,0)*q' has norm |v|, so forcing it to unit
// length threw away the vector's LENGTH and returned pure direction. Every caller that rotated a
// unit vector (the forward vectors, the eye-right axis) was unharmed and it hid for a month; the
// two callers that rotated a REAL length were not:
//   - the head position ([HEADPOS]) came out as a 1.000 m vector EVERY FRAME. Measured in a
//     2026-08-17 run: 2998 of 3000 frames at exactly 1.000 m while stationary. That is the
//     whole story of five failed lean builds - an apparent hard limit at 1 m, wild shaking
//     after a recenter (near the baseline dp is millimetres of sensor noise, and
//     normalising noise swings a FULL METRE around), and the view snapping to face the
//     character or moving when looking down. No amount of scale or filtering can fix a unit
//     vector. The tuning was never the problem;
//   - the cinema screen was planted at 1 m instead of its intended 1.8 m.
// Rodrigues form below: no quaternion multiply, no normalisation, length preserved exactly.
static XrVector3f QRotate(XrQuaternionf q, XrVector3f v) {
    const float ux = q.x, uy = q.y, uz = q.z, s = q.w;
    const float dot = ux * v.x + uy * v.y + uz * v.z;
    const float uu  = ux * ux + uy * uy + uz * uz;
    const float cx = uy * v.z - uz * v.y;
    const float cy = uz * v.x - ux * v.z;
    const float cz = ux * v.y - uy * v.x;
    XrVector3f out;
    out.x = 2.0f * dot * ux + (s * s - uu) * v.x + 2.0f * s * cx;
    out.y = 2.0f * dot * uy + (s * s - uu) * v.y + 2.0f * s * cy;
    out.z = 2.0f * dot * uz + (s * s - uu) * v.z + 2.0f * s * cz;
    return out;
}
static float WrapPi(float a) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
// Normalized lerp between two quaternions, hemisphere-corrected. Cheaper than slerp and
// indistinguishable at the small per-frame steps a head filter takes.
static XrQuaternionf NlerpQuat(XrQuaternionf a, XrQuaternionf b, float t) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (d < 0.0f) { b.x=-b.x; b.y=-b.y; b.z=-b.z; b.w=-b.w; }   // shortest arc
    XrQuaternionf r = { a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
                        a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t };
    return QNormalize(r);
}
// Angle between two orientations, in degrees.
static float QuatAngleDeg(XrQuaternionf a, XrQuaternionf b) {
    float d = fabsf(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.2957795f;
}
static XrVector3f PoseCenter(const XrView* v) {
    XrVector3f p;
    p.x = (v[0].pose.position.x + v[1].pose.position.x) * 0.5f;
    p.y = (v[0].pose.position.y + v[1].pose.position.y) * 0.5f;
    p.z = (v[0].pose.position.z + v[1].pose.position.z) * 0.5f;
    return p;
}
static bool Resolve(const char* name, void** out) {
    PFN_xrVoidFunction fn = nullptr;
    XrResult r = g_getProc(g_instance, name, &fn);
    if (!XrSucceeded(r) || !fn) { XLOG("[OLVR][XR] resolve FAIL %s r=%d", name, r); return false; }
    *out = (void*)fn; return true;
}
#define RES(name, field) do { if (!Resolve(name, (void**)&g_fn.field)) { g_dead = true; return false; } } while(0)
// [XRINPUT] OPTIONAL resolve: a runtime that does not hand these over loses motion controls and
// nothing else. Using the fatal RES here would mean one missing input entry point takes down a
// stereo mod that has worked for a month.
#define RESOPT(name, field) do { if (!Resolve(name, (void**)&g_fn.field)) g_fn.field = nullptr; } while(0)

static DWORD NowMs() { return GetTickCount(); }

// Create a D3D11 device on the adapter OpenXR requires (matched by LUID).
static bool CreateD3D11OnAdapter(const LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { XLOG("[OLVR][XR] CreateDXGIFactory1 failed"); return false; }
    IDXGIAdapter1* chosen = nullptr; IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            chosen = a; char nm[160]; size_t n = 0; wcstombs_s(&n, nm, d.Description, _TRUNCATE);
            XLOG("[OLVR][XR] XR adapter matched: %s", nm);
            break;
        }
        a->Release(); a = nullptr;
    }
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, fls, _countof(fls), D3D11_SDK_VERSION, &g_d3d11, &got, &g_ctx);
    if (chosen) chosen->Release();
    factory->Release();
    if (FAILED(hr)) { XLOG("[OLVR][XR] D3D11CreateDevice hr=0x%08lX", hr); return false; }
    XLOG("[OLVR][XR] D3D11 bridge device created featureLevel=0x%X", got);
    return true;
}

// One-time OpenXR + D3D11 + swapchain bring-up, sized to the game backbuffer.
static bool Init(UINT w, UINT h) {
    g_w = w; g_h = h;

    // [STEAMVRSCENE] escape hatch for the transparency hunt: [XR] BlackSceneLayer=0 stands the
    // placeholder down entirely (never built, never submitted) so a single ini flip A/Bs the
    // "placeholder changes SteamVR's layer interpretation" suspect without a rebuild.
    // [APILAYER] Virtual Desktop installs an IMPLICIT OpenXR API layer (oculus-compatibility)
    // that injects into every OpenXR process on the machine, including this one on SteamVR,
    // and rewrites calls between this mod and the runtime. This mod is not an OculusXR-plugin
    // app, so the layer has no business in this session on ANY runtime - stand it down for this process
    // only (the env var is per-process; system-wide Virtual Desktop is untouched). Escape hatch:
    // [XR] AllowVdCompatLayer=1 puts it back.
    {
        wchar_t exe[MAX_PATH] = { 0 };
        bool allowVdLayer = false;
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            wchar_t* slash = wcsrchr(exe, L'\\');
            if (slash) {
                *(slash + 1) = 0;
                wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%soutlastvr.ini", exe);
                g_blackAllowed = GetPrivateProfileIntW(L"XR", L"BlackSceneLayer", 1, ini) != 0;
                if (!g_blackAllowed) XLOG("[OLVR][XRRUNTIME] black scene layer DISABLED by ini ([XR] BlackSceneLayer=0)");
                allowVdLayer = GetPrivateProfileIntW(L"XR", L"AllowVdCompatLayer", 0, ini) != 0;
                g_useWorker = GetPrivateProfileIntW(L"XR", L"SubmitThread", 1, ini) != 0;
                // -1 = AUTO: on for runtimes that reproject each view to its own eye point
                // (measured true of Oculus/Quest Link), off for the ones the single-centre-pose
                // model was tuned against (VDXR, SteamVR). 0/1 force it either way.
                g_inputEnabled = GetPrivateProfileIntW(L"Input", L"MotionControls", 1, ini) ? 1 : 0;
                g_handCamOn = GetPrivateProfileIntW(L"Input", L"HandCamcorder", 1, ini) ? 1 : 0;
                { wchar_t hv[32]; GetPrivateProfileStringW(L"Input", L"HandCamcorderStrength", L"1.0", hv, 32, ini);
                  OLXR_SetHandCamStrength((float)_wtof(hv)); }
                g_perEyeMode = (int)GetPrivateProfileIntW(L"XR", L"PerEyePose", (UINT)-1, ini);
                g_fitMode    = (int)GetPrivateProfileIntW(L"XR", L"FitRuntimeFrustum", (UINT)-1, ini);
                if (!g_useWorker) XLOG("[OLVR][SUBMITTHREAD] disabled by ini - submits ride the game's Present again");
            }
        }
        if (!allowVdLayer) {
            SetEnvironmentVariableW(L"DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY", L"1");
            XLOG("[OLVR][APILAYER] Virtual Desktop oculus-compatibility layer disabled for this process");
        } else {
            XLOG("[OLVR][APILAYER] Virtual Desktop oculus-compatibility layer left ENABLED by ini");
        }
    }

    g_loaderDll = LoadLibraryW(L"openxr_loader.dll");
    if (!g_loaderDll) { XLOG("[OLVR][XR] openxr_loader.dll not found (need the x64 one next to OLGame.exe) err=%lu", GetLastError()); g_dead = true; return false; }
    g_getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loaderDll, "xrGetInstanceProcAddr");
    if (!g_getProc) { XLOG("[OLVR][XR] no xrGetInstanceProcAddr"); g_dead = true; return false; }

    // [APILAYER] Log what is ACTUALLY between this process and the runtime. The registry
    // says what is installed; only the loader knows what it will load after disable variables
    // are honoured. Zero layers is the expected line; anything listed here is rewriting these
    // calls and belongs at the top of the suspect list for any runtime-specific weirdness.
    {
        typedef int32_t (OL_XRAPI *PFN_xrEnumerateApiLayerProperties)(uint32_t, uint32_t*, XrApiLayerProperties*);
        PFN_xrVoidFunction enumFn = nullptr;
        g_getProc(0, "xrEnumerateApiLayerProperties", &enumFn);
        if (enumFn) {
            auto enumLayers = (PFN_xrEnumerateApiLayerProperties)enumFn;
            uint32_t n = 0;
            if (XrSucceeded(enumLayers(0, &n, nullptr))) {
                XLOG("[OLVR][APILAYER] active OpenXR API layers in this process: %u", n);
                if (n > 0 && n < 16) {
                    std::vector<XrApiLayerProperties> props(n);
                    for (auto& p : props) { p = {}; p.type = XR_TYPE_API_LAYER_PROPERTIES_VALUE; }
                    if (XrSucceeded(enumLayers(n, &n, props.data())))
                        for (uint32_t i = 0; i < n; ++i)
                            XLOG("[OLVR][APILAYER]   %s v%u", props[i].layerName, props[i].layerVersion);
                }
            }
        }
    }

    PFN_xrVoidFunction createFn = nullptr;
    if (!XrSucceeded(g_getProc(0, "xrCreateInstance", &createFn)) || !createFn) { XLOG("[OLVR][XR] no xrCreateInstance"); g_dead = true; return false; }
    const char* exts[] = { kD3D11ExtensionName };
    XrInstanceCreateInfo ici = {}; ici.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    strcpy_s(ici.applicationInfo.applicationName, "OutlastVR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "OutlastVR");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = MakeXrVersion(1, 0, 0);
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    XrResult r = ((PFN_xrCreateInstance)createFn)(&ici, &g_instance);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XR] xrCreateInstance r=%d (no runtime?)", r); g_dead = true; return false; }
    XLOG("[OLVR][XR] instance created");

    RES("xrDestroyInstance", destroyInstance); RES("xrPollEvent", pollEvent);
    RES("xrGetInstanceProperties", getInstanceProperties);

    // [XRRUNTIME] Which runtime is this? Ported from ME1, where both quirks below were found the
    // hard way. Non-fatal: a failure just leaves both flags false and every path stays as it was.
    {
        XrInstanceProperties props = {}; props.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
        if (XrSucceeded(g_fn.getInstanceProperties(g_instance, &props))) {
            props.runtimeName[XR_MAX_RUNTIME_NAME_SIZE_VALUE - 1] = '\0';
            g_isOculusRuntime  = strstr(props.runtimeName, "Oculus") != nullptr ||
                                 strstr(props.runtimeName, "Meta")   != nullptr;
            g_isSteamVrRuntime = strstr(props.runtimeName, "SteamVR") != nullptr;
            XLOG("[OLVR][XRRUNTIME] name='%s' metaFovCrop=%d steamVrFovCrop=%d",
                 props.runtimeName, g_isOculusRuntime ? 1 : 0, g_isSteamVrRuntime ? 1 : 0);
            // Per-eye pose was MEASURED to change nothing on Quest (2026-08-05), so it goes back
            // to off by default everywhere - one variable at a time. The ini can still force it.
            g_perEyePose = (g_perEyeMode > 0);
            // [FITFRUSTUM] auto-on for Oculus: the runtime that ignores the declared FOV.
            g_fitFrustum = (g_fitMode < 0) ? g_isOculusRuntime : (g_fitMode != 0);
            XLOG("[OLVR][FITFRUSTUM] %s (%s), perEyePose=%s",
                 g_fitFrustum ? "ON - the picture is placed inside the runtime's own frustum" : "off",
                 g_fitMode < 0 ? "auto, by runtime" : "forced by ini",
                 g_perEyePose ? "on" : "off");
        } else {
            XLOG("[OLVR][XRRUNTIME] could not read the runtime name - both quirk fixes stay off");
        }
    }
    RES("xrGetSystem", getSystem); RES("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements);
    RES("xrCreateSession", createSession); RES("xrDestroySession", destroySession);
    RES("xrCreateSwapchain", createSwapchain); RES("xrDestroySwapchain", destroySwapchain); RES("xrEnumerateSwapchainImages", enumerateSwapchainImages);
    RES("xrAcquireSwapchainImage", acquireSwapchainImage); RES("xrWaitSwapchainImage", waitSwapchainImage);
    RES("xrReleaseSwapchainImage", releaseSwapchainImage); RES("xrBeginSession", beginSession);
    RES("xrEndSession", endSession); RES("xrWaitFrame", waitFrame);
    RES("xrBeginFrame", beginFrame); RES("xrEndFrame", endFrame);
    RES("xrCreateReferenceSpace", createReferenceSpace); RES("xrDestroySpace", destroySpace);
    RES("xrLocateViews", locateViews);
    // [XRINPUT] core OpenXR 1.0, so these should always resolve - but RESOPT, not RES: a missing
    // one costs motion controls only, never the stereo mod that has worked for a month.
    RESOPT("xrStringToPath", stringToPath); RESOPT("xrCreateActionSet", createActionSet);
    RESOPT("xrDestroyActionSet", destroyActionSet); RESOPT("xrCreateAction", createAction);
    RESOPT("xrDestroyAction", destroyAction);
    RESOPT("xrSuggestInteractionProfileBindings", suggestInteractionProfileBindings);
    RESOPT("xrAttachSessionActionSets", attachSessionActionSets); RESOPT("xrSyncActions", syncActions);
    RESOPT("xrGetActionStateBoolean", getActionStateBoolean); RESOPT("xrGetActionStateFloat", getActionStateFloat);
    RESOPT("xrGetActionStateVector2f", getActionStateVector2f);
    RESOPT("xrCreateActionSpace", createActionSpace); RESOPT("xrLocateSpace", locateSpace);

    XrSystemGetInfo sgi = {}; sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    r = g_fn.getSystem(g_instance, &sgi, &g_system);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XR] xrGetSystem r=%d (headset off?)", r); g_dead = true; return false; }

    XrGraphicsRequirementsD3D11KHR req = {}; req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    r = g_fn.getD3D11GraphicsRequirements(g_instance, g_system, &req);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XR] getD3D11GraphicsRequirements r=%d", r); g_dead = true; return false; }

    if (!CreateD3D11OnAdapter(req.adapterLuid)) { g_dead = true; return false; }

    XrGraphicsBindingD3D11KHR bind = {}; bind.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE; bind.device = g_d3d11;
    XrSessionCreateInfo sci = {}; sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE; sci.next = &bind; sci.systemId = g_system;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XR] xrCreateSession r=%d", r); g_dead = true; return false; }
    XLOG("[OLVR][XR] session created");

    XrReferenceSpaceCreateInfo rsci = {}; rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW_VALUE; rsci.poseInReferenceSpace = IdentityPose();
    if (!XrSucceeded(g_fn.createReferenceSpace(g_session, &rsci, &g_viewSpace))) { XLOG("[OLVR][XR] createReferenceSpace(view) failed"); g_dead = true; return false; }
    XrReferenceSpaceCreateInfo lsci = rsci; lsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    if (!XrSucceeded(g_fn.createReferenceSpace(g_session, &lsci, &g_localSpace))) { XLOG("[OLVR][XR] createReferenceSpace(local) failed"); g_dead = true; return false; }
    XrReferenceSpaceCreateInfo asci = lsci;
    if (!XrSucceeded(g_fn.createReferenceSpace(g_session, &asci, &g_appSpace))) { XLOG("[OLVR][XR] createReferenceSpace(app) failed"); g_dead = true; return false; }

    // [XRINPUT] Build the action set HERE: after the session and its spaces exist, and before any
    // frame runs. xrAttachSessionActionSets is one-shot per session and every action must already
    // exist when it is called, so there is no later opportunity.
    BuildActionSet();

    XrSwapchainCreateInfo scci = {}; scci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    scci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;   // matches the D3D9 A8R8G8B8 readback bitwise
    scci.sampleCount = 1; scci.width = w; scci.height = h; scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
    r = g_fn.createSwapchain(g_session, &scci, &g_swapchain);
    if (!XrSucceeded(r)) { XLOG("[OLVR][XR] xrCreateSwapchain r=%d %ux%u", r, w, h); g_dead = true; return false; }

    uint32_t imgCount = 0; g_fn.enumerateSwapchainImages(g_swapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount);
    for (auto& im : imgs) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
    r = g_fn.enumerateSwapchainImages(g_swapchain, imgCount, &imgCount, (XrSwapchainImageBaseHeader*)imgs.data());
    if (!XrSucceeded(r)) { XLOG("[OLVR][XR] enumerateSwapchainImages r=%d", r); g_dead = true; return false; }
    for (auto& im : imgs) g_xrImages.push_back(im.texture);
    XLOG("[OLVR][XR] swapchain %ux%u images=%u", w, h, imgCount);
    if (!g_xrImages.empty() && g_xrImages[0]) {
        D3D11_TEXTURE2D_DESC dd; g_xrImages[0]->GetDesc(&dd);
        XLOG("[OLVR][GAMMAFMT] mono swapchain: asked fmt=%d B8G8R8A8_UNORM_SRGB, runtime allocated fmt=%d%s",
             (int)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int)dd.Format,
             dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? " (exact match)" : " (substituted)");
    }

    D3D11_TEXTURE2D_DESC td = {}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d11->CreateTexture2D(&td, nullptr, &g_uploadTex))) { XLOG("[OLVR][XR] CreateTexture2D(upload) failed"); g_dead = true; return false; }

    XLOG("[OLVR][XR] init OK - waiting for session READY");
    if (g_useWorker) {
        g_workerStop = 0;
        g_workerThread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
        if (g_workerThread)
            XLOG("[OLVR][SUBMITTHREAD] compositor heartbeat thread running - the game's fps no longer gates the submit cadence");
        else {
            g_useWorker = false;
            XLOG("[OLVR][SUBMITTHREAD] CreateThread FAILED - falling back to inline submits");
        }
    }
    g_inited = true;
    return true;
}

// [XRSTATE] SteamVR transparency forensics: whether the compositor ever promotes this session
// to FOCUSED is the difference between the submitted layers being the scene, and the submitted
// layers being ghosted over the SteamVR environment - and until now only READY/STOPPING were
// ever logged, so that
// question has never had an answer in any log. Name every state, log every transition.
static const char* SessionStateName(int32_t s) {
    switch (s) {
        case 1: return "IDLE";
        case XR_SESSION_STATE_READY_VALUE:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED_VALUE: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE_VALUE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED_VALUE:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING_VALUE:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING_VALUE: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING_VALUE:      return "EXITING";
        default: return "?";
    }
}
static volatile LONG g_sessionState = 0;   // last state the runtime reported, for the periodic log
static volatile LONG g_lastEndFrame = 0;   // last xrEndFrame result, for the periodic log
static volatile DWORD g_lastPresentMs = 0; // [EXITGUARD] last time the game presented

static void PollEvents() {
    XrEventDataBuffer ev;
    for (;;) {
        ev = {}; ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        XrResult r = g_fn.pollEvent(g_instance, &ev);
        if (r == XR_EVENT_UNAVAILABLE_VALUE || !XrSucceeded(r)) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE) {
            auto* ss = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            InterlockedExchange(&g_sessionState, (LONG)ss->state);
            XLOG("[OLVR][XRSTATE] session state -> %d (%s)", ss->state, SessionStateName(ss->state));
            if (ss->state == XR_SESSION_STATE_READY_VALUE) {
                XrSessionBeginInfo sbi = {}; sbi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
                if (XrSucceeded(g_fn.beginSession(g_session, &sbi))) { g_running = true; XLOG("[OLVR][XR] session RUNNING"); }
            } else if (ss->state == XR_SESSION_STATE_STOPPING_VALUE) {
                g_fn.endSession(g_session); g_running = false; XLOG("[OLVR][XR] session STOPPING");
            } else if (ss->state == XR_SESSION_STATE_EXITING_VALUE || ss->state == XR_SESSION_STATE_LOSS_PENDING_VALUE) {
                g_running = false; g_dead = true; XLOG("[OLVR][XR] session EXITING/LOST");
            }
        } else {
            // Anything else the runtime says was previously dropped on the floor. Log each
            // distinct event type once - if SteamVR is trying to signal something about this
            // session (overlay demotion, reference-space change), this is where it would appear.
            static uint32_t s_seenTypes[8] = {};
            static int s_seenCount = 0;
            bool seen = false;
            for (int i = 0; i < s_seenCount; ++i) if (s_seenTypes[i] == ev.type) { seen = true; break; }
            if (!seen && s_seenCount < 8) {
                s_seenTypes[s_seenCount++] = ev.type;
                XLOG("[OLVR][XRSTATE] runtime event type=%u (first occurrence)", ev.type);
            }
        }
    }
}

// Outlast Resets its device after the intro (3840x2160 fullscreen -> 3633x2044 windowed was
// seen live). Everything sized from the backbuffer has to follow, or the copies silently
// mismatch and the headset goes black: UpdateSubresource writes short rows into a wider
// texture, and CopyResource between different-sized textures does nothing at all.
static void DestroyEyeSwapchains() {
    if (g_fn.destroySwapchain) {
        if (g_scL) g_fn.destroySwapchain(g_scL);
        if (g_scR) g_fn.destroySwapchain(g_scR);
    }
    g_scL = g_scR = 0; g_imgL.clear(); g_imgR.clear();
    g_eyeInitTried = false;
}

static bool RebuildForSize(UINT w, UINT h) {
    XLOG("[OLVR][XR] backbuffer changed %ux%u -> %ux%u, rebuilding swapchains", g_w, g_h, w, h);
    DestroyEyeSwapchains();
    if (g_swapchain && g_fn.destroySwapchain) g_fn.destroySwapchain(g_swapchain);
    g_swapchain = 0; g_xrImages.clear();
    if (g_uploadTex) { g_uploadTex->Release(); g_uploadTex = nullptr; }
    g_w = w; g_h = h;

    XrSwapchainCreateInfo ci = {}; ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    ci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &ci, &g_swapchain))) {
        XLOG("[OLVR][XR] mono swapchain rebuild FAILED %ux%u", w, h); g_swapchain = 0; return false;
    }
    uint32_t n = 0; g_fn.enumerateSwapchainImages(g_swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n);
    for (auto& im : imgs) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
    if (!XrSucceeded(g_fn.enumerateSwapchainImages(g_swapchain, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()))) return false;
    for (auto& im : imgs) g_xrImages.push_back(im.texture);

    D3D11_TEXTURE2D_DESC td = {}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d11->CreateTexture2D(&td, nullptr, &g_uploadTex))) {
        XLOG("[OLVR][XR] upload texture rebuild FAILED"); g_uploadTex = nullptr; return false;
    }
    XLOG("[OLVR][XR] rebuild OK: swapchain+upload now %ux%u", w, h);
    return true;
}

// Any D3D9 surface -> SYSTEMMEM -> g_uploadTex. SFR needs this for the saved left-eye RT as
// well as the backbuffer, so the readback is written once against an arbitrary source.
static bool CaptureSurface(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    if (!src || !g_uploadTex) return false;
    D3DSURFACE_DESC sd;
    if (FAILED(src->GetDesc(&sd))) return false;
    static UINT s_w = 0, s_h = 0; static D3DFORMAT s_fmt = D3DFMT_UNKNOWN;
    if (!g_sysSurf || s_w != sd.Width || s_h != sd.Height || s_fmt != sd.Format) {
        if (g_sysSurf) { g_sysSurf->Release(); g_sysSurf = nullptr; }
        if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &g_sysSurf, nullptr))) {
            s_w = sd.Width; s_h = sd.Height; s_fmt = sd.Format;
            XLOG("[OLVR][XR] readback surface %ux%u fmt=%d", sd.Width, sd.Height, (int)sd.Format);
        } else {
            g_sysSurf = nullptr;
            XLOG("[OLVR][XR] CreateOffscreenPlainSurface FAILED %ux%u fmt=%d", sd.Width, sd.Height, (int)sd.Format);
        }
    }
    if (!g_sysSurf || FAILED(dev->GetRenderTargetData(src, g_sysSurf))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    g_ctx->UpdateSubresource(g_uploadTex, 0, nullptr, lr.pBits, (UINT)lr.Pitch, 0);
    g_sysSurf->UnlockRect();
    return true;
}

static bool CaptureBackbuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    bool ok = CaptureSurface(dev, bb);
    bb->Release();
    return ok;
}

// [SUBMITTHREAD] Game-thread half of the bridge: D3D9 readback into a CPU buffer plus the pose
// this frame was rendered with. No D3D11, no OpenXR - those belong to the worker alone. The
// pose rides WITH the pixels so the worker can tag a held frame with the pose of the image it
// is actually showing; tagging a stale image with a fresh pose is the documented cause of
// noticeably shaky head tracking (an earlier attempt).
static bool PublishBackbuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC sd;
    if (FAILED(bb->GetDesc(&sd)) || sd.Width < 64 || sd.Height < 64) { bb->Release(); return false; }
    static UINT s_w = 0, s_h = 0; static D3DFORMAT s_fmt = D3DFMT_UNKNOWN;
    if (!g_sysSurf || s_w != sd.Width || s_h != sd.Height || s_fmt != sd.Format) {
        if (g_sysSurf) { g_sysSurf->Release(); g_sysSurf = nullptr; }
        if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &g_sysSurf, nullptr))) {
            s_w = sd.Width; s_h = sd.Height; s_fmt = sd.Format;
            XLOG("[OLVR][XR] readback surface %ux%u fmt=%d", sd.Width, sd.Height, (int)sd.Format);
        } else { g_sysSurf = nullptr; bb->Release(); return false; }
    }
    bool ok = false;
    if (SUCCEEDED(dev->GetRenderTargetData(bb, g_sysSurf))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            const UINT rowBytes = sd.Width * 4;
            AcquireSRWLockExclusive(&g_pubLock);
            g_pubPixels.resize((size_t)rowBytes * sd.Height);
            const uint8_t* src = (const uint8_t*)lr.pBits;
            for (UINT y = 0; y < sd.Height; ++y)
                memcpy(g_pubPixels.data() + (size_t)y * rowBytes, src + (size_t)y * lr.Pitch, rowBytes);
            g_pubW = sd.Width; g_pubH = sd.Height; g_pubPitch = rowBytes;
            // [POSEFREEZE] Tag the finished image with the pose it was ACTUALLY rendered with -
            // the frozen live value both eyes just used - then promote the pending pose so the
            // next frame renders frozen at the newest locate. Order matters: promote after
            // tagging, or the image carries a pose one frame newer than its own pixels.
            g_pubPose = g_headPoseApp; g_pubPoseValid = g_olPoseValid;
            OLUE3_GetEyeFov(&g_pubFovX, &g_pubFovY);   // [FOVLATCH] the angle THESE pixels cover
            ++g_pubSeq;
            if (g_pendValid) {
                g_headPoseApp = g_pendPose;
                g_olYawRad = g_pendYaw; g_olPitchRad = g_pendPitch; g_olRollRad = g_pendRoll;
                g_olHeadX = g_pendPose.position.x; g_olHeadY = g_pendPose.position.y;
                g_olHeadZ = g_pendPose.position.z;
                g_olPoseValid = 1;
            }
            ReleaseSRWLockExclusive(&g_pubLock);
            g_sysSurf->UnlockRect();
            ok = true;
        }
    }
    bb->Release();
    return ok;
}

// [SUBMITTHREAD] Worker-side half: adopt the newest published frame. Swapchain rebuilds happen
// HERE, from the published size - the worker owns every D3D11 and OpenXR object.
static void ConsumeUpload() {
    AcquireSRWLockShared(&g_pubLock);
    const uint64_t seq = g_pubSeq; const UINT w = g_pubW, h = g_pubH;
    ReleaseSRWLockShared(&g_pubLock);
    if (!seq || seq == g_conSeq) return;
    if (w != g_w || h != g_h) { if (!RebuildForSize(w, h)) return; }
    AcquireSRWLockShared(&g_pubLock);
    if (g_pubW == g_w && g_pubH == g_h && g_uploadTex && !g_pubPixels.empty()) {
        g_ctx->UpdateSubresource(g_uploadTex, 0, nullptr, g_pubPixels.data(), g_pubPitch, 0);
        g_conSeq = g_pubSeq; g_conPose = g_pubPose; g_conPoseValid = g_pubPoseValid;
        g_conFovX = g_pubFovX; g_conFovY = g_pubFovY;
    }
    ReleaseSRWLockShared(&g_pubLock);
}

extern "C" int  OLUE3_StereoMode();          // 0 none, 1 SBS split, 2 SFR, 3 AER
extern "C" int  OLUE3_HeadRollEnabled();     // one flag for camera AND declared pose (see below)
extern "C" int  OLUE3_GetEyeSwap();          // the Mass Effect model flips the routing with it too
extern "C" int  OLUE3_GetHeadPos();          // [HEADPOS] lean/duck switch, for the periodic log
extern "C" int  OLUE3_CurrentEye();          // AER: which eye this frame rendered (0=L, 1=R)
extern "C" IDirect3DSurface9* OLProxy_LeftEye();
extern "C" int  OLUE3_SplitActive();
extern "C" void OLUE3_FrameTick();
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy);

// One swapchain per eye. SBS packs both eyes into one frame, so each eye is half-width; SFR and
// AER render a full frame per eye, so each eye is the full backbuffer.
//
// The eye width therefore depends on the MODE, and the mode is switchable at runtime (NUM2/NUM4).
// Latching it once was a real bug: after a split->AER switch the swapchains kept the half width,
// and CopyResource between mismatched sizes silently does NOTHING - one eye freezes or goes black
// with no error anywhere. Rebuild whenever the mode changes.
static int g_eyeMode = -1;
static int g_eyeSlack = 0;          // convergence headroom currently baked into g_eyeW

// Slack the current convergence needs, in pixels, quantized to 32 so dragging the slider does
// not rebuild the swapchains at every notch. Capped so the eye always keeps 2/3 of its width.
static int WantConvSlackPx() {
    // [STEREOMODEL] the Mass Effect model reserves nothing: it samples with a clamping sampler,
    // so the eye keeps the full half at every convergence setting.
    if (g_meModel) return 0;
    const int halfW = (int)(g_w / 2);
    int need = (int)ceilf(fabsf(g_convShift) * kConvUnit * (float)halfW);
    if (need <= 0) return 0;
    need = ((need + 31) / 32) * 32;
    const int cap = halfW / 3;
    return need > cap ? cap : need;
}

static bool EnsureEyeSwapchains() {
    const int m = OLUE3_StereoMode();
    const int wantSlack = (m == 1) ? WantConvSlackPx() : 0;
    if (g_scL && g_scR) {
        if (m == g_eyeMode && wantSlack == g_eyeSlack) return true;
        XLOG("[OLVR][XR] stereo mode %d -> %d, rebuilding per-eye swapchains", g_eyeMode, m);
        DestroyEyeSwapchains();
        for (int e = 0; e < 2; ++e) { if (g_eyeHold[e]) { g_eyeHold[e]->Release(); g_eyeHold[e] = nullptr; } }
    }
    // The slack must be part of this guard too: a convergence-only change leaves the mode equal,
    // and without it the rebuild would be skipped and the eye width would silently disagree with
    // the copy box - which is the "one eye freezes" failure described above.
    if (g_eyeInitTried && m == g_eyeMode && wantSlack == g_eyeSlack) return false;
    g_eyeInitTried = true;
    g_eyeMode = m;
    // CONVERGENCE HEADROOM, only when it is being used. Each rendered half is exactly one eye
    // wide, so a convergence shift has nowhere to go: sliding the window pulls the OTHER eye's
    // pixels in at the edge, which is what made convergence feel broken. ME1 gets away with a
    // plain shift because it samples with a clamping sampler; this mod does box copies instead,
    // so the room is reserved ahead of time. At convergence 0 the slack is 0 and the eye gets the FULL half - no sharpness
    // is paid for a feature that is switched off. Crossing 0 rebuilds the swapchains once.
    g_eyeSlack = wantSlack;
    g_eyeW = (m == 1) ? (g_w / 2 - 2 * g_eyeSlack) : g_w; g_eyeH = g_h;
    XrSwapchainCreateInfo ci = {}; ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    ci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    ci.sampleCount = 1; ci.width = g_eyeW; ci.height = g_eyeH;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrSwapchain* scs[2] = { &g_scL, &g_scR };
    std::vector<ID3D11Texture2D*>* vecs[2] = { &g_imgL, &g_imgR };
    for (int e = 0; e < 2; ++e) {
        if (!XrSucceeded(g_fn.createSwapchain(g_session, &ci, scs[e]))) {
            XLOG("[OLVR][XR] eye swapchain %d create FAILED %ux%u", e, g_eyeW, g_eyeH);
            g_scL = g_scR = 0; return false;
        }
        uint32_t n = 0; g_fn.enumerateSwapchainImages(*scs[e], 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(n);
        for (auto& im : imgs) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
        if (!XrSucceeded(g_fn.enumerateSwapchainImages(*scs[e], n, &n, (XrSwapchainImageBaseHeader*)imgs.data()))) {
            g_scL = g_scR = 0; return false;
        }
        for (auto& im : imgs) vecs[e]->push_back(im.texture);
        // [GAMMAFMT] The format the runtime ACTUALLY backs the swapchain with decides whether
        // the compositor degammas these bytes on sampling. VDXR was measured substituting the
        // typeless family for the requested sRGB format and still looking right; if SteamVR resolves the
        // same bytes through a different view, that is one uniform gamma step - the "a little
        // dark" report. This line is the ground truth for that comparison.
        if (e == 0 && !imgs.empty() && imgs[0].texture) {
            D3D11_TEXTURE2D_DESC dd; imgs[0].texture->GetDesc(&dd);
            XLOG("[OLVR][GAMMAFMT] eye swapchain: asked fmt=%d B8G8R8A8_UNORM_SRGB, runtime allocated fmt=%d%s",
                 (int)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int)dd.Format,
                 dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? " (exact match)" : " (substituted)");
        }
    }
    XLOG("[OLVR][XR] per-eye swapchains ready %ux%u (SBS split detected)", g_eyeW, g_eyeH);
    return true;
}

// ---- [GAMMA] corrected blit ---------------------------------------------------------------
// "The colors are weird, the blacks are not good" (2026-07-23) has two real halves:
//  1. The game's brightness calibration goes through SetGammaRamp - a MONITOR output curve.
//     This capture reads the backbuffer BEFORE it, so the headset never saw that calibration.
//  2. The bytes are tagged sRGB for the compositor, but the game was authored against ~2.2
//     displays. The two curves differ exactly in the toe - the BLACKS.
// Fix = ME1's shape: a Gamma knob (ME1 m8_menu.cpp: 0.5..2.5, default 1.0) applied at the
// blit, with the captured game ramp folded in. Both live in one 256-entry LUT. The LUT stores
// LINEAR values (the sRGB render target re-encodes on write, so storing gamma bytes would
// double-encode - washed out), which also means identity-LUT == plain copy, bit for bit.
// When gamma == 1.0 and no ramp is set, the shader path is bypassed entirely and the blits
// stay the exact CopyResource calls of the confirmed baseline.
extern "C" int OLProxy_GammaRampGen();
extern "C" int OLProxy_GetGammaRamp(unsigned short* r, unsigned short* g, unsigned short* b);

// RANGE, and why it is not the obvious one (2026-08-03, after a live blinding). The old slider
// ran gamma 0.5 to 2.5, and gamma ABOVE 1 darkens. Outlast is already a nearly black game, so a
// small nudge upward crushed the whole picture, including this menu, to unreadable - and there
// was no way back that did not involve reading a slider you could no longer see.
// Two fixes: the dark half is capped where it is still a toe adjustment rather than a blackout,
// and the menu now presents this as BRIGHTNESS (1/gamma), so the slider moves the same way the
// picture does. An out-of-range value from an older ini clamps in here rather than being obeyed.
static const float kGammaMin = 0.50f;   // brightest
static const float kGammaMax = 1.20f;   // darkest that can still be seen through
static volatile float g_gamma = 1.0f;
extern "C" float OLXR_GetGamma() { return g_gamma; }
extern "C" void  OLXR_SetGamma(float v) {
    if (!(v > 0.0f) || !isfinite(v)) v = 1.0f;
    if (v < kGammaMin) v = kGammaMin; if (v > kGammaMax) v = kGammaMax;
    g_gamma = v;
}
extern "C" void  OLXR_ResetGamma() { g_gamma = 1.0f; }
extern "C" int OLXR_GammaRampApplied() { return OLProxy_GammaRampGen() ? 1 : 0; }

static ID3D11VertexShader*       g_blitVS = nullptr;
static ID3D11PixelShader*        g_blitPS = nullptr;
static ID3D11Buffer*             g_blitCB = nullptr;
static ID3D11SamplerState*       g_blitSamp = nullptr;
static ID3D11Texture2D*          g_lutTex = nullptr;
static ID3D11ShaderResourceView* g_lutSRV = nullptr;
static int   g_lutGen = -1;
static float g_lutGamma = -999.0f;
static bool  g_blitInitTried = false, g_blitReady = false;
static ID3D11BlendState*         g_blitBlend = nullptr;
static ID3D11DepthStencilState*  g_blitDepth = nullptr;
static ID3D11RasterizerState*    g_blitRaster = nullptr;
// The blit's own render target. This NEVER draws straight into an OpenXR swapchain image: every
// confirmed-good build here and in ME1/ME2 only ever COPIES into those, and the direct-render
// version scrambled the headset picture while every D3D call reported success.
static ID3D11Texture2D*          g_blitTmp = nullptr;
static ID3D11RenderTargetView*   g_blitTmpRtv = nullptr;
static UINT g_blitTmpW = 0, g_blitTmpH = 0;

static bool GammaActive() {
    return fabsf(g_gamma - 1.0f) > 0.005f || OLProxy_GammaRampGen() != 0;
}

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// clampU.xy bounds the sampled u inside this eye's OWN half of the backbuffer. The Mass Effect
// model slides the sampled window past the half's edge on purpose; clamping repeats the edge
// column instead of letting the left eye pull in the right eye's pixels at the seam. That clamp
// is exactly what lets the shift cost no resolution and reserve no slack.
static const char* kBlitHlsl =
"cbuffer CB : register(b0) { float4 box; float4 clampU; };\n"        // u0,v0,du,dv | minU,maxU
"struct VOut { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"VOut vs(uint id : SV_VertexID) {\n"
"  float2 t = float2((id << 1) & 2, id & 2);\n"                      // fullscreen triangle
"  VOut o; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"  o.uv = box.xy + t * box.zw; return o; }\n"
"Texture2D srcT : register(t0);\n"
"Texture2D lutT : register(t1);\n"
"SamplerState s0 : register(s0);\n"
"float4 ps(VOut i) : SV_Target {\n"
"  float2 uv = float2(clamp(i.uv.x, clampU.x, clampU.y), i.uv.y);\n"
"  float3 c = srcT.Sample(s0, uv).rgb;\n"                            // gamma-space bytes
"  float3 o;\n"
"  o.r = lutT.Sample(s0, float2((c.r * 255.0 + 0.5) / 256.0, 0.5)).r;\n"
"  o.g = lutT.Sample(s0, float2((c.g * 255.0 + 0.5) / 256.0, 0.5)).g;\n"
"  o.b = lutT.Sample(s0, float2((c.b * 255.0 + 0.5) / 256.0, 0.5)).b;\n"
"  return float4(o, 1.0); }\n";

static bool EnsureGammaPipeline() {
    if (g_blitReady) return true;
    if (g_blitInitTried) return false;
    g_blitInitTried = true;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { XLOG("[OLVR][GAMMA] d3dcompiler_47 unavailable - gamma disabled, plain copy"); return false; }
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        XLOG("[OLVR][GAMMA] shader compile FAILED%s%s", err ? ": " : "", err ? (const char*)err->GetBufferPointer() : "");
        if (vsb) vsb->Release(); if (psb) psb->Release(); if (err) err->Release();
        return false;
    }
    bool ok = SUCCEEDED(g_d3d11->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_blitVS)) &&
              SUCCEEDED(g_d3d11->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_blitPS));
    vsb->Release(); psb->Release();
    if (ok) {
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = SUCCEEDED(g_d3d11->CreateBuffer(&bd, nullptr, &g_blitCB));
    }
    if (ok) {
        D3D11_SAMPLER_DESC sdc = {}; sdc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sdc.AddressU = sdc.AddressV = sdc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ok = SUCCEEDED(g_d3d11->CreateSamplerState(&sdc, &g_blitSamp));
    }
    // Explicit pipeline state for the blit draw. The first version trusted context defaults and
    // the image came out scrambled; ME1's blit sets everything it depends on, so this does too.
    if (ok) {
        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].BlendEnable = FALSE;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = SUCCEEDED(g_d3d11->CreateBlendState(&bld, &g_blitBlend));
    }
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC dsd = {};                 // depth and stencil both off
        ok = SUCCEEDED(g_d3d11->CreateDepthStencilState(&dsd, &g_blitDepth));
    }
    if (ok) {
        D3D11_RASTERIZER_DESC rsd = {};
        rsd.FillMode = D3D11_FILL_SOLID; rsd.CullMode = D3D11_CULL_NONE; rsd.DepthClipEnable = TRUE;
        ok = SUCCEEDED(g_d3d11->CreateRasterizerState(&rsd, &g_blitRaster));
    }
    g_blitReady = ok;
    XLOG("[OLVR][GAMMA] corrected-blit pipeline %s", ok ? "ready" : "FAILED - plain copy");
    return ok;
}

static float SrgbToLinear(float s) {
    return (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
}

// LUT[i] = linear( pow(ramp[i], gamma) ). Rebuilt only when the slider or the game ramp moves.
static bool BuildGammaLut() {
    unsigned short rr[256], rg[256], rb[256];
    const int   gen = OLProxy_GetGammaRamp(rr, rg, rb);
    const float gam = g_gamma;
    if (g_lutSRV && gen == g_lutGen && fabsf(gam - g_lutGamma) < 0.001f) return true;
    float data[256 * 4];
    for (int i = 0; i < 256; ++i) {
        const float br = gen ? rr[i] / 65535.0f : i / 255.0f;
        const float bg = gen ? rg[i] / 65535.0f : i / 255.0f;
        const float bb = gen ? rb[i] / 65535.0f : i / 255.0f;
        data[i * 4 + 0] = SrgbToLinear(powf(br, gam));
        data[i * 4 + 1] = SrgbToLinear(powf(bg, gam));
        data[i * 4 + 2] = SrgbToLinear(powf(bb, gam));
        data[i * 4 + 3] = 1.0f;
    }
    if (g_lutSRV) { g_lutSRV->Release(); g_lutSRV = nullptr; }
    if (g_lutTex) { g_lutTex->Release(); g_lutTex = nullptr; }
    D3D11_TEXTURE2D_DESC td = {}; td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { data, 256 * 16, 0 };
    if (FAILED(g_d3d11->CreateTexture2D(&td, &sd, &g_lutTex)) ||
        FAILED(g_d3d11->CreateShaderResourceView(g_lutTex, nullptr, &g_lutSRV))) {
        XLOG("[OLVR][GAMMA] LUT create FAILED"); return false;
    }
    g_lutGen = gen; g_lutGamma = gam;
    XLOG("[OLVR][GAMMA] LUT rebuilt: gamma=%.2f ramp=%s", gam, gen ? "game" : "none");
    return true;
}

// The blit draw's private render target, recreated only when the requested size changes.
static bool EnsureBlitTmp(UINT w, UINT h) {
    if (g_blitTmp && g_blitTmpRtv && g_blitTmpW == w && g_blitTmpH == h) return true;
    if (g_blitTmpRtv) { g_blitTmpRtv->Release(); g_blitTmpRtv = nullptr; }
    if (g_blitTmp)    { g_blitTmp->Release();    g_blitTmp = nullptr; }
    D3D11_TEXTURE2D_DESC td = {}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_d3d11->CreateTexture2D(&td, nullptr, &g_blitTmp))) { g_blitTmp = nullptr; return false; }
    if (FAILED(g_d3d11->CreateRenderTargetView(g_blitTmp, nullptr, &g_blitTmpRtv))) {
        g_blitTmp->Release(); g_blitTmp = nullptr; g_blitTmpRtv = nullptr; return false;
    }
    g_blitTmpW = w; g_blitTmpH = h;
    return true;
}

// Draw src[box] -> a private target with the LUT applied, then COPY that into dst (the acquired
// swapchain image). ME1's shape exactly. boxX is SIGNED and fractional so the Mass Effect model
// can slide the window past the half's edge; clampLoPx/clampHiPx bound the sample to this eye's
// own half. Views on src are made per call - caching by texture pointer is a use-after-free trap
// when swapchains rebuild.
// dstVp* place the drawn image inside dst, and boxY offsets the source vertically. Both exist
// for [FITFRUSTUM]: when the rendered picture covers a NARROWER angle than the eye the runtime
// is compositing, it must sit at its true position inside that eye with black around it, not
// be stretched to fill. A zero-offset, full-size call is byte-identical to the old behaviour.
static bool GammaDraw(ID3D11Texture2D* src, UINT srcW, UINT srcH,
                      float boxX, UINT boxW, UINT boxH,
                      float clampLoPx, float clampHiPx,
                      ID3D11Texture2D* dst, UINT dstW, UINT dstH,
                      float boxY = 0.0f,
                      float dstVpX = 0.0f, float dstVpY = 0.0f,
                      float dstVpW = 0.0f, float dstVpH = 0.0f) {
    if (!EnsureBlitTmp(dstW, dstH)) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_d3d11->CreateShaderResourceView(src, nullptr, &srv))) return false;
    // Half-texel inset so linear filtering can never blend across the SBS seam.
    const float cb[8] = { boxX / (float)srcW, boxY / (float)srcH,
                          (float)boxW / (float)srcW, (float)boxH / (float)srcH,
                          (clampLoPx + 0.5f) / (float)srcW, (clampHiPx - 0.5f) / (float)srcW,
                          0.0f, 0.0f };
    g_ctx->UpdateSubresource(g_blitCB, 0, nullptr, cb, 0, 0);
    const bool placed = (dstVpW > 0.5f && dstVpH > 0.5f);
    D3D11_VIEWPORT vp = placed ? D3D11_VIEWPORT{ dstVpX, dstVpY, dstVpW, dstVpH, 0.0f, 1.0f }
                               : D3D11_VIEWPORT{ 0.0f, 0.0f, (float)dstW, (float)dstH, 0.0f, 1.0f };
    if (placed) {
        // Everything outside the placed picture must be opaque black, not last frame's pixels.
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_ctx->ClearRenderTargetView(g_blitTmpRtv, black);
    }
    const float blendFactor[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView* srvs[2] = { srv, g_lutSRV };
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_blitVS, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_blitCB);
    g_ctx->PSSetShader(g_blitPS, nullptr, 0);
    // The PS reads clampU from this same buffer, so it MUST be bound to the pixel stage as well.
    // Missing this line was the entire "scrambled horizontally" bug: an unbound PS constant buffer
    // reads zero, so clamp(u,0,0) pinned every sample to column 0.
    g_ctx->PSSetConstantBuffers(0, 1, &g_blitCB);
    g_ctx->PSSetShaderResources(0, 2, srvs);
    g_ctx->PSSetSamplers(0, 1, &g_blitSamp);
    g_ctx->OMSetBlendState(g_blitBlend, blendFactor, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_blitDepth, 0);
    g_ctx->OMSetRenderTargets(1, &g_blitTmpRtv, nullptr);
    g_ctx->RSSetState(g_blitRaster);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul[2] = { nullptr, nullptr };
    ID3D11RenderTargetView* nulrt = nullptr;
    g_ctx->PSSetShaderResources(0, 2, nul);
    g_ctx->OMSetRenderTargets(1, &nulrt, nullptr);
    srv->Release();
    g_ctx->CopyResource(dst, g_blitTmp);
    return true;
}

// True when the shader path can run at all.
static bool ShaderBlitReady() {
    return EnsureGammaPipeline() && BuildGammaLut();
}
static bool GammaBlitReady() {
    return GammaActive() && ShaderBlitReady();
}

// Copy one eye's window of the captured backbuffer into that eye's swapchain image.
// srcX is SIGNED and fractional so the Mass Effect model can slide it past the half's edge;
// clampLo/clampHi bound the sample inside that eye's own half. In the Outlast model the window
// is a plain in-bounds box and this is the baseline's exact CopySubresourceRegion.
static bool BlitHalfToEye(XrSwapchain sc, std::vector<ID3D11Texture2D*>& imgs,
                          float srcX, float clampLoPx, float clampHiPx) {
    if (!sc || imgs.empty()) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(sc, &ai, &idx))) return false;
    bool ok = false;
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(sc, &wi))) {
        const bool shifted = fabsf(srcX - clampLoPx) > 0.01f;
        // ALWAYS the shader when it is available, even with nothing to correct: it is the only
        // path that writes an OPAQUE alpha. A plain copy carries the game backbuffer alpha
        // through untouched, and a D3D9 backbuffer alpha is typically zero - SteamVR honours
        // that and composites the whole game as a see-through overlay on its own environment.
        if (ShaderBlitReady()) {
            ok = GammaDraw(g_uploadTex, g_w, g_h, srcX, g_eyeW, g_eyeH,
                           clampLoPx, clampHiPx, imgs[idx], g_eyeW, g_eyeH);
        }
        if (!ok) {
            // Degrade locally, never silently: drop the shift for this eye rather than failing
            // the whole submit (a failed eye used to collapse the entire frame to mono).
            if (shifted) {
                static bool warned = false;
                if (!warned) { warned = true;
                    XLOG("[OLVR][XR] convergence shift needs the shader blit - unavailable, "
                         "copying the unshifted window instead"); }
            }
            const UINT x0 = (UINT)clampLoPx;
            D3D11_BOX box = { x0, 0, 0, x0 + g_eyeW, g_eyeH, 1 };
            g_ctx->CopySubresourceRegion(imgs[idx], 0, 0, 0, 0, g_uploadTex, 0, &box);
            ok = true;
        }
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(sc, &ri);
    return ok;
}

// [FITFRUSTUM] Place one eye's rendered picture at its TRUE angular position inside an image
// that represents the RUNTIME's own frustum, black everywhere else.
//
// Why this exists: the Oculus/Quest Link runtime composites using its own per-eye frustum and
// ignores a differing declared FOV (ME1's [LINKFOV] finding, re-measured here 2026-08-05:
// rtFov e0 -54.0/+40.0 against this mod's symmetric +/-44). Declaring an intersection cannot help
// when the declaration is discarded - the submitted rect just gets stretched across the runtime's
// frustum instead, which displaces each eye ~5 deg in OPPOSITE directions and is exactly the
// unfusable doubling. Here the submitted image already spans the runtime's frustum, so the
// result is right whether the runtime honours the declaration or throws it away.
//
// srcX0/srcX1 bound this eye's half of the backbuffer (already carrying the convergence shift),
// ourFovX/Y are the angles that half actually covers, and rt is the eye the runtime wants.
static bool BlitFitToEye(XrSwapchain sc, std::vector<ID3D11Texture2D*>& imgs,
                         float srcX0, float srcX1, float clampLoPx, float clampHiPx,
                         float ourFovX, float ourFovY, const XrFovf& rt) {
    if (!sc || imgs.empty()) return false;
    const float tanTL = tanf(rt.angleLeft), tanTR = tanf(rt.angleRight);
    const float tanTU = tanf(rt.angleUp),   tanTD = tanf(rt.angleDown);
    const float tanOL = tanf(-ourFovX * 0.5f), tanOR = tanf(ourFovX * 0.5f);
    const float tanOU = tanf(ourFovY * 0.5f),  tanOD = tanf(-ourFovY * 0.5f);
    const float spanTH = tanTR - tanTL, spanTV = tanTU - tanTD;
    const float spanOH = tanOR - tanOL, spanOV = tanOU - tanOD;
    if (spanTH < 1e-4f || spanTV < 1e-4f || spanOH < 1e-4f || spanOV < 1e-4f) return false;
    // The overlap of the rendered picture with what the eye covers.
    const float iL = (tanOL > tanTL) ? tanOL : tanTL, iR = (tanOR < tanTR) ? tanOR : tanTR;
    const float iU = (tanOU < tanTU) ? tanOU : tanTU, iD = (tanOD > tanTD) ? tanOD : tanTD;
    if (iR - iL < 1e-4f || iU - iD < 1e-4f) return false;
    // Where that overlap lands in the runtime's eye image...
    float dx0 = (iL - tanTL) / spanTH * (float)g_eyeW, dx1 = (iR - tanTL) / spanTH * (float)g_eyeW;
    float dy0 = (tanTU - iU) / spanTV * (float)g_eyeH, dy1 = (tanTU - iD) / spanTV * (float)g_eyeH;
    // ...and which part of the source picture it comes from.
    const float srcW = srcX1 - srcX0;
    const float sx0 = srcX0 + (iL - tanOL) / spanOH * srcW;
    const float sx1 = srcX0 + (iR - tanOL) / spanOH * srcW;
    const float sy0 = (tanOU - iU) / spanOV * (float)g_h;
    const float sy1 = (tanOU - iD) / spanOV * (float)g_h;
    if (dx1 - dx0 < 1.0f || dy1 - dy0 < 1.0f || sx1 - sx0 < 1.0f || sy1 - sy0 < 1.0f) return false;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(sc, &ai, &idx))) return false;
    bool ok = false;
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(sc, &wi)) && ShaderBlitReady()) {
        ok = GammaDraw(g_uploadTex, g_w, g_h, sx0, (UINT)(sx1 - sx0), (UINT)(sy1 - sy0),
                       clampLoPx, clampHiPx, imgs[idx], g_eyeW, g_eyeH,
                       sy0, dx0, dy0, dx1 - dx0, dy1 - dy0);
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(sc, &ri);
    return ok;
}

// [STEAMVRSCENE] Build the black placeholder once, on SteamVR only. Every image in the chain is
// cleared to opaque black at creation, and the layer is re-submitted without ever being touched
// again, so this costs one 64x64 swapchain and nothing per frame.
static bool EnsureBlackScene() {
    if (g_blackSc) return true;
    if (g_blackTried) return false;
    g_blackTried = true;
    XrSwapchainCreateInfo ci = {}; ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    ci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    ci.sampleCount = 1; ci.width = kBlackSceneSize; ci.height = kBlackSceneSize;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &ci, &g_blackSc))) {
        g_blackSc = 0; XLOG("[OLVR][XRRUNTIME] black scene swapchain create FAILED"); return false;
    }
    uint32_t n = 0; g_fn.enumerateSwapchainImages(g_blackSc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n);
    for (auto& im : imgs) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
    if (!XrSucceeded(g_fn.enumerateSwapchainImages(g_blackSc, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()))) {
        g_blackSc = 0; return false;
    }
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (auto& im : imgs) {
        g_blackImgs.push_back(im.texture);
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(g_d3d11->CreateRenderTargetView(im.texture, nullptr, &rtv)) && rtv) {
            g_ctx->ClearRenderTargetView(rtv, black);
            rtv->Release();
        }
    }
    // One acquire/release cycle so the runtime treats the chain as having valid content.
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (XrSucceeded(g_fn.acquireSwapchainImage(g_blackSc, &ai, &idx))) {
        XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
        wi.timeout = XR_INFINITE_DURATION_VALUE;
        g_fn.waitSwapchainImage(g_blackSc, &wi);
        XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
        g_fn.releaseSwapchainImage(g_blackSc, &ri);
    }
    XLOG("[OLVR][XRRUNTIME] black scene layer ready (%d px, SteamVR only)", kBlackSceneSize);
    return true;
}

static bool EnsureEyeHold() {
    for (int e = 0; e < 2; ++e) {
        if (g_eyeHold[e]) continue;
        D3D11_TEXTURE2D_DESC td = {}; td.Width = g_eyeW; td.Height = g_eyeH;
        td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3d11->CreateTexture2D(&td, nullptr, &g_eyeHold[e]))) { g_eyeHold[e] = nullptr; return false; }
    }
    return true;
}

static bool BlitHoldToEye(XrSwapchain sc, std::vector<ID3D11Texture2D*>& imgs, ID3D11Texture2D* src) {
    if (!sc || imgs.empty() || !src) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(sc, &ai, &idx))) return false;
    bool ok = false;
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(sc, &wi))) {
        if (ShaderBlitReady()) ok = GammaDraw(src, g_eyeW, g_eyeH, 0.0f, g_eyeW, g_eyeH, 0.0f, (float)g_eyeW, imgs[idx], g_eyeW, g_eyeH);   // opaque alpha
        else { g_ctx->CopyResource(imgs[idx], src); ok = true; }
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(sc, &ri);
    return ok;
}

static bool BlitToSwapchain() {
    if (!g_swapchain || g_xrImages.empty()) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_swapchain, &ai, &idx))) return false;
    bool copied = false;
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(g_swapchain, &wi))) {
        if (ShaderBlitReady()) {   // opaque alpha, see BlitHalfToEye
            copied = GammaDraw(g_uploadTex, g_w, g_h, 0.0f, g_w, g_h, 0.0f, (float)g_w, g_xrImages[idx], g_w, g_h);
        } else {
            g_ctx->CopyResource(g_xrImages[idx], g_uploadTex);   // UNORM -> UNORM_SRGB, same typeless family
            copied = true;
        }
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_swapchain, &ri);
    return copied;
}

void OLXR_OnPresent(IDirect3DDevice9* dev) {
    if (g_dead || !dev) return;
    g_lastPresentMs = NowMs();   // [EXITGUARD] heartbeat the worker watches
    if (!g_inited) {
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        D3DSURFACE_DESC sd; bb->GetDesc(&sd); bb->Release();
        if (sd.Width < 64 || sd.Height < 64) return;    // not a real scene yet
        if (!Init(sd.Width, sd.Height)) return;
    }

    // [SUBMITTHREAD] With the worker on, the game thread's whole job is publishing frames; the
    // worker owns events, pacing and every XR call. Publishing is gated on the session running
    // so a headset on standby does not pay the readback for nothing.
    if (g_useWorker) {
        if (g_running) PublishBackbuffer(dev);
        OLUE3_FrameTick();   // once per GAME frame - see the note at the old call site
        return;
    }

    PollEvents();
    if (!g_running) return;

    // Follow the live backbuffer size. Cheap, and it is the difference between a picture and
    // a black headset the moment the game changes resolution.
    {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC sd;
            if (SUCCEEDED(bb->GetDesc(&sd)) && sd.Width >= 64 && sd.Height >= 64 &&
                (sd.Width != g_w || sd.Height != g_h)) {
                bb->Release();
                if (!RebuildForSize(sd.Width, sd.Height)) return;
            } else bb->Release();
        }
    }

    // [PACING] Capture the backbuffer BEFORE xrWaitFrame. GetRenderTargetData drains the whole
    // GPU pipeline behind the just-rendered 4K frame, and the lock+upload moves ~33MB on the CPU.
    // Doing that between waitFrame and endFrame put it inside the compositor's armed deadline;
    // SteamVR booked the overruns as frame timeouts (903 episodes in one 30s session) and held
    // the game cross-faded into its backdrop - the "transparent over the environment" look. The
    // work is identical out here; only the deadline exposure changes. Mode 2 (SFR) re-captures
    // inside the window on purpose: its left-eye upload overwrites g_uploadTex, so the right eye
    // must take the backbuffer again after it.
    const bool preCaptured = CaptureBackbuffer(dev);
    RunXrFrame(dev, preCaptured);
    OLUE3_FrameTick();   // once per Present, exactly as before the worker existed
}

// One full compositor frame: waitFrame -> pose -> layers -> endFrame. Called from the game's
// Present in inline mode (dev = the D3D9 device) or from the worker's loop (dev = null; the
// image is whatever ConsumeUpload last put in g_uploadTex, and the pose tag is the one that
// was published WITH that image). The SFR/AER lanes need the live device and quietly stand
// down under the worker - both are dev-only research lanes, mode split is what ships.
static bool RunXrFrame(IDirect3DDevice9* dev, bool preCaptured) {
    XrFrameWaitInfo fwi = {}; fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    XrFrameState fs = {}; fs.type = XR_TYPE_FRAME_STATE_VALUE;
    if (!XrSucceeded(g_fn.waitFrame(g_session, &fwi, &fs))) return false;

    XrFrameBeginInfo fbi = {}; fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    g_fn.beginFrame(g_session, &fbi);

    // [XRINPUT] Controllers, on the worker, once per frame, at the same predicted display time
    // the head pose uses - so hands and head are sampled for the same instant. This is the ONLY
    // place actions are synced; the game thread just reads the published snapshot.
    SyncInput(fs.predictedDisplayTime);

    // POSE (2026-07-23), both earlier shapes falsified LIVE, keep this history:
    //  - Latch the render pose, predict for the CURRENT display: declared sits a full frame
    //    behind the display pose, the compositor timewarps a frame of yaw per turn - reads as
    //    the view warping on every head turn (attempt 1).
    //  - Declare the FRESH pose over a frame-old image (the literal ME1 read): every pacing
    //    hiccup between game fps and headset refresh shows raw - reads as noticeably shaky
    //    head tracking (attempt 2).
    // The actual fix is PREDICTION, not freshness: the locate below asks for the head pose one
    // display period AHEAD - the time the frame the camera is about to render will actually hit
    // the screen. The camera renders with it; THIS latch carries it to the submit one Present
    // later, when that frame is displayed. Declared == rendered again (no shake), and the
    // declared pose targets the right instant (no systematic timewarp, no warp on turns). What
    // remains for the compositor is prediction error, which is what timewarp is FOR.
    // [SUBMITTHREAD] Under the worker the image being submitted is the last PUBLISHED game
    // frame, so its tag is the pose that travelled with it - not the latest locate, which may
    // be several compositor frames newer than the pixels.
    const XrPosef poseForThisImage = dev ? g_headPoseApp : g_conPose;
    const long    poseValidForThisImage = dev ? g_olPoseValid : g_conPoseValid;

    // ---- head pose: software baseline from LOCAL, current-minus-baseline thereafter ----
    if (g_recenterReq) {
        XrViewLocateInfo li = {}; li.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        li.displayTime = fs.predictedDisplayTime; li.space = g_localSpace;
        XrViewState ls = {}; ls.type = XR_TYPE_VIEW_STATE_VALUE;
        XrView lv[2] = {}; lv[0].type = XR_TYPE_VIEW_VALUE; lv[1].type = XR_TYPE_VIEW_VALUE; uint32_t ln = 0;
        if (XrSucceeded(g_fn.locateViews(g_session, &li, &ls, 2, &ln, lv)) &&
            (ls.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE)) {
            // YAW ONLY. Capturing the full orientation makes "forward" mean whatever tilt your
            // head happened to have at that instant, and every later pose is measured against
            // that tilted frame - so the horizon is permanently off and the world reads as
            // "not straight". It bites hardest on the automatic baseline at startup, which
            // fires while the headset is still being put on. Pitch and roll must stay absolute
            // so the world stays gravity-aligned; only the direction you face is adopted.
            // Extraction is the round trip already proven in this file (see the cinema-screen
            // anchor): rotating (0,0,-1) by +theta lands at (-sin,0,-cos), so
            // atan2(-fwd.x,-fwd.z) recovers +theta and the quaternion takes +yaw/2.
            const XrQuaternionf ql = QNormalize(lv[0].pose.orientation);
            const XrVector3f fwd = QRotate(ql, { 0.0f, 0.0f, -1.0f });
            const float horiz = fwd.x * fwd.x + fwd.z * fwd.z;
            if (horiz > 1e-6f) {
                const float yaw = atan2f(-fwd.x, -fwd.z);
                g_baseQ = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
            }   // straight up or down: yaw is meaningless there, keep the previous facing
            g_baseP = PoseCenter(lv);
            g_recenterReq = 0; g_centered = 1; g_olPoseValid = 0;
            // [HEADPOS] A player-requested recenter is a trusted position zero. The boot one is
            // not: it starts the pickup-then-still watch instead, and position stays inert
            // until that (or a G press) arms it.
            const bool manual = g_recenterManual != 0;
            g_recenterManual = 0;
            if (manual) {
                g_posArmed = 1; g_posAutoDone = 1;
            } else {
                g_posArmed = 0; g_posPickedUp = 0; g_settleSince = 0;
            }
            XLOG("[OLVR][XR] recenter: baseline captured (yaw only - the horizon stays level), "
                 "position %s", manual ? "armed (player recenter)"
                                       : "waiting - arms on the first still pose after the headset is picked up, or on recenter");
        }
    } else if (g_centered) {
        XrViewLocateInfo vli = {}; vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        // One period AHEAD: this pose is consumed by the camera for the frame rendered during
        // the NEXT interval, displayed at roughly the next Present. See the pose note above.
        vli.displayTime = fs.predictedDisplayTime + fs.predictedDisplayPeriod;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        XrView av[2] = {}; av[0].type = XR_TYPE_VIEW_VALUE; av[1].type = XR_TYPE_VIEW_VALUE; uint32_t nv = 0;
        if (XrSucceeded(g_fn.locateViews(g_session, &vli, &vs, 2, &nv, av)) &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE)) {
            // Raw LOCAL-space head pose, kept for anchoring the cinema screen in the world.
            g_lastLocalQ = QNormalize(av[0].pose.orientation);
            g_lastLocalP = PoseCenter(av);
            g_lastLocalValid = 1;
            XrQuaternionf qraw = QMul(QConj(g_baseQ), QNormalize(av[0].pose.orientation));
            XrVector3f c = PoseCenter(av);
            // [HEADPOS] Automatic arming, once: pickup (moved > kPickupM from the boot
            // baseline) then still (within kStillM of one spot for kStillNs). Position zero
            // becomes that still spot. Only the position baseline moves - g_baseQ (yaw) is
            // left exactly as the boot recenter took it, so nothing rotates under the player.
            if (!g_posArmed && !g_posAutoDone) {
                const float bx = c.x - g_baseP.x, by = c.y - g_baseP.y, bz = c.z - g_baseP.z;
                if (!g_posPickedUp) {
                    if (bx * bx + by * by + bz * bz > kPickupM * kPickupM) {
                        g_posPickedUp = 1; g_settleRef = c; g_settleSince = vli.displayTime;
                        XLOG("[OLVR][HEADPOS] headset moved off the boot baseline - watching for it to hold still");
                    }
                } else {
                    const float sx = c.x - g_settleRef.x, sy = c.y - g_settleRef.y, sz = c.z - g_settleRef.z;
                    if (sx * sx + sy * sy + sz * sz > kStillM * kStillM) {
                        g_settleRef = c; g_settleSince = vli.displayTime;   // still moving: restart
                    } else if (vli.displayTime - g_settleSince >= kStillNs) {
                        g_baseP = c;
                        g_posArmed = 1; g_posAutoDone = 1;
                        XLOG("[OLVR][HEADPOS] held still - position baseline armed automatically "
                             "(recenter any time to re-take it)");
                    }
                }
            }
            XrVector3f dp = { c.x - g_baseP.x, c.y - g_baseP.y, c.z - g_baseP.z };
            XrVector3f relP = QRotate(QConj(g_baseQ), dp);
            // SPEED-ADAPTIVE QUATERNION SMOOTHING (ME1's model). Filter ONE quaternion here,
            // upstream of the angle extraction, so both the render angles and the declared tag
            // come from the same smoothed value (a per-axis Euler filter, the old approach,
            // desynced render from tag - ME1's own history documents that bug). The follow factor
            // ramps from heavy-at-rest to 1.0 (raw) once the frame delta passes kSnapDeg, so a
            // real turn has zero added lag while a still head stops jittering.
            // [HEADPOS] Position is NOT filtered - ME1's model exactly (xr_session.cpp: headPos is
            // the raw view centre, only the quaternion is smoothed). Filtering it (tried
            // 2026-08-17, read as worse) lags the lean behind the real head.
            if (!g_smoothInit) { g_smoothQuat = qraw; g_smoothInit = true; }
            if (g_smoothing > 0.001f) {
                const float baseFollow = 1.0f - g_smoothing * 0.95f;
                float t = QuatAngleDeg(g_smoothQuat, qraw) / kSnapDeg;
                if (t > 1.0f) t = 1.0f;
                g_smoothQuat = NlerpQuat(g_smoothQuat, qraw, baseFollow + (1.0f - baseFollow) * t);
            } else {
                g_smoothQuat = qraw;
            }
            XrQuaternionf q = g_smoothQuat;

            // [HANDCAM] HAND THE CAMERA TO THE RIGHT CONTROLLER. Everything downstream of this
            // point - the angle extraction, the declared orientation rebuild, the position
            // publish - is untouched and does not know the difference, which is exactly why
            // declared still equals rendered. Blend so strength 0 is the head, 1 is the hand,
            // and anything between is a real mix rather than a hard switch (a hard switch at the
            // grip press snaps the world; a 4-frame blend does not).
            {
                static float s_blend = 0.0f;
                const bool want = (g_handCamOn && g_handCamActive && g_handRelValid);
                const float target = want ? g_handCamStrength : 0.0f;
                s_blend += (target - s_blend) * 0.25f;          // ~4 frames in and out
                if (s_blend < 0.001f) { s_blend = 0.0f; g_handSmoothInit = false; }
                if (s_blend > 0.0f && g_handRelValid) {
                    // Same speed-adaptive filter as the head, own state: a hand shakes more than
                    // a head, and an unfiltered lens is unwatchable.
                    if (!g_handSmoothInit) { g_handSmoothQ = g_handRelQ; g_handSmoothInit = true; }
                    if (g_smoothing > 0.001f) {
                        const float bf = 1.0f - g_smoothing * 0.95f;
                        float t = QuatAngleDeg(g_handSmoothQ, g_handRelQ) / kSnapDeg;
                        if (t > 1.0f) t = 1.0f;
                        g_handSmoothQ = NlerpQuat(g_handSmoothQ, g_handRelQ, bf + (1.0f - bf) * t);
                    } else {
                        g_handSmoothQ = g_handRelQ;
                    }
                    q = NlerpQuat(g_smoothQuat, g_handSmoothQ, s_blend);
                    // POSITION only moves to the hand when the game will ACTUALLY move the camera
                    // there - i.e. lean is on and armed, because that is the code path that
                    // applies it. Declaring the hand position for an image the game rendered from
                    // the head is a ~40 cm declared/rendered mismatch, and that mismatch is
                    // exactly what produced the swimming this project spent a day chasing.
                    if (OLUE3_GetHeadPos() && g_posArmed) {
                        relP.x += (g_handRelP.x - relP.x) * s_blend;
                        relP.y += (g_handRelP.y - relP.y) * s_blend;
                        relP.z += (g_handRelP.z - relP.z) * s_blend;
                    }
                }
            }

            float fx = -2.0f * (q.x * q.z + q.w * q.y);
            float fy = -2.0f * (q.y * q.z - q.w * q.x);
            float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            float pyc = (fy < -1.0f) ? -1.0f : (fy > 1.0f ? 1.0f : fy);
            float yawR   = atan2f(fx, -fz);
            float pitchR = asinf(pyc);
            // ROLL - fixed 2026-07-22: "look behind and the camera flips upside down".
            //
            // Yaw and pitch above are read off the FORWARD vector, which is the yaw-pitch-roll
            // (Y then X then Z) decomposition. Roll must come from the SAME decomposition, and
            // for R = Ry(yaw)*Rx(pitch)*Rz(roll) that is atan2(R[1][0], R[1][1]):
            //     R[1][0] = 2(xy + wz)        R[1][1] = 1 - 2(x^2 + z^2)
            // The old denominator was 1 - 2(y^2 + z^2), which is R[0][0] - the roll term of a
            // DIFFERENT euler sequence. Substituting a pure yaw of theta makes that denominator
            // cos(theta): it goes negative past 90 degrees, so atan2(0, negative) snaps roll to
            // exactly 180 degrees and the whole view turns over the moment you look behind you.
            // R[1][1] stays +1 through a full turn, so this is stable all the way round.
            float rollR  = atan2f(2.0f * (q.x * q.y + q.w * q.z),
                                  1.0f - 2.0f * (q.x * q.x + q.z * q.z));

            // (Smoothing already applied to the quaternion above, so the angles here are the
            // smoothed ones; nothing further to filter.)
            // [POSEFREEZE] Under the worker these are the PENDING angles - they reach the game
            // only at the next publish, frozen for that whole frame. Inline mode writes the
            // live values directly, exactly as before.
            if (!g_useWorker) { g_olYawRad = yawR; g_olPitchRad = pitchR; g_olRollRad = rollR; }
            else { g_pendYaw = yawR; g_pendPitch = pitchR; g_pendRoll = rollR; }
            // Rebuild the declared orientation from the SAME three numbers the camera hook is
            // about to use. Two reasons it is rebuilt rather than passed through:
            //  - head tilt must do nothing, so roll is omitted unless HeadRoll is on. Claim roll
            //    the camera did not render and the compositor rotates the flat eye image to
            //    reconcile them - the picture spins with empty wedges at the corners.
            //  - smoothing changed the angles above; the declared pose has to carry the smoothed
            //    ones or the compositor is reconciling against a frame that was never rendered.
            // Ry(yaw) * Rx(pitch) [* Rz(roll)] is the composition the extraction decomposes, and
            // g_olYawRad is the NEGATED xr yaw. rolltest.cpp covers this round trip.
            {
                const float hy = -yawR * 0.5f, hp = pitchR * 0.5f;
                const XrQuaternionf qy = { 0.0f, sinf(hy), 0.0f, cosf(hy) };
                const XrQuaternionf qx = { sinf(hp), 0.0f, 0.0f, cosf(hp) };
                q = QMul(qy, qx);
                if (OLUE3_HeadRollEnabled()) {
                    const float hr = rollR * 0.5f;
                    const XrQuaternionf qz = { 0.0f, 0.0f, sinf(hr), cosf(hr) };
                    q = QMul(q, qz);
                }
            }
            // Remember the HEADSET's own field of view. The flat/quad path used to guess a fixed
            // 75 deg, which on a ~100 deg headset leaves a band of black down both sides - the
            // "menu doesn't fill the eye" complaint. The runtime provides the real number; use it.
            g_rtFovX = av[0].fov.angleRight - av[0].fov.angleLeft;
            g_rtFovY = av[0].fov.angleUp    - av[0].fov.angleDown;
            // [XRRUNTIME] keep each eye's own asymmetric frustum - the crop below needs the real
            // per-eye numbers, not the symmetric width above.
            g_rtEyeFov[0] = av[0].fov; g_rtEyeFov[1] = av[1].fov; g_rtEyeFovValid = true;
            // [XRSUBMIT] keep the runtime's OWN per-eye poses too. The gap between them is the
            // IPD the runtime expects the two images to have been rendered with - and this mod
            // deliberately declares ONE centre pose for both eyes because the stereo lives in
            // the pixels. A runtime that reprojects each view to its own eye point would then
            // add its separation on top of this mod's = over-separated, unfusable = "double".
            // This line is what distinguishes that from a leftover FOV error.
            g_rtEyePose[0] = av[0].pose; g_rtEyePose[1] = av[1].pose;
            {
                const float ex = av[1].pose.position.x - av[0].pose.position.x;
                const float ey = av[1].pose.position.y - av[0].pose.position.y;
                const float ez = av[1].pose.position.z - av[0].pose.position.z;
                g_rtEyeSep = sqrtf(ex * ex + ey * ey + ez * ez);
            }
            // [POSEFREEZE] Same split for the pose itself. The lock guards the struct copy: the
            // game thread reads it at publish, and a torn 7-float read is its own jitter source.
            AcquireSRWLockExclusive(&g_pubLock);
            if (!g_useWorker) {
                g_headPoseApp.orientation = q; g_headPoseApp.position = relP;
                g_olHeadX = relP.x; g_olHeadY = relP.y; g_olHeadZ = relP.z;
                g_olPoseValid = 1;
            } else {
                g_pendPose.orientation = q; g_pendPose.position = relP;
                g_pendValid = 1;
            }
            ReleaseSRWLockExclusive(&g_pubLock);
        }
    }

    int layerCount = 0;
    XrCompositionLayerProjection proj = {};
    XrCompositionLayerProjectionView pv[2] = {};
    XrCompositionLayerQuad quad = {};
    XrCompositionLayerQuad eyeQuad[2] = {};      // cinema screen: one quad per eye
    bool sbsProjectionSubmitted = false;                     // [STEAMVRSCENE] see below
    XrCompositionLayerProjection blackProj = {};             // [STEAMVRSCENE] placeholder, see below
    XrCompositionLayerProjectionView blackPv[2] = {};
    const XrCompositionLayerBaseHeader* layers[3];            // room for the placeholder underneath

    // ---- SBS path: the engine split the frame, so each half is one eye ----
    bool sbs = false;
    const int mode = OLUE3_StereoMode();
    if (fs.shouldRender && OLUE3_SplitActive() && EnsureEyeSwapchains()) {
        bool eyesOk = false;
        // The angle the rendered picture covers has to be known BEFORE the blit in [FITFRUSTUM] mode,
        // because it decides where inside the runtime's eye the picture is placed.
        float fx = 0.0f, fy = 0.0f;
        if (dev) OLUE3_GetEyeFov(&fx, &fy); else { fx = g_conFovX; fy = g_conFovY; }
        const bool fitMode = g_fitFrustum && g_rtEyeFovValid && fx > 0.1f && fy > 0.1f;
        bool fittedToRuntime = false;
        if (mode == 3) {
            // AER: this frame rendered one eye full-screen. Store it, then re-submit both eyes.
            // Dev-only lane: needs the live device pairing, stands down under the worker.
            if (dev && preCaptured && EnsureEyeHold()) {
                const int cur = OLUE3_CurrentEye() ? 1 : 0;
                g_ctx->CopyResource(g_eyeHold[cur], g_uploadTex);
                eyesOk = BlitHoldToEye(g_scL, g_imgL, g_eyeHold[0]) &&
                         BlitHoldToEye(g_scR, g_imgR, g_eyeHold[1]);
            }
        } else if (mode == 2) {
            // SFR: left eye is the copy taken between the two passes, right eye is the backbuffer.
            // Dev-only lane: needs two live-device captures per frame, stands down under the worker.
            IDirect3DSurface9* lsurf = dev ? OLProxy_LeftEye() : nullptr;
            if (lsurf && CaptureSurface(dev, lsurf))  eyesOk = BlitHalfToEye(g_scL, g_imgL, 0.0f, 0.0f, (float)g_w);
            if (eyesOk && CaptureBackbuffer(dev))     eyesOk = BlitHalfToEye(g_scR, g_imgR, 0.0f, 0.0f, (float)g_w);
            else                                      eyesOk = false;
        } else if (g_meModel) {
            // [STEREOMODEL 1 - Mass Effect] Sample a shifted window per half with the edge clamped
            // inside that half. Nothing is reserved, so the eye keeps the FULL half at every
            // convergence setting and no field of view is lost. The sign rides the HALF, not the
            // destination eye, exactly like ME: left half -conv, right half +conv.
            // SWAP flips the ROUTING here while ol_ue3 flips the CAMERAS with the same flag. The
            // two cancel for depth - the left eye still sees the left viewpoint, so there is no
            // inversion and no near-object doubling - and what actually changes is that the
            // convergence plane inverts, because the convergence sign rides the half.
            const float halfW  = (float)(g_w / 2);
            const float convPx = g_convShift * kConvUnit * halfW;
            const int   swap   = OLUE3_GetEyeSwap();
            XrSwapchain scL = swap ? g_scR : g_scL;  auto& imL = swap ? g_imgR : g_imgL;
            XrSwapchain scR = swap ? g_scL : g_scR;  auto& imR = swap ? g_imgL : g_imgR;
            g_cropLx = (int)(0.0f - convPx); g_cropRx = (int)(halfW + convPx);
            if (fitMode) {
                // Each half feeds whichever EYE the swap routing sends it to, so it must be
                // fitted to THAT eye's frustum - they are mirror images of each other.
                const int eyeOfLeftHalf  = swap ? 1 : 0;
                const int eyeOfRightHalf = swap ? 0 : 1;
                eyesOk = preCaptured
                      && BlitFitToEye(scL, imL, 0.0f - convPx, halfW - convPx, 0.0f, halfW,
                                      fx, fy, g_rtEyeFov[eyeOfLeftHalf])
                      && BlitFitToEye(scR, imR, halfW + convPx, (float)g_w + convPx, halfW, (float)g_w,
                                      fx, fy, g_rtEyeFov[eyeOfRightHalf]);
                fittedToRuntime = eyesOk;
            }
            if (!fittedToRuntime)
            eyesOk = preCaptured
                  && BlitHalfToEye(scL, imL, 0.0f  - convPx, 0.0f,  halfW)
                  && BlitHalfToEye(scR, imR, halfW + convPx, halfW, (float)g_w);
        } else {
            // Convergence: take each eye's half from a window slid inward/outward by conv pixels.
            // Positive moves the halves TOWARD each other, pushing the zero-parallax plane
            // further away. Clamped so the source window always stays inside the frame - a box
            // that runs off the edge is a silent no-copy in D3D11, i.e. a frozen eye.
            // ME1's sign: the LEFT window slides left and the RIGHT window slides right, so the two
            // images move toward each other and the zero-parallax plane pushes away. Each window is
            // clamped INSIDE its own half, so an eye can never sample the other eye's picture.
            const int halfW = (int)(g_w / 2);
            const int conv  = (int)(g_convShift * kConvUnit * (float)halfW);
            int lx = g_eyeSlack - conv;
            int rx = halfW + g_eyeSlack + conv;
            if (lx < 0) lx = 0;
            if (lx > halfW - (int)g_eyeW) lx = halfW - (int)g_eyeW;
            if (rx < halfW) rx = halfW;
            if (rx > (int)g_w - (int)g_eyeW) rx = (int)g_w - (int)g_eyeW;
            g_cropLx = lx; g_cropRx = rx;
            eyesOk = preCaptured
                  && BlitHalfToEye(g_scL, g_imgL, (float)lx, (float)lx, (float)(lx + g_eyeW))
                  && BlitHalfToEye(g_scR, g_imgR, (float)rx, (float)rx, (float)(rx + g_eyeW));
        }
        if (eyesOk) {
            // fx/fy were read above ([FOVLATCH]: under the worker these belong to the image on
            // screen, not to whatever the game's camera is doing right now).
            if (fx > 0.1f && fy > 0.1f) {
                // Cinema-screen decision, with a +/-3 deg hysteresis band so a FOVApproach sweep
                // through the threshold flips the presentation once, not every frame.
                const float fovDeg = fx * 57.2957795f;
                if (g_screenBelowDeg > 0.5f) {
                    if (!g_screenOn && fovDeg < g_screenBelowDeg - 3.0f) {
                        InterlockedExchange(&g_screenOn, 1);
                        // Plant the theater screen in the ROOM: kScreenDist in front of where
                        // the head is right now, upright (yaw-only - a pitched/rolled anchor
                        // would hang the screen tilted for the whole cutscene).
                        if (g_lastLocalValid) {
                            const XrVector3f fwd = QRotate(g_lastLocalQ, { 0.0f, 0.0f, -1.0f });
                            const float yaw = atan2f(-fwd.x, -fwd.z);
                            // Rotation about +Y by yaw: rotating (0,0,-1) by +theta lands at
                            // (-sin,0,-cos), and atan2(-fwd.x,-fwd.z) recovers exactly +theta -
                            // so the quaternion takes +yaw/2, no negation. (rolltest lesson:
                            // derive the round trip, never eyeball quaternion signs.)
                            const XrQuaternionf yq = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
                            const XrVector3f off = QRotate(yq, { 0.0f, 0.0f, -1.8f });
                            g_screenPose.orientation = yq;
                            g_screenPose.position = { g_lastLocalP.x + off.x,
                                                      g_lastLocalP.y + off.y,
                                                      g_lastLocalP.z + off.z };
                            g_screenPoseValid = 1;
                        } else g_screenPoseValid = 0;
                        XLOG("[OLVR][XR] cinema screen ON (eyeFov %.1f < %.1f deg) - %s", fovDeg,
                             g_screenBelowDeg, g_screenPoseValid ? "world-locked in front of you" : "head-locked (no pose yet)");
                    } else if (g_screenOn && fovDeg > g_screenBelowDeg + 3.0f) {
                        InterlockedExchange(&g_screenOn, 0);
                        XLOG("[OLVR][XR] cinema screen OFF (eyeFov %.1f > %.1f deg) - back to true projection", fovDeg, g_screenBelowDeg);
                    }
                } else if (g_screenOn) InterlockedExchange(&g_screenOn, 0);

                if (g_screenOn) {
                    // Per-eye quads: LEFT eye sees the left render, RIGHT eye the right render, on
                    // one coplanar screen grown to the headset's real FOV keeping the eye image's
                    // own aspect (stretching is the "squashed" look; never do it). WORLD-locked at
                    // the anchor captured when the screen engaged - a theater screen fixed in the
                    // room - falling back to head-locked only if no pose has ever been located.
                    const float kDist = 1.8f;
                    const float hfx = (g_rtFovX > 0.2f) ? g_rtFovX : 1.75f;   // ~100 deg fallback
                    const float hfy = (g_rtFovY > 0.2f) ? g_rtFovY : 1.75f;
                    const float maxW = 2.0f * kDist * tanf(hfx * 0.5f) * 0.98f;
                    const float maxH = 2.0f * kDist * tanf(hfy * 0.5f) * 0.98f;
                    float qw = maxW, qh = qw * (float)g_eyeH / (float)g_eyeW;
                    if (qh > maxH) { qh = maxH; qw = qh * (float)g_eyeW / (float)g_eyeH; }
                    for (int e = 0; e < 2; ++e) {
                        eyeQuad[e].type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                        eyeQuad[e].layerFlags = 0;
                        eyeQuad[e].eyeVisibility = (e == 0) ? XR_EYE_VISIBILITY_LEFT_VALUE
                                                            : XR_EYE_VISIBILITY_RIGHT_VALUE;
                        eyeQuad[e].subImage.swapchain = (e == 0) ? g_scL : g_scR;
                        eyeQuad[e].subImage.imageArrayIndex = 0;
                        eyeQuad[e].subImage.imageRect.offset = { 0, 0 };
                        eyeQuad[e].subImage.imageRect.extent = { (int32_t)g_eyeW, (int32_t)g_eyeH };
                        if (g_screenPoseValid) {
                            eyeQuad[e].space = g_localSpace;
                            eyeQuad[e].pose  = g_screenPose;
                        } else {
                            eyeQuad[e].space = g_viewSpace;
                            eyeQuad[e].pose  = IdentityPose(); eyeQuad[e].pose.position.z = -kDist;
                        }
                        eyeQuad[e].size.width = qw; eyeQuad[e].size.height = qh;
                        layers[e] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&eyeQuad[e]);
                    }
                    layerCount = 2;
                    sbs = true;
                } else {
                for (int e = 0; e < 2; ++e) {
                    pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                    // BOTH eyes carry the SAME (centre head) pose on purpose. The stereo lives in
                    // the IMAGES - the engine already rendered them from two separated cameras.
                    // Giving each eye its own IPD-apart pose makes the compositor reproject them
                    // to slightly different predicted poses, and they diverge during motion.
                    pv[e].pose = poseValidForThisImage ? poseForThisImage : IdentityPose();
                    // [PEREYEPOSE] Opt-in ([XR] PerEyePose=1). The default declares ONE centre
                    // pose for both views because the separation is already in the pixels, and
                    // that is what VDXR and SteamVR were tuned against. A runtime that instead
                    // reprojects each view to its OWN eye point sees two views claiming the same
                    // viewpoint and pulls them apart itself, adding its IPD on top of this mod's -
                    // over-separated and unfusable, i.e. "double everywhere". This declares each
                    // view at the point its image was actually rendered from: the camera used
                    // separation (halfEyeUU / WorldToMeters) along the declared right vector.
                    if (g_perEyePose && g_rtEyeSep > 0.001f) {
                        // Use the runtime's own MEASURED eye separation, not halfEyeUU /
                        // WorldToMeters: that world scale is a dead control in this build
                        // (positional tracking is off, the slider is inert) and at a saved
                        // 3.753uu / 50 it would declare 0.15m - twice the truth, which makes
                        // the doubling worse rather than better. rtEyeSep is what the runtime
                        // itself reports and is what it will reproject to.
                        const float s = (e == 0) ? -g_rtEyeSep * 0.5f : g_rtEyeSep * 0.5f;
                        const XrVector3f right = QRotate(pv[e].pose.orientation, { 1.0f, 0.0f, 0.0f });
                        pv[e].pose.position.x += right.x * s;
                        pv[e].pose.position.y += right.y * s;
                        pv[e].pose.position.z += right.z * s;
                    }
                    // The declared frustum is the CENTERED window, while the copy above took the
                    // SHIFTED window - deliberately, and the distinction is the whole feature
                    // (falsified live 2026-07-23): the crop being NARROWER than the half is
                    // honest and is compensated here through tan space; but the convergence
                    // SHIFT must NOT be compensated. Declaring the shifted crop's true angles
                    // puts every pixel back at its original direction and cancels the depth
                    // change EXACTLY - a mathematical no-op that leaves only the black edge
                    // bars, which is precisely what "convergence does nothing but squash the
                    // image" was. ME1 shifts the image and declares an unshifted frustum; the
                    // ~1-3 deg angular lie IS the toe-in mechanism. The half spans -T..+T in
                    // tan space, an edge at fraction f sits at tan = -T + 2*T*f.
                    if (g_meModel) {
                        // [STEREOMODEL 1] Nothing was reserved, so the image IS the full half and
                        // the honest frustum is the plain symmetric one. The convergence shift is
                        // still deliberately NOT compensated - that angular lie is the toe-in.
                        pv[e].fov.angleLeft  = -fx * 0.5f; pv[e].fov.angleRight = fx * 0.5f;
                    } else {
                        const float halfWf = (float)(g_w / 2);
                        const float f0 = (float)g_eyeSlack / halfWf;
                        const float f1 = f0 + (float)g_eyeW / halfWf;
                        const float T  = tanf(fx * 0.5f);
                        pv[e].fov.angleLeft  = atanf(-T + 2.0f * T * f0);
                        pv[e].fov.angleRight = atanf(-T + 2.0f * T * f1);
                    }
                    pv[e].fov.angleUp   =  fy * 0.5f; pv[e].fov.angleDown  = -fy * 0.5f;
                    pv[e].subImage.imageArrayIndex = 0;
                    pv[e].subImage.imageRect.offset = { 0, 0 };
                    pv[e].subImage.imageRect.extent = { (int32_t)g_eyeW, (int32_t)g_eyeH };

                    // [XRRUNTIME] Meta and SteamVR only: declare the INTERSECTION of the declared window and
                    // the runtime's own per-eye frustum, and crop the submitted rect to exactly
                    // that. Pure rect arithmetic in tan space - nothing is re-rendered, and the
                    // declared FOV still describes precisely the pixels being handed over, because
                    // the rect shrinks along with it. Ported from ME1's [LINKFOV]/[STEAMVRFOV].
                    // On Virtual Desktop and every other runtime this never runs.
                    if (fittedToRuntime) {
                        // [FITFRUSTUM] The image already spans this eye exactly, so declare the
                        // runtime's own frustum and hand over the whole thing. Nothing here can
                        // disagree with the pixels, which is the entire point - a runtime that
                        // ignores the declaration lands on the same answer as one that honours it.
                        pv[e].fov = g_rtEyeFov[e];
                        pv[e].subImage.imageRect.offset = { 0, 0 };
                        pv[e].subImage.imageRect.extent = { (int32_t)g_eyeW, (int32_t)g_eyeH };
                    } else if ((g_isOculusRuntime || g_isSteamVrRuntime) && g_rtEyeFovValid) {
                        const XrFovf d = pv[e].fov;              // what was rendered
                        const XrFovf& t = g_rtEyeFov[e];         // what the runtime expects
                        const float aL = (d.angleLeft  > t.angleLeft)  ? d.angleLeft  : t.angleLeft;
                        const float aR = (d.angleRight < t.angleRight) ? d.angleRight : t.angleRight;
                        const float aU = (d.angleUp    < t.angleUp)    ? d.angleUp    : t.angleUp;
                        const float aD = (d.angleDown  > t.angleDown)  ? d.angleDown  : t.angleDown;
                        const float tanDL = tanf(d.angleLeft), tanDR = tanf(d.angleRight);
                        const float tanDU = tanf(d.angleUp),   tanDD = tanf(d.angleDown);
                        const float hSpan = tanDR - tanDL, vSpan = tanDU - tanDD;
                        if (aR - aL > 0.05f && aU - aD > 0.05f && hSpan > 1e-4f && vSpan > 1e-4f) {
                            const int32_t w = (int32_t)g_eyeW, h = (int32_t)g_eyeH;
                            int32_t x0 = (int32_t)lroundf((tanf(aL) - tanDL) / hSpan * w);
                            int32_t x1 = (int32_t)lroundf((tanf(aR) - tanDL) / hSpan * w);
                            int32_t y0 = (int32_t)lroundf((tanDU - tanf(aU)) / vSpan * h);
                            int32_t y1 = (int32_t)lroundf((tanDU - tanf(aD)) / vSpan * h);
                            if (x0 < 0) x0 = 0; if (x0 > w - 1) x0 = w - 1;
                            if (x1 < x0 + 1) x1 = x0 + 1; if (x1 > w) x1 = w;
                            if (y0 < 0) y0 = 0; if (y0 > h - 1) y0 = h - 1;
                            if (y1 < y0 + 1) y1 = y0 + 1; if (y1 > h) y1 = h;
                            pv[e].subImage.imageRect.offset = { x0, y0 };
                            pv[e].subImage.imageRect.extent = { x1 - x0, y1 - y0 };
                            pv[e].fov.angleLeft = aL; pv[e].fov.angleRight = aR;
                            pv[e].fov.angleUp   = aU; pv[e].fov.angleDown  = aD;
                            static int s_cropLogs = 0;
                            if (s_cropLogs < 2) { ++s_cropLogs;
                                XLOG("[OLVR][XRRUNTIME] eye %d cropped to the runtime frustum: "
                                     "rect %dx%d+%d,%d fov %.1f/%.1f deg horizontal",
                                     e, x1 - x0, y1 - y0, x0, y0,
                                     aL * 57.2957795f, aR * 57.2957795f); }
                        }
                    }
                }
                pv[0].subImage.swapchain = g_scL;
                pv[1].subImage.swapchain = g_scR;
                for (int e = 0; e < 2; ++e) {
                    g_dbgDeclFov[e] = pv[e].fov;
                    g_dbgRect[e][0] = pv[e].subImage.imageRect.offset.x;
                    g_dbgRect[e][1] = pv[e].subImage.imageRect.offset.y;
                    g_dbgRect[e][2] = pv[e].subImage.imageRect.extent.width;
                    g_dbgRect[e][3] = pv[e].subImage.imageRect.extent.height;
                }
                g_dbgDeclPose = pv[0].pose;
                proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                proj.layerFlags = 0; proj.space = g_appSpace; proj.viewCount = 2; proj.views = pv;
                sbsProjectionSubmitted = true;   // [STEAMVRSCENE] a real scene layer went out
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                layerCount = 1;
                sbs = true;
                static bool once = false;
                if (!once) { once = true;
                    XLOG("[OLVR][XR] STEREO submit live: %ux%u per eye, fov %.1f x %.1f deg",
                         g_eyeW, g_eyeH, fx * 57.2957795f, fy * 57.2957795f); }
                }
            }
        }
    }

    if (!sbs && fs.shouldRender && preCaptured && BlitToSwapchain()) {
        // Until the camera lane reports the game's REAL rendered FOV, the quad is the honest
        // presentation: a flat panel makes no claim about world geometry. Projection (P) is a
        // debug/preview path and WILL be wrong-scaled until FOV is wired.
        float fovX = (g_olDeclFovXDeg > 0.5f) ? g_olDeclFovXDeg * 0.01745329f : 1.5708f;  // 90deg = Outlast DefaultFOV
        float fovY = (g_olDeclFovYDeg > 0.5f) ? g_olDeclFovYDeg * 0.01745329f
                                              : 2.0f * atanf(tanf(fovX * 0.5f) * (float)g_h / (float)g_w);
        if (g_olUseProjection) {
            for (int e = 0; e < 2; ++e) {
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                pv[e].pose = g_olPoseValid ? g_headPoseApp : IdentityPose();
                pv[e].fov.angleLeft = -fovX * 0.5f; pv[e].fov.angleRight = fovX * 0.5f;
                pv[e].fov.angleUp = fovY * 0.5f;    pv[e].fov.angleDown = -fovY * 0.5f;
                pv[e].subImage.swapchain = g_swapchain;      // mono: both eyes read the same image
                pv[e].subImage.imageArrayIndex = 0;
                pv[e].subImage.imageRect.offset = { 0, 0 };
                pv[e].subImage.imageRect.extent = { (int32_t)g_w, (int32_t)g_h };
            }
            proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            proj.layerFlags = 0; proj.space = g_appSpace; proj.viewCount = 2; proj.views = pv;
            sbsProjectionSubmitted = true;   // [STEAMVRSCENE] mono projection is a scene layer too
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            layerCount = 1;
        } else {
            // The quad is a virtual SCREEN showing the full frame (menus, loading). Its width
            // used to follow the last captured per-eye world FOV, so after a narrow-FOV stretch
            // (camcorder zoom, intro cameras at ~33deg) the main menu inherited that angle and
            // showed as a postage stamp floating in black. A flat screen makes no claim about
            // world geometry - give it a fixed, comfortable width instead.
            // Fill as much of the headset as the image honestly can. Grow the quad to the runtime's
            // real FOV, keep the frame's own aspect (never stretch - that is the "squashed" look),
            // and if that makes it taller than the headset, fit to the height instead. Whatever
            // black is left over is the difference between a 16:9 frame and a near-square lens,
            // and the only way to remove THAT is to render wider, not to distort the picture that exists.
            const float kDist = 1.8f;
            const float fovX = (g_rtFovX > 0.2f) ? g_rtFovX : 1.75f;   // ~100 deg fallback
            const float fovY = (g_rtFovY > 0.2f) ? g_rtFovY : 1.75f;
            const float maxW = 2.0f * kDist * tanf(fovX * 0.5f) * 0.98f;
            const float maxH = 2.0f * kDist * tanf(fovY * 0.5f) * 0.98f;
            float qw = maxW, qh = qw * (float)g_h / (float)g_w;
            if (qh > maxH) { qh = maxH; qw = qh * (float)g_w / (float)g_h; }
            quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE; quad.layerFlags = 0; quad.space = g_viewSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
            quad.subImage.swapchain = g_swapchain; quad.subImage.imageArrayIndex = 0;
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { (int32_t)g_w, (int32_t)g_h };
            quad.pose = IdentityPose(); quad.pose.position.z = -kDist;
            quad.size.width = qw; quad.size.height = qh;
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            layerCount = 1;
        }
    }

    // [STEAMVRSCENE] SteamVR's compositor is a scene-app state machine, not a layer blender. Quad
    // layers arrive as OVERLAYS, but its waiting room stays up until the app submits PROJECTION
    // frames, and it comes back the moment they stop - so on SteamVR every frame that carries only
    // quads (menus, the cinema screen, anything before the first capture) would show the void with
    // the settings menu floating in it. ME1 hit exactly this and its cure is a tiny opaque-black projection
    // layer slipped UNDERNEATH the quads to keep the scene state alive. It is 64px, it is declared
    // at the runtime's own frustum so it can never trip the crop above, and it is never built or
    // submitted on any other runtime.
    if (g_isSteamVrRuntime && g_blackAllowed && layerCount > 0 && !sbsProjectionSubmitted && g_rtEyeFovValid &&
        EnsureBlackScene()) {
        for (int e = 0; e < 2; ++e) {
            blackPv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
            blackPv[e].pose = g_olPoseValid ? g_headPoseApp : IdentityPose();
            blackPv[e].fov  = g_rtEyeFov[e];
            blackPv[e].subImage.swapchain = g_blackSc;
            blackPv[e].subImage.imageArrayIndex = 0;
            blackPv[e].subImage.imageRect.offset = { 0, 0 };
            blackPv[e].subImage.imageRect.extent = { kBlackSceneSize, kBlackSceneSize };
        }
        blackProj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
        blackProj.layerFlags = 0; blackProj.space = g_appSpace;
        blackProj.viewCount = 2; blackProj.views = blackPv;
        // Slide the existing layers up and put the placeholder at the bottom of the stack.
        for (int i = layerCount; i > 0; --i) layers[i] = layers[i - 1];
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&blackProj);
        ++layerCount;
        static bool onceBlack = false;
        if (!onceBlack) { onceBlack = true;
            XLOG("[OLVR][XRRUNTIME] SteamVR: adding the black scene layer under quad-only frames"); }
    }

    // [POSEFREEZE] OLUE3_FrameTick is NOT called here any more - it ages GAME-frame counters
    // (split freshness, the head-injection ramp) and its own comment says "ticked once per
    // Present". Under the worker this ran at 90Hz against a ~35fps game, draining the split
    // freshness 2-3x too fast: the moment the game missed a split the mod declared stereo
    // stale and fell to the head-locked QUAD layer, then the next game frame revived it. That
    // flapping between a world-locked projection and a head-locked panel IS "flickering head
    // tracking" (visible in the log as layer=STEREO/quad alternating). It now ticks once per
    // Present in both modes, from OLXR_OnPresent.
    XrFrameEndInfo fei = {}; fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount; fei.layers = layerCount ? layers : nullptr;
    // [XRENDF] ME1's lesson, ported: a runtime rejecting the layers is indistinguishable from
    // success if the result is discarded (SteamVR validates and rejects; Meta forgives). The
    // result also rides the periodic log below, so a run answers this without a repro recipe.
    const XrResult efr = g_fn.endFrame(g_session, &fei);
    g_lastEndFrame = (LONG)efr;
    if (!XrSucceeded(efr)) {
        static unsigned s_efLogs = 0;
        const unsigned n = s_efLogs++;
        if (n < 8 || (n % 300) == 0)
            XLOG("[OLVR][XRENDF] xrEndFrame FAILED r=%d layers=%d", (int)efr, layerCount);
    }

    ++g_frame;
    static DWORD s_lastLog = 0;
    DWORD now = NowMs();
    if (now - s_lastLog > 3000) {
        s_lastLog = now;
        // Log the per-eye FOV CONTINUOUSLY, not once at startup. Outlast's camera FOV moves
        // constantly during play - DefaultFOV=90, RunningFOV=100, and the camcorder zooms down
        // to CamcorderMinFOV=15 - and under AspectRatio_MaintainXFOV that value IS the horizontal
        // FOV each eye renders. A single early sample says nothing about the rest of the run;
        // one taken during a camcorder zoom reads ~32deg and looks like a bug that isn't one.
        float fx = 0.0f, fy = 0.0f;
        OLUE3_GetEyeFov(&fx, &fy);
        // bb/eye sizes and the headset's own FOV: the "menus and cutscenes do not fill the eye"
        // class of bug is decided by WHICH path the frame took (STEREO vs quad) and how the source
        // aspect compares to the lens. Guessing that from a description is how wrong builds happen.
        XLOG("[OLVR][XR] bb=%ux%u eye=%ux%u hmdFov=%.0fx%.0fdeg slack=%d",
             g_w, g_h, g_eyeW, g_eyeH, g_rtFovX * 57.2957795f, g_rtFovY * 57.2957795f, g_eyeSlack);
        // gameFrames is the published-frame counter: (delta between two log lines)/3s = the
        // game's real fps, while frames= is the submit cadence - under the worker the two
        // are DIFFERENT numbers, and their ratio is this whole fix working.
        // [HEADPOS] head=(x,y,z) is the position offset in METRES from the current baseline and
        // pos= says whether lean/duck is applying it. A seated human stays within ~0.2 m of
        // their own centre; a steady y of +0.8 or more with pos=armed is a desk baseline - recenter.
        XLOG("[OLVR][XR] frames=%lld layer=%s pose=%ld yaw=%.1f pitch=%.1f head=(%.2f,%.2f,%.2f)m pos=%s eyeFov=%.1fx%.1fdeg state=%s ef=%ld gameFrames=%llu",
             g_frame, sbs ? (g_screenOn ? "SCREEN" : "STEREO") : (g_olUseProjection ? "proj" : "quad"), (long)g_olPoseValid,
             g_olYawRad * 57.2957795f, g_olPitchRad * 57.2957795f,
             g_olHeadX, g_olHeadY, g_olHeadZ,
             !OLUE3_GetHeadPos() ? "off" : (g_posArmed ? "armed" : (g_posPickedUp ? "settling" : "waiting")),
             fx * 57.2957795f, fy * 57.2957795f,
             SessionStateName(g_sessionState), (long)g_lastEndFrame,
             (unsigned long long)(dev ? 0 : g_conSeq));
        // [XRSUBMIT] The full submitted truth, ME1's shape. Reads the doubling question
        // directly: rtEyeSep is the separation the RUNTIME expects between the two eye
        // viewpoints; this mod declares ONE pose for both (declSep is therefore 0 by construction).
        // If a runtime honours its own eye points, the submitted images carry this mod's own
        // separation AND its reprojection adds that gap on top - the over-separated, unfusable picture.
        if (g_rtEyeFovValid) {
            const float dx = g_rtEyePose[1].position.x - g_rtEyePose[0].position.x;
            const float dy = g_rtEyePose[1].position.y - g_rtEyePose[0].position.y;
            const float dz = g_rtEyePose[1].position.z - g_rtEyePose[0].position.z;
            XLOG("[OLVR][XRSUBMIT] rtEyeSep=%.4fm rtFov e0 %.1f/%.1f e1 %.1f/%.1f | decl e0 %.1f/%.1f "
                 "rect %dx%d+%d,%d | decl e1 %.1f/%.1f rect %dx%d+%d,%d | declPose q=(%.3f,%.3f,%.3f,%.3f)",
                 sqrtf(dx * dx + dy * dy + dz * dz),
                 g_rtEyeFov[0].angleLeft * 57.2957795f, g_rtEyeFov[0].angleRight * 57.2957795f,
                 g_rtEyeFov[1].angleLeft * 57.2957795f, g_rtEyeFov[1].angleRight * 57.2957795f,
                 g_dbgDeclFov[0].angleLeft * 57.2957795f, g_dbgDeclFov[0].angleRight * 57.2957795f,
                 g_dbgRect[0][2], g_dbgRect[0][3], g_dbgRect[0][0], g_dbgRect[0][1],
                 g_dbgDeclFov[1].angleLeft * 57.2957795f, g_dbgDeclFov[1].angleRight * 57.2957795f,
                 g_dbgRect[1][2], g_dbgRect[1][3], g_dbgRect[1][0], g_dbgRect[1][1],
                 g_dbgDeclPose.orientation.x, g_dbgDeclPose.orientation.y,
                 g_dbgDeclPose.orientation.z, g_dbgDeclPose.orientation.w);
        }
    }
    return true;
}

// [SUBMITTHREAD] The worker: a clean loop whose only blocking cost is waitFrame itself, so the
// compositor sees its cadence answered every single interval no matter what the game is doing.
static DWORD WINAPI WorkerMain(LPVOID) {
    while (!g_workerStop) {
        // [EXITGUARD] Step OUT of the OpenXR runtime as soon as the game stops presenting.
        // When the game closes, Windows tears the process down while this thread may be parked
        // inside xrWaitFrame; the runtime's own teardown then blocks on a lock this thread
        // holds and the process hangs forever with no window, appearing to still be running after
        // the game was closed. A thread sitting in Sleep() is safe to tear down, one inside the
        // runtime is not. Present going quiet is also exactly what a shutdown looks like from
        // here, and if it resumes - a long load, a hitch - playback simply picks back up.
        const DWORD quiet = NowMs() - g_lastPresentMs;
        if (g_lastPresentMs && quiet > 2000) { Sleep(50); continue; }
        PollEvents();
        if (g_dead) break;
        if (!g_running) { Sleep(10); continue; }
        ConsumeUpload();
        if (!RunXrFrame(nullptr, g_conSeq != 0)) Sleep(20);
    }
    return 0;
}

void OLXR_Shutdown() {
    // This runs from DllMain(PROCESS_DETACH), which holds the LOADER LOCK. Waiting on the
    // worker there is a guaranteed deadlock - the thread cannot complete its exit while this
    // lock is held, so the game "closes" and the process stays alive forever (measured
    // 2026-08-04, requiring the process to be killed manually). Signal and walk away: the process is being
    // torn down, and destroying XR objects underneath a thread that may still be inside an
    // OpenXR call is worse than leaving the OS to reclaim them.
    if (g_workerThread) {
        InterlockedExchange(&g_workerStop, 1);
        CloseHandle(g_workerThread); g_workerThread = nullptr;
        g_dead = true;              // stops the worker at its next loop check
        return;
    }
    if (g_session) { if (g_running) g_fn.endSession(g_session); g_fn.destroySession(g_session); g_session = 0; }
    if (g_sysSurf) { g_sysSurf->Release(); g_sysSurf = nullptr; }
    if (g_uploadTex) { g_uploadTex->Release(); g_uploadTex = nullptr; }
    if (g_blitTmpRtv) { g_blitTmpRtv->Release(); g_blitTmpRtv = nullptr; }
    if (g_blitTmp)    { g_blitTmp->Release();    g_blitTmp = nullptr; }
    if (g_blitBlend)  { g_blitBlend->Release();  g_blitBlend = nullptr; }
    if (g_blitDepth)  { g_blitDepth->Release();  g_blitDepth = nullptr; }
    if (g_blitRaster) { g_blitRaster->Release(); g_blitRaster = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_d3d11) { g_d3d11->Release(); g_d3d11 = nullptr; }
}
