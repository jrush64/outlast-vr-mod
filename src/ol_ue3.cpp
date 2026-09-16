// ol_ue3.cpp - Outlast Stage 3: CalcSceneView hook + live offset discovery.
// =============================================================================
// The stereo mechanism (ported from ME2/ME3, see Docs/MELE_SAMEFRAME_STEREO_METHOD.md):
// hook ULocalPlayer::CalcSceneView, call it TWICE per frame with the player's normalized
// viewport rect rewritten to the left then right half of the backbuffer. The engine renders
// both eyes side-by-side into one frame, each with its own culling/post.
//
// To do that in Outlast, two things come from Ghidra and two things only come from the running
// game:
//   (Ghidra, static)  CalcSceneView RVA, AllocateViewState RVA
//   (live, this file) the ULocalPlayer viewport-rect field offsets, and the FSceneView layout
//
// So this file is BOTH the discovery tool and, once armed, the hook. It is read-only until
// explicitly armed, and every game-memory read is SEH-guarded.
//
// Config: outlastvr.ini next to OLGame.exe
//   [Stage3]
//   CalcSceneViewRva=0x0        ; RVA in the RUNNING exe; 0 = disabled
//   Mode=probe                  ; probe = log only (safe) | split = double-call stereo
// =============================================================================

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <math.h>
#include <share.h>    // _SH_DENYWR for the [CAMPROBE] csv (readable while the game runs)
#include <tlhelp32.h> // [BONEWATCH] thread enumeration, to set debug registers on every thread
#include <intrin.h>   // _ReturnAddress for the call-site gate
#include "ol_ue3.h"
#include "ol_xr.h"
#include "ol_menu.h"
#include "MinHook.h"

static uintptr_t g_base = 0;
static uintptr_t g_cvRva = 0;
static uintptr_t g_vpRva = 0;
static uintptr_t g_avsRva = 0;
static uintptr_t g_sharedRva = 0;
static uintptr_t g_creatorRva = 0;
static volatile long  g_eyePhase = 0;   // 0 = off, 1 = LEFT eye (-half), 2 = RIGHT eye (+half)
// AER state (declared up here because OLUE3_StereoMode(), defined further down, reports it)
static volatile long g_aerMode = 0;
static volatile long g_aerFrame = 0;
static volatile long g_aerEye = 0;      // eye rendered THIS frame: 0 = left, 1 = right
static bool      g_splitMode = false;
static long      g_calls = 0;
static long      g_logged = 0;

// UE3 x64 CalcSceneView, 5-arg form:
//   FSceneView* __fastcall(ULocalPlayer* this, FSceneViewFamily* Family,
//                          FVector& OutViewLoc, FRotator& OutViewRot,
//                          FViewport* Viewport, FViewElementDrawer* Drawer)
typedef void* (__fastcall *CalcSceneView_t)(void*, void*, void*, void*, void*, void*);
static CalcSceneView_t o_CalcSceneView = nullptr;

// ---- SEH-guarded reads. Every one of these can hit a freed/garbage pointer. ----
static bool SafeReadBytes(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeReadFloat(const void* p, float* out) { return SafeReadBytes(p, out, sizeof(float)); }

static bool PlausibleFrac(float f) { return f >= -0.001f && f <= 1.001f; }

// ---- viewport-rect finder -------------------------------------------------------------
// UE3's ULocalPlayer stores the normalized viewport rect as four consecutive floats:
//   Origin.X, Origin.Y, Size.X, Size.Y
// For a single fullscreen player that is EXACTLY {0,0,1,1} - a very distinctive quad. Scan
// the object for it. Dishonored (x86, 2012 UE3) had it at +0x64; Outlast is x64 so the
// offset WILL differ. Do not assume - this is why it is scanned rather than assumed.
static const size_t kScanBytes = 0x900;

static void ScanForRect(void* obj) {
    if (!obj) return;
    unsigned char buf[kScanBytes];
    if (!SafeReadBytes(obj, buf, sizeof(buf))) { OLProxyLog("[OLVR][S3] rect scan: object unreadable"); return; }
    char line[512];
    int hits = 0;
    for (size_t off = 0; off + 16 <= sizeof(buf); off += 4) {
        const float* f = reinterpret_cast<const float*>(buf + off);
        if (f[0] == 0.0f && f[1] == 0.0f && f[2] == 1.0f && f[3] == 1.0f) {
            sprintf_s(line, "[OLVR][S3] rect CANDIDATE at this+0x%zx = {0,0,1,1}", off);
            OLProxyLog(line);
            if (++hits >= 12) break;
        }
    }
    if (!hits) {
        // Not fullscreen (letterboxed/cutscene?) - dump every plausible 0..1 quad instead.
        for (size_t off = 0; off + 16 <= sizeof(buf); off += 4) {
            const float* f = reinterpret_cast<const float*>(buf + off);
            if (PlausibleFrac(f[0]) && PlausibleFrac(f[1]) && f[2] > 0.01f && f[2] <= 1.001f && f[3] > 0.01f && f[3] <= 1.001f) {
                sprintf_s(line, "[OLVR][S3] rect maybe at this+0x%zx = {%.3f,%.3f,%.3f,%.3f}", off, f[0], f[1], f[2], f[3]);
                OLProxyLog(line);
                if (++hits >= 12) break;
            }
        }
    }
    if (!hits) OLProxyLog("[OLVR][S3] rect scan: no 0..1 quad found in the first 0x900 bytes");
}

// ---- FSceneView layout finder ----------------------------------------------------------
// Needed: ViewMatrix (camera right vector = the eye-shift axis), ProjectionMatrix (the FOV that
// must be declared to the compositor), PreViewTranslation and ViewOrigin (where the eye shift is
// applied). Find them by shape: a view matrix's last row is {x,y,z,1}; a UE3 perspective
// projection has proj[0]=1/tan(halfH), proj[5]=1/tan(halfV), proj[11]=1, proj[15]=0.
static void ScanSceneView(void* view) {
    if (!view) { OLProxyLog("[OLVR][S3] CalcSceneView returned NULL"); return; }
    unsigned char buf[0x400];
    if (!SafeReadBytes(view, buf, sizeof(buf))) { OLProxyLog("[OLVR][S3] view scan: unreadable"); return; }
    char line[512];
    int projHits = 0, mtxHits = 0;
    for (size_t off = 0; off + 64 <= sizeof(buf); off += 4) {
        const float* m = reinterpret_cast<const float*>(buf + off);
        // projection: [0] and [5] positive finite, [11]==1, [15]==0, [1]==[2]==[4]==0
        if (m[11] == 1.0f && m[15] == 0.0f && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f &&
            m[1] == 0.0f && m[2] == 0.0f && m[4] == 0.0f) {
            float fovH = 2.0f * atanf(1.0f / m[0]) * 57.2957795f;
            float fovV = 2.0f * atanf(1.0f / m[5]) * 57.2957795f;
            sprintf_s(line, "[OLVR][S3] PROJECTION at view+0x%zx  fovH=%.1fdeg fovV=%.1fdeg", off, fovH, fovV);
            OLProxyLog(line);
            if (++projHits >= 4) break;
        }
        // rigid transform: last row {x,y,z,1} and the 3x3 rows unit-length
        else if (m[15] == 1.0f && m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f) {
            float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
            if (l0 > 0.98f && l0 < 1.02f && mtxHits < 6) {
                sprintf_s(line, "[OLVR][S3] view-like MATRIX at view+0x%zx right=(%.3f,%.3f,%.3f) trans=(%.1f,%.1f,%.1f)",
                          off, m[0], m[4], m[8], m[12], m[13], m[14]);
                OLProxyLog(line);
                ++mtxHits;
            }
        }
    }
    if (!projHits) OLProxyLog("[OLVR][S3] view scan: no projection-shaped matrix in the first 0x400 bytes");
}

// ---- the split ------------------------------------------------------------------------
// Confirmed live 2026-07-22: ULocalPlayer's normalized viewport rect is four floats at
//   this+0xA0 Origin.X, +0xA4 Origin.Y, +0xA8 Size.X, +0xAC Size.Y
// Rewrite it to the left half, let the engine build a view; rewrite to the right half, build
// a second view; restore. The engine renders both into one backbuffer, side by side, in one
// frame - each with its own culling and post.
static const size_t kRectOff = 0xA0;

// CalcSceneView has FOUR call sites and only one is the render path. The others build views
// for Project/Deproject-style queries at different FOVs (86/96/60 deg were all seen in one
// session). Splitting those would corrupt whatever they feed. Gate on the return address
// landing inside UGameViewportClient::Draw.
static uintptr_t g_drawLo = 0, g_drawHi = 0;
static volatile long g_splitOn = 0;      // runtime toggle (NUM4), seeded from the ini
static volatile long g_inSplit = 0;      // re-entry guard
static long g_splitFrames = 0, g_splitFail = 0;

// The FOV declared to the compositor must be EXACTLY what the engine rendered, or the image
// warps and swims. Do not compute it from the ini's DefaultFOV: the halves are half-width, the
// game changes FOV when running, and the camcorder zooms. Read it off the projection matrix of
// the view the engine actually built (FSceneView+0xC0, confirmed live), every frame.
static const size_t kProjOff = 0xC0;
static volatile long  g_splitFresh = 0;      // frames since the last successful split
static volatile float g_fovX = 0.0f, g_fovY = 0.0f;

static void g_cineMeasureK(float eyeTan);   // fwd: [CINEFOV] state lives further down

static void CaptureRenderedFov(void* view) {
    float m[16];
    if (!view || !SafeReadBytes((char*)view + kProjOff, m, sizeof(m))) return;
    if (!(m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f)) return;
    g_fovX = 2.0f * atanf(1.0f / m[0]);
    g_fovY = 2.0f * atanf(1.0f / m[5]);
    g_cineMeasureK(1.0f / m[0]);   // [CINEFOV] self-correct the per-eye mapping constant
}

// Consumed by the XR submit. The freshness counter gives hysteresis: a loading screen stops
// splitting for a moment and the headset must not flip to mono and back (that flashes).
extern "C" int OLUE3_SplitActive() { return (g_splitFresh > 0) ? 1 : 0; }

// HEAD-INJECTION WEIGHT. 1 in gameplay, 0 while the cinema screen is up (menus/cutscenes):
// injecting head rotation there pans the picture inside a screen that itself follows the head -
// the "warpy, off" cutscene look. ME1's cine shape is a tracking-off 3D picture. The weight is
// RAMPED (exponential, ~15 frames), never stepped: stepping a camera rotation in one frame is
// the FP-storm lesson from ME1. Ticked once per Present so both eyes of a frame share one value.
extern "C" int OLXR_ScreenActive();
static volatile float g_injW = 1.0f;

// [NVLIGHT] the gamepad/body aim, captured pre-injection in Hook_GetViewPoint. Declared here
// because OLUE3_FrameTick (just below) ages its freshness counter.
static volatile int  g_baseViewRot[3] = { 0, 0, 0 };
static volatile long g_baseViewFresh = 0;

extern "C" void OLUE3_NvLightReport();

extern "C" void OLUE3_FrameTick() {
    if (g_splitFresh > 0) InterlockedDecrement(&g_splitFresh);
    if (g_baseViewFresh > 0) InterlockedDecrement(&g_baseViewFresh);
    // [NVCENSUS] every ~10 s, so one run with the camcorder raised answers which light is which.
    static long s_tick = 0;
    if ((++s_tick % 600) == 0) OLUE3_NvLightReport();
    const float target = OLXR_ScreenActive() ? 0.0f : 1.0f;
    float w = g_injW + (target - g_injW) * 0.15f;
    if (w < 0.001f) w = 0.0f; if (w > 0.999f) w = 1.0f;
    g_injW = w;
}
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy) {
    if (fx) *fx = g_fovX;
    if (fy) *fy = g_fovY;
}

static bool SafeWriteRect(void* self, const float* r) {
    __try { memcpy((char*)self + kRectOff, r, 4 * sizeof(float)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- per-eye view state ------------------------------------------------------------------
// UE3 keeps each view's TEMPORAL history in an FSceneViewState hanging off the LocalPlayer at
// +0xB0 (confirmed: CalcSceneView reads it, and AllocateViewState's caller tests that field).
// History means eye adaptation/auto-exposure, occlusion-query results, and any temporal post.
//
// Handing BOTH eyes the same state makes them ping-pong: the left eye renders and updates the
// history, the right eye then renders against the left's, and vice versa next frame. With two
// identical cameras that was invisible; with real parallax it shows up as flicker and as one
// eye being a different brightness/colour from the other. Exactly what was observed.
//
// Fix: allocate a SECOND state with the engine's own AllocateViewState() and swap it in for the
// right eye, so each eye accumulates its own history. Allocated once, lazily, then reused.
typedef void* (__fastcall *AllocViewState_t)();
static AllocViewState_t o_AllocViewState = nullptr;
static const size_t kViewStateOff = 0xB0;
static void* g_stateR = nullptr;
static bool  g_stateTried = false;
static void* g_localPlayer = nullptr;   // captured in CalcSceneView; SFR needs it to swap states

static bool SafeWritePtr(void* obj, size_t off, void* val) {
    __try { *(void**)((char*)obj + off) = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void* SafeReadPtr(void* obj, size_t off) {
    __try { return *(void**)((char*)obj + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- the OTHER view-state source ---------------------------------------------------------
// CalcSceneView does NOT always take the view state from the LocalPlayer:
//     if ((this[0xC8] & 1) == 0 || globalFlag != 0)  state = this->ViewState;   // +0xB0
//     else                                           state = GetSharedViewState(g);
// and GetSharedViewState hands back ONE lazily-created object cached at g+0x528. If that is the
// live branch, both eyes share a single state no matter what is written to +0xB0 -- which is
// exactly what happened when swapping +0xB0 changed nothing.
//
// Symptom fit: discrete objects (chairs, windows, doors) flicker while ground and walls do not.
// Those are the primitives UE3 runs hardware OCCLUSION QUERIES on, and the per-primitive
// occlusion history lives in the view state. Two views sharing one history means each frame's
// visibility answers get applied to the wrong camera, so objects wink in and out.
//
// So: hook the getter and hand the right eye its own instance, built by the engine's own
// creator so it is a real, correctly-constructed object rather than a hand-crafted stand-in.
typedef void* (__fastcall *GetSharedState_t)(void*);
typedef void* (__fastcall *StateCreator_t)(void*, void*, void*, void*, void*);
static GetSharedState_t o_GetSharedState = nullptr;
static StateCreator_t   o_StateCreator = nullptr;
static void* g_sharedStateR = nullptr;
static volatile long g_sharedHits = 0, g_sharedGaveOurs = 0;

static void* __fastcall Hook_GetSharedState(void* g) {
    void* orig = o_GetSharedState(g);
    InterlockedIncrement(&g_sharedHits);
    if (g_eyePhase != 2) return orig;                 // left eye / not splitting: untouched

    if (!g_sharedStateR && o_StateCreator && g) {
        __try {
            void* arg = *(void**)((char*)g + 0x530);  // the same argument the engine passes
            g_sharedStateR = o_StateCreator(nullptr, arg, nullptr, nullptr, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_sharedStateR = nullptr; }
        char l[190];
        sprintf_s(l, "[OLVR][S3] right-eye SHARED view state %s (%p) - occlusion history is now "
                     "per-eye", g_sharedStateR ? "created" : "creation FAILED", g_sharedStateR);
        OLProxyLog(l);
    }
    if (g_sharedStateR) { InterlockedIncrement(&g_sharedGaveOurs); return g_sharedStateR; }
    return orig;
}

// ---- parallax --------------------------------------------------------------------------
// The eye offset is applied AT THE SOURCE, not by patching the built FSceneView afterwards.
// CalcSceneView gets its camera from GetPlayerViewPoint(Actor*, FVector& OutLoc, FRotator& OutRot);
// hooking that and shifting OutLoc during the right-eye pass makes the engine build the ENTIRE
// second view from a genuinely moved camera - view matrix, translated view, projection,
// culling, all of it consistent. Patching PreViewTranslation/ViewOrigin on the finished view
// (the ME2/ME3 approach) means also recomputing every derived matrix by hand and knowing four
// more field offsets. Same result, far less that can silently go stale.
//
// This is a true camera move, so it produces true parallax. It is NOT projection shear /
// convergence, which is a known dead end on this project (it shears the image and breaks fusion).
typedef void (__fastcall *GetViewPoint_t)(void*, float*, int*);
static GetViewPoint_t o_GetViewPoint = nullptr;

static volatile float g_halfEyeUU = 1.6f;  // half-IPD in Unreal units; tune live with NUM1/NUM3
static volatile long  g_eyeSwap = 0;       // flips which side gets the + shift

// ---- 6DOF head tracking --------------------------------------------------------------------
// Until now NOTHING consumed the head pose (ol_xr.h said so in a comment): the pose was located,
// logged, and handed to the compositor as the layer pose - while the game kept rendering from the
// gamepad camera. Declaring "rendered from the head pose" over an image that was NOT is exactly
// what makes the world feel nailed to your face: turn your head and nothing moves relative to you.
//
// Fix: inject the head delta INTO the camera at the source, the same place the eye offset goes.
// The engine then builds the whole view - matrices, culling, projection - from a genuinely
// head-tracked camera, and the declared pose finally describes what was rendered.
//
// Scope: ONLY during the mod's own view-building passes (g_eyePhase != 0). GetPlayerViewPoint is the
// engine's general "where is this actor looking" call; AI, audio and aim must keep the gamepad
// answer. Head moves the CAMERA, not the character.
//
// Rotation conventions, derived not guessed:
//   OpenXR is right-handed, +X right, +Y up, -Z forward. Turning LEFT is a positive rotation
//   about +Y, and g_olYawRad = atan2(fwd.x, -fwd.z) evaluates to -theta for that turn, so a left
//   turn reads NEGATIVE. UE3 yaw is measured CCW from +X toward +Y (the existing right-axis code
//   below assumes exactly that), so turning left INCREASES yaw. Hence UE3 yaw -= g_olYawRad.
//   Pitch: g_olPitchRad = asin(fwd.y) is positive looking up; UE3 pitch is positive up. Same sign.
//   Roll: both systems make "head tilted left" positive (XR: +Z is backwards, so +roll carries
//   +X toward +Y; UE3: roll about forward carries right toward up). Same sign - this is the one
//   axis whose convention is not pinned by existing code, so it is the first suspect if tilting
//   your head tilts the world the wrong way.
//
// Scale: 1 metre of real head movement = g_worldToMeters Unreal units. 50 uu/m is the UE3-era
// human-scale convention AND it is what the existing half-IPD implies: 1.6 uu * 2 = 3.2 uu =
// 6.4 cm at 50 uu/m, a textbook IPD. The two numbers must always agree - if the world ever feels
// the wrong SIZE, these move together.
static volatile float g_worldToMeters = 50.0f;
static volatile long  g_headTrack = 1;

// [LEAN100] THE SCALE WAS THE BUG. Measured 2026-08-17 by the [CAMPROBE] run, three independent
// confirmations: camera sits 168 uu above the pawn root standing (eye height 1.68 m), walking
// logs 210-300 uu/s (2.1-3.0 m/s, human walking pace), and the hand-tuned HalfEyeUU 3.295 makes
// the eyes 6.6 uu apart = 6.6 cm at 100 uu/m, matching the measured 6.97 cm IPD. Outlast is
// ~100 uu/m. Every earlier lean build (50-95 uu/m) rendered HALF the movement the declared pose
// promised, so the compositor had nothing to correct and the world dragged with the head by
// half of every motion, reading as unstable and not moving forward correctly.
// The same run killed the animation theory: standing still the camera moves 0.35 uu per 10 s
// (3 mm) and breathes +/-0.6 deg of pitch. There is no idle bob to fight. No filters, no
// steadying, no forward emphasis, no trigger-driving - position raw at the true scale,
// declared == rendered, and the compositor closes the seam.
static const float kMetersToUU = 100.0f;  // measured, not assumed - see [CAMPROBE] note above
static const float kPosSanityM = 2.0f;    // per-axis sanity clamp, nothing tighter
static volatile float g_leanGain = 1.0f;  // 1.0 = declared==rendered exactly; other values trade
                                          // that honesty for taste, keep the default
extern "C" float OLUE3_GetLeanGain() { return g_leanGain; }
extern "C" void  OLUE3_SetLeanGain(float v) {
    if (v < 0.5f) v = 0.5f; if (v > 2.0f) v = 2.0f;
    g_leanGain = v;
}

// HEAD ROLL - OFF (2026-07-22: a head tilt must produce no change on screen).
//
// This flag governs BOTH the camera injection here AND the roll component of the pose declared
// to the compositor (ol_xr.cpp asks via OLUE3_HeadRollEnabled). It has to be one flag: declared
// must equal rendered. Cancel roll on only one side and the compositor rotates the flat eye
// image to reconcile the difference - the picture visibly spins as you tilt, with empty wedges
// swinging in at the corners. With roll off on BOTH sides there is nothing to reconcile and a
// head tilt changes nothing on screen.
static volatile long g_headRoll = 0;
extern "C" int OLUE3_HeadRollEnabled() { return g_headRoll ? 1 : 0; }

// [LEAN100] enable flag for the camera lean. Applies only once the position baseline is armed
// ([HEADPOS], ol_xr.cpp); off = the camera is byte-for-byte where Outlast puts it.
static volatile long g_headPos = 1;
extern "C" int OLXR_PosArmed();

void OLUE3_ToggleHeadPos() {
    g_headPos = g_headPos ? 0 : 1;
    char line[190];
    sprintf_s(line, "[OLVR][HEAD] positional tracking %s", g_headPos
        ? "ON - lean/duck moves the camera at the measured 100 uu/m once the baseline is armed"
        : "OFF - camera stays exactly where Outlast puts it; rotation still tracks");
    OLProxyLog(line);
}

void OLUE3_ToggleHeadTrack() {
    g_headTrack = g_headTrack ? 0 : 1;
    char line[160];
    sprintf_s(line, "[OLVR][HEAD] head tracking %s (halfEye=%.2f uu)",
              g_headTrack ? "ON" : "OFF - camera is gamepad-only", g_halfEyeUU);
    OLProxyLog(line);
}
void OLUE3_AdjustWorldScale(float mul) {
    float v = g_worldToMeters * mul;
    if (v < 5.0f) v = 5.0f;
    if (v > 500.0f) v = 500.0f;
    g_worldToMeters = v;
    char line[160];
    sprintf_s(line, "[OLVR][HEAD] worldToMeters = %.1f uu/m (1 m of head motion = %.1f uu; "
                    "matching IPD would be halfEye %.2f uu)", v, v, v * 0.032f);
    OLProxyLog(line);
}

void OLUE3_AdjustEye(float delta) {
    float v = g_halfEyeUU + delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 30.0f) v = 30.0f;
    g_halfEyeUU = v;
    char line[128];
    sprintf_s(line, "[OLVR][S3] halfEye = %.2f uu (IPD = %.2f uu)", v, v * 2.0f);
    OLProxyLog(line);
}
// ---- accessors for the in-headset menu (ol_menu.cpp) ------------------------------------------
// Deliberately clamped HERE rather than in the menu: these are the only writers, so a bad value
// cannot reach the camera hook no matter who calls them.
extern "C" float OLUE3_GetHalfEye() { return g_halfEyeUU; }
extern "C" void  OLUE3_SetHalfEye(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 30.0f) v = 30.0f;
    g_halfEyeUU = v;
}
extern "C" float OLUE3_GetWorldToMeters() { return g_worldToMeters; }
extern "C" void  OLUE3_SetWorldToMeters(float v) {
    if (v < 5.0f) v = 5.0f; if (v > 500.0f) v = 500.0f;
    g_worldToMeters = v;
}
extern "C" int  OLUE3_GetHeadTrack() { return g_headTrack ? 1 : 0; }
extern "C" void OLUE3_SetHeadTrack(int v) { g_headTrack = v ? 1 : 0; }
extern "C" int  OLUE3_GetHeadPos() { return g_headPos ? 1 : 0; }
extern "C" void OLUE3_SetHeadPos(int v) { g_headPos = v ? 1 : 0; }
extern "C" int  OLUE3_GetHeadRoll() { return g_headRoll ? 1 : 0; }
extern "C" void OLUE3_SetHeadRoll(int v) { g_headRoll = v ? 1 : 0; }
extern "C" int  OLUE3_GetEyeSwap() { return g_eyeSwap ? 1 : 0; }
extern "C" void OLUE3_SetEyeSwap(int v) { g_eyeSwap = v ? 1 : 0; }

void OLUE3_SwapEyes() {
    g_eyeSwap = g_eyeSwap ? 0 : 1;
    OLProxyLog(g_eyeSwap ? "[OLVR][S3] eyes SWAPPED" : "[OLVR][S3] eyes normal");
}

// ---- [CINEFOV] true VR cutscenes -----------------------------------------------------------
// "I am not in the world, it's like looking through a flat screen" (2026-07-23). Cutscenes and
// the menus are LIVE renders (the split runs during them) but at ~58 and ~33 deg/eye against
// gameplay's 88, and a narrow render can only ever be PRESENTED as a box. So widen the render.
//
// THREE ROUNDS OF GUESSING AT MEMORY, ALL FAILED, ALL LOGGED IN THE HANDOFF:
//   r1 scan for "a float that tracks the FOV" -> never converged, silently did nothing.
//   r2 same, self-verifying -> wedged after one probe (restore keyed off the locked offset,
//      which is 0 while still searching, so the patched flag never cleared).
//   r3 search for the camera POV struct {Location, Rotation, FOV} by matching the Location
//      GetPlayerViewPoint just returned -> "POV signature not found", every time. Outlast
//      drives the view from a camera BONE (OLGame.ini CameraBoneName=Hero-Camera), so the
//      view point is computed from animation each frame and never sits in memory to match.
//
// Then I stopped guessing and read the disassembly, which is what this project's own playbook
// says to do. ULocalPlayer::CalcSceneView (0x62A020) builds the projection as
// tanf(FOV * PI/360) where FOV is the RETURN VALUE of a getter at 0x60F4F0 - which is not a
// field read at all, it is an UnrealScript event dispatch (ProcessEvent -> GetFOVAngle). That
// is why no memory scan could ever have found it.
//
// So hook the getter. It returns a float in XMM0 (verified in the disassembly:
// `MOVSS XMM0,[RSP+0x30]` then RET, and the caller does `MOVAPS XMM13,XMM0`). During these view
// passes only, hand back a widened angle; every other caller of that getter - and there are 4
// others - sees the game's real value. Nothing is written to game memory, so there is nothing
// to restore and nothing that can be left inconsistent if this is interrupted.
//
// Target = kCineTargetDeg per eye, i.e. literally what gameplay renders ("like gameplay" is
// the ask). Applied ONLY when the frame would otherwise come out below kCineNarrowDeg, so
// gameplay is untouched. The camera angle needed for a given per-eye angle goes through the
// engine's own measured mapping k = tan(eye/2)/tan(cam/2), which self-corrects every frame it
// widens from what the projection matrix actually came out as.
static const float kCineTargetDeg = 88.0f;   // per-eye goal = the confirmed-good gameplay width
static const float kCineNarrowDeg = 80.0f;   // only frames below this get widened
static const uintptr_t kFovGetterRva = 0x60F4F0;
static const unsigned char kFovGetterPrologue[13] = {
    0x48,0x89,0x5C,0x24,0x10, 0x57, 0x48,0x83,0xEC,0x20, 0x0F,0x57,0xC0
};

static volatile long g_cineFovOn = 1;
// [WIDEFOV] Render at the HEADSET's width instead of stopping at gameplay's 88. The 88 target
// leaves a shortfall against a ~94-99 deg eye, which is the black wedge on Quest's fitted
// frustum and the crop on SteamVR - and filling it is how the world gets BIGGER without
// touching eye separation (scale rides IPD alone; width rides this). Costs sharpness: the same
// 1920px half now spans more degrees. [Render] WideFov=1, default OFF - with it off every path
// below is byte-identical to the shipped behaviour.
static volatile long g_wideFov = 0;
extern "C" void OLXR_GetHmdFovDeg(float* fx, float* fy);
static float g_fovK = 0.560f;              // tan(eye/2)/tan(cam/2), measured live
static float g_fovWrittenTan = 0.0f;       // tan(written/2); consumed by CaptureRenderedFov
static long  g_fovHookOk = 0;
static long  g_fovApplied = 0;

typedef float (__fastcall *GetFovAngle_t)(void*);
static GetFovAngle_t o_GetFovAngle = nullptr;

extern "C" int  OLUE3_GetCineFov() { return g_cineFovOn ? 1 : 0; }
extern "C" void OLUE3_SetCineFov(int v) { g_cineFovOn = v ? 1 : 0; }
extern "C" int  OLUE3_CineFovState() { return g_fovHookOk ? (g_cineFovOn ? 2 : 0) : 1; }

// The per-eye angle everything gets widened toward. Default = gameplay's confirmed-good 88;
// [WIDEFOV] moves it to the headset's own measured width so the render fills the eye.
static float EyeTargetDeg() {
    if (g_wideFov) {
        float hx = 0.0f, hy = 0.0f;
        OLXR_GetHmdFovDeg(&hx, &hy);
        // Sane band only: 0 means the runtime has not answered yet, and anything huge would
        // ask the engine for a camera past the tan blow-up.
        if (hx > 60.0f) return (hx > 130.0f) ? 130.0f : hx;
    }
    return kCineTargetDeg;
}

// The camera angle to ask for so the ENGINE renders targetDeg per eye.
static float CineFovWriteDeg(float targetDeg) {
    float k = g_fovK;
    if (k < 0.3f || k > 1.0f) k = 0.560f;
    float w = 2.0f * atanf(tanf(targetDeg * 0.00872665f) / k) * 57.2957795f;
    if (w > 165.0f) w = 165.0f;
    return w;
}

static float __fastcall Hook_GetFovAngle(void* self) {
    const float real = o_GetFovAngle(self);
    // ONLY inside the mod's own view-building passes. Aim, HUD and script logic keep the real value.
    if (!g_cineFovOn || !g_eyePhase) return real;
    if (!(real > 5.0f && real < 171.0f)) return real;
    __try {
        // Would this camera come out narrower than gameplay? Judge it through the same mapping
        // the engine demonstrably uses, so the decision is about the RENDERED angle, not the
        // camera number (a 90 deg camera renders ~58 deg/eye here - that is the whole problem).
        const float wouldRenderDeg = 2.0f * atanf(g_fovK * tanf(real * 0.00872665f)) * 57.2957795f;
        const float targetDeg = EyeTargetDeg();
        // [WIDEFOV] off: the shipped rule, gameplay (>= kCineNarrowDeg) is left exactly alone
        // and only narrow cine/menu frames are pulled up to 88. On: one rule for every frame -
        // anything narrower than the headset is widened to the headset, gameplay's 88 included.
        // The near-target guard keeps the writes from flapping as k self-corrects.
        if (!g_wideFov && wouldRenderDeg >= kCineNarrowDeg) return real;
        if (wouldRenderDeg >= targetDeg - 1.0f) return real;
        const float w = CineFovWriteDeg(targetDeg);
        if (w <= real + 2.0f) return real;                      // only ever widen
        g_fovWrittenTan = tanf(w * 0.00872665f);
        const long n = InterlockedIncrement(&g_fovApplied);
        if (n == 1 || (n % 3000) == 0) {
            char l[190];
            sprintf_s(l, "[OLVR][%s] widening: camera %.0f -> %.0f deg (would render %.0f, "
                         "target %.0f deg/eye) frames=%ld", g_wideFov ? "WIDEFOV" : "CINEFOV",
                      real, w, wouldRenderDeg, targetDeg, n);
            OLProxyLog(l);
        }
        return w;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return real; }
}

// The mapping constant, measured live: the requested angle and what the engine actually
// rendered (off the projection matrix) are both known. Their tan ratio IS k.
static void g_cineMeasureK(float eyeTan) {
    if (g_fovWrittenTan > 0.01f) {
        const float k = eyeTan / g_fovWrittenTan;
        if (k > 0.25f && k < 1.2f) g_fovK += (k - g_fovK) * 0.2f;
        g_fovWrittenTan = 0.0f;
    }
}

// ---- [NVLIGHT] camcorder / night-vision light follows the head ------------------------------
// Reported: in camcorder mode with night vision on, head tracking does not move the light.
//
// MECHANISM (disassembled, not guessed - the earlier guess of "a screen-space post pass"
// was WRONG). The NV light is a real world SPOTLIGHT owned by the player pawn: OLGame.ini
// [OLGame.OLHero] carries NVLightZoomedInInnerAngle / OuterAngle / Radius / Brightness, i.e.
// spotlight cone parameters. UnrealScript aims it through USpotLightComponent::SetRotation,
// whose native implementation is RVA 0x2CA9F0 (found via the exec-name registration table:
// 'USpotLightComponentexecSetRotation' -> exec 0x428960 -> real setter):
//     void SetRotation(UStructComponent* this, FRotator* rot)  // writes +0x26C/+0x270/+0x274
// FRotator is {Pitch, Yaw, Roll} as ints - the same layout Hook_GetViewPoint already uses.
//
// Why the light diverges: head rotation is injected ONLY during the mod's own view-building
// passes (g_eyePhase != 0), deliberately, so AI/audio/aim keep the gamepad answer - "head moves the
// CAMERA, not the character". The light is aimed by the BODY during the game tick, so turning
// your head swings the view while the lit cone stays where the body points.
//
// FIX: correct the rotation IN FLIGHT as a parameter. The FRotator is copied, the same head
// delta the camera got is added, and the copy is handed to the engine - so nothing is ever
// written into a live engine object directly; the engine's own setter does its normal work.
//
// SCOPING - this must not rotate level spotlights. A light that is aimed along the player's view
// IS the camcorder/NV light; a level light points somewhere arbitrary. So the delta is applied
// only when the requested rotation is within kNvAimTolUU of the player's CURRENT gamepad view
// rotation (captured pre-injection in Hook_GetViewPoint). Anything else passes through untouched.
static volatile long g_nvLightOn = 1;
static const uintptr_t kSpotSetRotRva = 0x2CA9F0;
static const unsigned char kSpotSetRotPrologue[8] = { 0x8B,0x02, 0x89,0x81,0x6C,0x02,0x00,0x00 };
// IDENTIFYING THE PLAYER'S LIGHTS - from the live census, not a guess. The player's lights are
// set with { Pitch = the aim's pitch exactly, Yaw ~ 0, Roll = 0 }: the component rotation is
// RELATIVE to the pawn, which already carries the body yaw, so script only has to supply pitch.
// Measured over ~8000 calls: two player lights (pitch offset 0..594 uu, yaw -6..+2) and one
// level light (pitch offset 2000-3500 uu, 75 calls total, does not track). So the test is
// "pitch TRACKS the aim AND yaw is ~0" - which is also why the first attempt barely fired: it
// compared yaw against the ABSOLUTE aim, so it only matched while the body faced yaw~0.
static const int kNvPitchTolUU = 1200;      // ~6.6 deg - player lights sat inside 600, level at 2000+
static const int kNvYawZeroUU  = 900;       // ~5 deg  - the relative yaw is ~0 for a player light

typedef void (__fastcall *SpotSetRot_t)(void*, int*);
static SpotSetRot_t o_SpotSetRot = nullptr;

// (no learned-component state: the correction is applied per call, in flight)
static volatile long g_nvHits = 0;

extern "C" int  OLUE3_GetNvLight() { return g_nvLightOn ? 1 : 0; }
extern "C" void OLUE3_SetNvLight(int v) { g_nvLightOn = v ? 1 : 0; }

// Signed wrap for UE3's 16-bit-style angles, so comparisons work across the 0/65536 seam.
static int WrapUE(int d) { d &= 0xFFFF; if (d > 32768) d -= 65536; return d; }

static bool NvHeadDelta(int* outPitch, int* outYaw) {
    if (!g_headTrack || !g_olPoseValid) return false;
    const float w = g_injW;                      // 0 while the cinema screen is up
    if (w <= 0.0f) return false;
    const float kToUnits = 65536.0f / 6.28318531f;
    *outPitch = (int)(g_olPitchRad * w * kToUnits);
    *outYaw   = (int)(g_olYawRad   * w * kToUnits);
    return true;
}

// [NVLIGHT] TELEMETRY. The hook fires and matches, but the light still does not follow the head
// live - so the assumption that "this spotlight's Rotation is what aims the beam" is unproven.
// Census every distinct component that gets SetRotation, with its rotation vs the player's aim,
// so one run says which light is actually being moved and whether anything else is driving it.
static void*   g_nvSeen[6]   = { 0 };
static long    g_nvSeenN[6]  = { 0 };
static int     g_nvSeenRot[6][3] = { { 0 } };
static int     g_nvDistinct  = 0;
static long    g_nvCalls     = 0;
static long    g_nvMatched   = 0;

static void NvCensus(void* comp, const int* rot) {
    int idx = -1;
    for (int i = 0; i < g_nvDistinct; ++i) if (g_nvSeen[i] == comp) { idx = i; break; }
    if (idx < 0 && g_nvDistinct < 6) { idx = g_nvDistinct++; g_nvSeen[idx] = comp; g_nvSeenN[idx] = 0; }
    if (idx >= 0) {
        ++g_nvSeenN[idx];
        g_nvSeenRot[idx][0] = rot[0]; g_nvSeenRot[idx][1] = rot[1]; g_nvSeenRot[idx][2] = rot[2];
    }
}

extern "C" void OLUE3_NvLightReport() {
    if (!g_nvCalls) { OLProxyLog("[OLVR][NVCENSUS] SetRotation has NEVER been called - this is not "
                                 "how the NV light is aimed"); return; }
    char l[240];
    sprintf_s(l, "[OLVR][NVCENSUS] calls=%ld matched=%ld distinct=%d  baseView(P=%d Y=%d) head(P=%d Y=%d)",
              g_nvCalls, g_nvMatched, g_nvDistinct, g_baseViewRot[0], g_baseViewRot[1],
              (int)(g_olPitchRad * (65536.0f / 6.28318531f)), (int)(g_olYawRad * (65536.0f / 6.28318531f)));
    OLProxyLog(l);
    for (int i = 0; i < g_nvDistinct; ++i) {
        sprintf_s(l, "[OLVR][NVCENSUS]   comp %p calls=%ld lastRot(P=%d Y=%d R=%d) offFromAim(P=%d Y=%d)",
                  g_nvSeen[i], g_nvSeenN[i], g_nvSeenRot[i][0], g_nvSeenRot[i][1], g_nvSeenRot[i][2],
                  WrapUE(g_nvSeenRot[i][0] - g_baseViewRot[0]), WrapUE(g_nvSeenRot[i][1] - g_baseViewRot[1]));
        OLProxyLog(l);
    }
}

static void __fastcall Hook_SpotSetRotation(void* comp, int* rot) {
    if (!g_nvLightOn || !rot) { o_SpotSetRot(comp, rot); return; }
    int mod[3];
    bool use = false;
    __try {
        InterlockedIncrement(&g_nvCalls);
        NvCensus(comp, rot);
        int dp = 0, dy = 0;
        if (g_baseViewFresh > 0 && NvHeadDelta(&dp, &dy)) {
            const int offP = WrapUE(rot[0] - g_baseViewRot[0]);   // does the pitch track the aim?
            const int relY = WrapUE(rot[1]);                      // is the yaw relative (~0)?
            if (offP > -kNvPitchTolUU && offP < kNvPitchTolUU &&
                relY > -kNvYawZeroUU  && relY < kNvYawZeroUU) {
                InterlockedIncrement(&g_nvMatched);
                // The yaw is RELATIVE to the body, and the head yaw is also measured from the
                // body's forward (the recenter baseline) - so it adds directly. Pitch is the
                // aim's pitch, and the head pitch is likewise a delta on it. Both just add.
                mod[0] = rot[0] + dp; mod[1] = rot[1] + dy; mod[2] = rot[2];
                use = true;
                if (InterlockedIncrement(&g_nvHits) == 1)
                    OLProxyLog("[OLVR][NVLIGHT] player light(s) identified (pitch tracks the aim, "
                               "yaw is body-relative) - they now follow your head");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { use = false; }
    o_SpotSetRot(comp, use ? mod : rot);
}

// NO per-frame re-drive, deliberately. The first cut re-applied the rotation every Present in
// case script only set it when the aim moved - but the census settles it: 7435 calls across
// ~8200 frames on the main light, i.e. script drives these lights unconditionally every frame.
// So the parameter correction above is sufficient, and dropping the re-drive removes the only
// place this feature wrote into a live engine object (and the only stale-pointer risk with it).

// ---- [CAMPROBE] logging-only camera decomposition probe ----------------------------------------
// Answers, with numbers instead of another feel-build: what does the Hero-Camera bone actually do
// per state (idle / walk / camcorder up / trigger lean), and is there a readable PAWN ROOT whose
// motion is pure locomotion? If camera = root + animation, the animation term can be scaled
// exactly (bob/sway off, locomotion untouched) and a stable camera lean finally has a base.
// Armed by [Stage3] CamProbe=1, default off = this block costs one integer compare per frame.
// Output: %LOCALAPPDATA%\OutlastVR\CamProbe.csv - per frame the base camera pos/rot (pre-
// injection) + rendered FOV, plus up to 8 candidate "root" triplets found by scanning the hooked
// actor object (direct fields at frame 600, one pointer hop at frame 660 - PC -> Pawn -> Location
// is one hop). Candidate identities (offsets) go to the main log; the CSV carries their values
// every frame so bob-vs-flat separates them in analysis. Row cap 30k (~8 min), then it stops.
static long  g_camProbe = 0;
static FILE* g_probeFile = nullptr;
static long  g_probeRows = 0, g_probeFrame = 0;
static struct { int ptrHop; unsigned offA, offB; } g_probeCand[8];
static int   g_probeCandN = 0;

static int ProbeReadF3(const void* addr, float* out) {
    __try {
        const float* f = (const float*)addr;
        const float a = f[0], b = f[1], c = f[2];
        if (!(a == a) || !(b == b) || !(c == c)) return 0;                       // NaN
        if (fabsf(a) > 1e7f || fabsf(b) > 1e7f || fabsf(c) > 1e7f) return 0;     // not a world pos
        out[0] = a; out[1] = b; out[2] = c; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* ProbeReadPtr(const void* addr) {
    __try {
        void* p = *(void* const*)addr;
        if ((uintptr_t)p < 0x10000 || (uintptr_t)p > 0x00007FFFFFFF0000ull) return nullptr;
        return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static int ProbeNear(const float* v, const float* cam) {
    const float dx = v[0] - cam[0], dy = v[1] - cam[1], dz = v[2] - cam[2];
    if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) return 0;
    return dx * dx + dy * dy + dz * dz <= 400.0f * 400.0f;   // within 4 m of the camera
}
static float g_probeCandV[8][3];   // value at record time, to reject duplicate finds
static void ProbeScan(void* actor, const float* cam, int ptrHop) {
    char l[192];
    unsigned nextDirectOk = 0;
    for (unsigned offA = 0; offA <= 0x7F8 && g_probeCandN < 8; offA += (ptrHop ? 8 : 4)) {
        void* base = actor;
        if (ptrHop) {
            base = ProbeReadPtr((char*)actor + offA);
            if (!base || base == actor) continue;
        } else if (offA < nextDirectOk) {
            continue;                        // inside the previous hit's triplet, skip
        }
        int fromThisPtr = 0;
        for (unsigned offB = 0; offB <= (ptrHop ? 0x3F8u : 0u) && g_probeCandN < 8; offB += 4) {
            float v[3];
            const unsigned off = ptrHop ? offB : offA;
            if (!ProbeReadF3((char*)base + off, v) || !ProbeNear(v, cam)) continue;
            // The same location reached through two different pointers is not two separate findings.
            int dup = 0;
            for (int i = 0; i < g_probeCandN; ++i) {
                const float ex = v[0] - g_probeCandV[i][0], ey = v[1] - g_probeCandV[i][1],
                            ez = v[2] - g_probeCandV[i][2];
                if (ex * ex + ey * ey + ez * ez < 0.25f) { dup = 1; break; }
            }
            if (dup) { offB += 8; continue; }
            g_probeCand[g_probeCandN].ptrHop = ptrHop;
            g_probeCand[g_probeCandN].offA = offA;
            g_probeCand[g_probeCandN].offB = ptrHop ? off : 0;
            g_probeCandV[g_probeCandN][0] = v[0]; g_probeCandV[g_probeCandN][1] = v[1];
            g_probeCandV[g_probeCandN][2] = v[2];
            if (ptrHop) sprintf_s(l, "[OLVR][CAMPROBE] c%d = ptr@+0x%X -> +0x%X = (%.1f, %.1f, %.1f)  below cam %.1f uu",
                                  g_probeCandN, offA, off, v[0], v[1], v[2], cam[2] - v[2]);
            else        sprintf_s(l, "[OLVR][CAMPROBE] c%d = actor+0x%X = (%.1f, %.1f, %.1f)  below cam %.1f uu",
                                  g_probeCandN, offA, v[0], v[1], v[2], cam[2] - v[2]);
            OLProxyLog(l);
            ++g_probeCandN;
            offB += 8;                       // don't re-hit the same triplet at +4
            if (!ptrHop) { nextDirectOk = offA + 12; break; }
            if (++fromThisPtr >= 2) break;   // max 2 per pointer, keep variety
        }
    }
}
static void ProbeTick(void* actor, const float* loc, const int* rot) {
    if (g_probeRows >= 30000) return;
    if (!g_probeFile) {
        wchar_t path[MAX_PATH]; wchar_t* lad = _wgetenv(L"LOCALAPPDATA");
        if (!lad) { g_probeRows = 30000; return; }
        swprintf_s(path, L"%s\\OutlastVR\\CamProbe.csv", lad);
        g_probeFile = _wfsopen(path, L"w", _SH_DENYWR);
        if (!g_probeFile) { g_probeRows = 30000; return; }
        fprintf(g_probeFile, "ms,frame,camX,camY,camZ,pitch,yaw,roll,fovXdeg,"
                "hX,hY,hZ,armed,hPitchDeg,hYawDeg,"
                "c0x,c0y,c0z,c1x,c1y,c1z,c2x,c2y,c2z,c3x,c3y,c3z,"
                "c4x,c4y,c4z,c5x,c5y,c5z,c6x,c6y,c6z,c7x,c7y,c7z\n");
        OLProxyLog("[OLVR][CAMPROBE] armed - logging the base camera to CamProbe.csv "
                   "(root-candidate scan at ~10s and ~11s in)");
    }
    ++g_probeFrame;
    if (g_probeFrame == 600) ProbeScan(actor, loc, 0);   // direct fields on the hooked actor
    if (g_probeFrame == 660) {
        ProbeScan(actor, loc, 1);                        // one pointer hop (PC -> Pawn -> Location)
        char l[96]; sprintf_s(l, "[OLVR][CAMPROBE] scan done, %d candidate(s) in the CSV", g_probeCandN);
        OLProxyLog(l);
    }
    float fx = 0.0f, fyv = 0.0f; OLUE3_GetEyeFov(&fx, &fyv);
    // [CAMPROBE v2] the HEAD-POSITION FEED itself, next to the camera it drives. Reports of an
    // apparent movement limit while sitting still tie to these values reaching METRES -
    // this is the column set that convicts the feed (origin jump / prediction overshoot) or
    // clears it (pitch-correlated pumping = neck-orbit double-count).
    fprintf(g_probeFile, "%lu,%ld,%.2f,%.2f,%.2f,%d,%d,%d,%.1f,%.4f,%.4f,%.4f,%d,%.2f,%.2f",
            GetTickCount(), g_probeFrame, loc[0], loc[1], loc[2], rot[0], rot[1], rot[2],
            fx * 57.2957795f,
            g_olHeadX, g_olHeadY, g_olHeadZ, OLXR_PosArmed(),
            g_olPitchRad * 57.2957795f, g_olYawRad * 57.2957795f);
    for (int i = 0; i < 8; ++i) {
        float v[3]; int ok = 0;
        if (i < g_probeCandN) {
            void* base = actor;
            unsigned off = g_probeCand[i].offA;
            if (g_probeCand[i].ptrHop) {
                base = ProbeReadPtr((char*)actor + g_probeCand[i].offA);
                off = g_probeCand[i].offB;
            }
            if (base) ok = ProbeReadF3((char*)base + off, v);
        }
        if (ok) fprintf(g_probeFile, ",%.2f,%.2f,%.2f", v[0], v[1], v[2]);
        else    fprintf(g_probeFile, ",,,");
    }
    fprintf(g_probeFile, "\n");
    if ((++g_probeRows % 60) == 0) fflush(g_probeFile);
}

// ---- [BONEPROBE] find Miles's skeleton: the ground truth for real arm motion controls ----------
// Goal: moving the controller moves Miles's arms. That needs three addresses this
// probe exists to find, log-only, one shot:
//   1. GNames - UE3's global name table, so FName indices become readable strings;
//   2. the RefSkeleton bone-name array of the skeletal mesh the view lives in (CameraBoneName=
//      Hero-Camera, so the array CONTAINING a name like that is the one the camera rides - the
//      same skeleton as the arms);
//   3. bone-count-sized transform arrays on the owning component (SpaceBases/LocalAtoms are in
//      the exe's strings) - the eventual WRITE target.
// Nothing is assumed about struct layouts: the bone array is found by trying stride x name-offset
// grids and demanding that 7 of 8 consecutive elements resolve to printable names - the same
// runtime-discovery style that found ME3's HUD element table.
// Armed by [Stage3] BoneProbe=1, default off. Everything is SEH-guarded reads; nothing is written.
static long  g_boneProbe = 0;
static void** g_bpNames = nullptr;     // GNames: flat FNameEntry* array
static unsigned g_bpNameSoff = 0;      // offset of the ANSI string inside an FNameEntry
static int   g_bpBoneCount = 0;        // bone count of the identified view skeleton

static int BP_Read(const void* a, void* out, size_t n) {
    __try { memcpy(out, a, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int BP_Printable(const char* s, int cap) {
    if (s[0] < 'A' || (s[0] > 'Z' && s[0] < 'a') || s[0] > 'z') return 0;   // names start with a letter
    for (int i = 1; i < cap; ++i) {
        const char c = s[i];
        if (!c) return 1;
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 0;
}
static const char* BP_Name(uint32_t idx, char* buf, size_t bufn) {
    if (!g_bpNames || idx > 2u * 1000u * 1000u) return nullptr;
    void* e = nullptr;
    if (!BP_Read(g_bpNames + idx, &e, 8) || !e) return nullptr;
    char tmp[64];
    if (!BP_Read((char*)e + g_bpNameSoff, tmp, sizeof(tmp))) return nullptr;
    tmp[63] = 0;
    if (!BP_Printable(tmp, 63)) return nullptr;
    strcpy_s(buf, bufn, tmp);
    return buf;
}

// GNames: scan the exe's writable sections for a pointer to an array whose entry 0 carries the
// string "None" and entry 1 "ByteProperty" - indices 0 and 1 of every UE3 name table, which makes
// false positives effectively impossible.
static void BP_FindGNames() {
    char l[160];
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!BP_Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return;
    if (!BP_Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)(base + dos.e_lfanew +
        FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER sh;
        if (!BP_Read(sec + s, &sh, sizeof(sh))) continue;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* p = base + sh.VirtualAddress;
        const uint8_t* end = p + sh.Misc.VirtualSize - 8;
        for (; p < end; p += 8) {
            void** cand = nullptr;
            if (!BP_Read(p, &cand, 8)) { p += 0x10000 - 8; continue; }   // unreadable page: skip ahead
            if ((uintptr_t)cand < 0x10000 || (uintptr_t)cand > 0x00007FFFFFFF0000ull) continue;
            void* e0 = nullptr, *e1 = nullptr;
            if (!BP_Read(cand, &e0, 8) || !e0 || !BP_Read(cand + 1, &e1, 8) || !e1) continue;
            char h0[0x40], h1[0x40];
            if (!BP_Read(e0, h0, sizeof(h0)) || !BP_Read(e1, h1, sizeof(h1))) continue;
            for (unsigned off = 0; off <= 0x38 - 5; ++off) {
                if (memcmp(h0 + off, "None", 5) == 0 && memcmp(h1 + off, "ByteProperty", 13) == 0) {
                    g_bpNames = cand; g_bpNameSoff = off;
                    sprintf_s(l, "[OLVR][BONEPROBE] GNames @ %p (in-exe slot %p), entry string offset 0x%X",
                              (void*)cand, (const void*)p, off);
                    OLProxyLog(l);
                    return;
                }
            }
        }
    }
    OLProxyLog("[OLVR][BONEPROBE] GNames NOT found - no name table, cannot read bones this way");
}

// Scan one object for a TArray that reads as a bone-name table: {Data, Num, Max} sane, and 7 of
// the first 8 elements resolving to printable names on some stride/name-offset pair.
// Run 1 lesson (2026-08-17): "7 of 8 resolve" alone is a WEAK test - any array holding a
// constant dword that happens to be a valid name index passes it, which buried the log in
// false tables all reading the same name. A real skeleton never repeats a bone name, so the
// test is now 8-of-8 resolving AND at least 6 DISTINCT strings. And run 1's stride grid
// stopped at 0x40 while UE3's x64 FMeshBone (FName + flags + VJointPos + parent + children)
// is ~0x44-0x50 - the real RefSkeleton fell straight through the grid. Extended to 0x70.
static const unsigned kBpStride[]  = { 0x10, 0x14, 0x18, 0x1C, 0x20, 0x28, 0x30, 0x38, 0x40,
                                       0x44, 0x48, 0x4C, 0x50, 0x58, 0x60, 0x68, 0x70 };
static const unsigned kBpNameOff[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20,
                                       0x28, 0x2C, 0x30, 0x38 };
static int BP_ScanForBones(void* obj, const char* how) {
    if (!obj) return 0;
    char l[256], nm[64];
    int found = 0;
    for (unsigned off = 0; off <= 0x900; off += 8) {
        struct { void* d; int n; int mx; } ta;
        if (!BP_Read((char*)obj + off, &ta, 16)) continue;
        if (!ta.d || ta.n < 12 || ta.n > 512 || ta.mx < ta.n || ta.mx > 1024) continue;
        for (unsigned si = 0; si < sizeof(kBpStride) / sizeof(kBpStride[0]); ++si) {
            for (unsigned ni = 0; ni < sizeof(kBpNameOff) / sizeof(kBpNameOff[0]); ++ni) {
                const unsigned stride = kBpStride[si], noff = kBpNameOff[ni];
                if (noff + 4 > stride) continue;
                int good = 0, distinct = 0;
                char seenNm[8][64];
                for (int i = 0; i < 8; ++i) {
                    uint32_t idx = 0;
                    if (!BP_Read((char*)ta.d + (size_t)i * stride + noff, &idx, 4) ||
                        !BP_Name(idx, nm, sizeof(nm))) break;
                    ++good;
                    bool dup = false;
                    for (int j = 0; j < distinct; ++j) if (!strcmp(seenNm[j], nm)) { dup = true; break; }
                    if (!dup) strcpy_s(seenNm[distinct++], nm);
                }
                if (good < 8 || distinct < 6) continue;
                // Real find. Dump the table and look for the camera bone.
                sprintf_s(l, "[OLVR][BONEPROBE] BONE TABLE via %s: obj %p +0x%X  count=%d stride=0x%X nameOff=0x%X",
                          how, obj, off, ta.n, stride, noff);
                OLProxyLog(l);
                int camIdx = -1;
                char line[200]; int ll = 0; line[0] = 0;
                const int dumpN = ta.n < 160 ? ta.n : 160;   // arms can index past 64; dump more
                for (int i = 0; i < dumpN; ++i) {
                    uint32_t idx = 0; nm[0] = 0;
                    BP_Read((char*)ta.d + (size_t)i * stride + noff, &idx, 4);
                    if (!BP_Name(idx, nm, sizeof(nm))) strcpy_s(nm, "?");
                    if (strstr(nm, "amera")) camIdx = i;   // Hero-Camera, camera, Camera...
                    // Flush BEFORE appending when space runs low - sprintf_s on overflow is
                    // process death, not truncation (killed run 4 in the sibling dump).
                    if (ll > 140) { sprintf_s(l, "[OLVR][BONEPROBE]   %.200s", line); OLProxyLog(l); ll = 0; line[0] = 0; }
                    ll += sprintf_s(line + ll, sizeof(line) - ll, "%d:%.40s  ", i, nm);
                }
                if (ll) { sprintf_s(l, "[OLVR][BONEPROBE]   %.200s", line); OLProxyLog(l); }
                if (camIdx >= 0) {
                    sprintf_s(l, "[OLVR][BONEPROBE] *** camera bone at index %d - THIS is the view skeleton "
                                 "(the arms live here too) ***", camIdx);
                    OLProxyLog(l);
                    g_bpBoneCount = ta.n;
                }
                ++found;
                break;
            }
            if (found) break;
        }
    }
    return found;
}

// Transform arrays: any TArray on the object whose Num equals the identified bone count and whose
// data reads as floats. Those are SpaceBases/LocalAtoms shaped - the future write targets.
static void BP_ScanForTransforms(void* obj, const char* how) {
    if (!obj || !g_bpBoneCount) return;
    char l[192];
    for (unsigned off = 0; off <= 0x900; off += 8) {
        struct { void* d; int n; int mx; } ta;
        if (!BP_Read((char*)obj + off, &ta, 16)) continue;
        if (!ta.d || ta.n != g_bpBoneCount || ta.mx < ta.n || ta.mx > 2048) continue;
        float f[8];
        if (!BP_Read(ta.d, f, sizeof(f))) continue;
        int fin = 0;
        for (int i = 0; i < 8; ++i) if (f[i] == f[i] && fabsf(f[i]) < 1e7f) ++fin;
        if (fin < 8) continue;
        sprintf_s(l, "[OLVR][BONEPROBE] transform-shaped array via %s: obj %p +0x%X  num=%d "
                     "(first floats %.2f %.2f %.2f %.2f)", how, obj, off, ta.n, f[0], f[1], f[2], f[3]);
        OLProxyLog(l);
    }
}

// [BONEPROBE r3] Run 2 lesson: blind two-hop pointer walking drowns - the 1024-object budget
// went to UI localization tables and property metadata without ever touching a mesh. The engine
// keeps a global array of EVERY live object (GObjObjects), and with GNames working, it can be
// found the same way GNames was found - by shape - then simply asked for every object named
// *SkeletalMeshComponent*. No hops, no luck.
// GObjObjects discovery: a TArray{UObject** data; int num; int max} in the exe's writable data
// with a big num, where some fixed offset inside the pointed-to objects resolves to printable
// names for nearly every sampled object. That offset IS UObject::Name - discovered, not assumed.
static void** g_bpObjs = nullptr;      // GObjObjects data
static int    g_bpObjNum = 0;
static unsigned g_bpUNameOff = 0;      // UObject::Name offset, discovered by voting

static void BP_FindObjects() {
    char l[192], nm[64];
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!BP_Read(base, &dos, sizeof(dos)) || !BP_Read(base + dos.e_lfanew, &nt, sizeof(nt))) return;
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)(base + dos.e_lfanew +
        FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER sh;
        if (!BP_Read(sec + s, &sh, sizeof(sh))) continue;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* p = base + sh.VirtualAddress;
        const uint8_t* end = p + sh.Misc.VirtualSize - 16;
        for (; p < end; p += 8) {
            struct { void** d; int n; int mx; } ta;
            if (!BP_Read(p, &ta, 16)) { p += 0x10000 - 8; continue; }
            if (!ta.d || ta.n < 5000 || ta.mx < ta.n || ta.mx > 5000000) continue;
            if ((uintptr_t)ta.d < 0x10000 || (uintptr_t)ta.d > 0x00007FFFFFFF0000ull) continue;
            // Sample 16 objects spread across the array; vote for the UObject::Name offset.
            void* smp[16]; int nsmp = 0;
            for (int k = 0; k < 16; ++k) {
                void* o = nullptr;
                if (BP_Read(ta.d + (size_t)k * (ta.n / 16), &o, 8) && o &&
                    (uintptr_t)o > 0x10000 && (uintptr_t)o < 0x00007FFFFFFF0000ull) smp[nsmp++] = o;
            }
            if (nsmp < 12) continue;
            // Run 3 lesson: "resolves to a printable name" alone voted for ObjectInternalInteger -
            // the object's own INDEX in this very array. Every small int resolves to SOME name, so
            // a counter beats the real Name on that test. Two mechanical separators:
            //   - a counter EQUALS its own array slot; a name never tracks the slot;
            //   - real names REPEAT across objects (hundreds share 'Function'); a counter cannot.
            // So: disqualify slot-tracking offsets, then require the winner to actually contain
            // objects named Function - the one name every UE3 registry holds in bulk.
            int slotOf[16];
            for (int k = 0; k < nsmp; ++k) slotOf[k] = k * (ta.n / 16);
            unsigned candOff[4] = {}; int candHits[4] = {}; int ncand = 0;
            for (unsigned no = 0x18; no <= 0xA0; no += 8) {
                int hits = 0, self = 0, rep = 0; uint32_t seenIdx[16]; int nseen = 0;
                for (int k = 0; k < nsmp; ++k) {
                    uint32_t idx = 0;
                    if (!BP_Read((char*)smp[k] + no, &idx, 4)) continue;
                    if ((int)idx == slotOf[k]) ++self;
                    if (!BP_Name(idx, nm, sizeof(nm))) continue;
                    ++hits;
                    for (int j = 0; j < nseen; ++j) if (seenIdx[j] == idx) { ++rep; break; }
                    if (nseen < 16) seenIdx[nseen++] = idx;
                }
                if (hits < nsmp - 2 || self >= nsmp / 2) continue;
                if (ncand < 4) { candOff[ncand] = no; candHits[ncand] = hits + rep; ++ncand; }
            }
            if (!ncand) continue;
            // Validate candidates against the full array: the true Name offset finds 'Function'
            // objects within the first few thousand entries; a wrong one never does.
            for (int ci = 0; ci < ncand; ++ci) {
                int fn = 0;
                const int lim = ta.n < 20000 ? ta.n : 20000;
                for (int i = 0; i < lim && fn <= 10; ++i) {
                    void* o = nullptr; uint32_t idx = 0;
                    if (!BP_Read(ta.d + i, &o, 8) || !o) continue;
                    if (!BP_Read((char*)o + candOff[ci], &idx, 4)) continue;
                    if (BP_Name(idx, nm, sizeof(nm)) && strstr(nm, "Function")) ++fn;
                }
                if (fn <= 10) continue;
                g_bpObjs = ta.d; g_bpObjNum = ta.n; g_bpUNameOff = candOff[ci];
                sprintf_s(l, "[OLVR][BONEPROBE] GObjObjects @ %p num=%d, UObject::Name at +0x%X "
                             "(validated: Function objects present)", (void*)ta.d, ta.n, candOff[ci]);
                OLProxyLog(l);
                // Self-diagnosing sample so a wrong pick is visible in the log without a rebuild.
                // BUFFER RULE, learned the hard way in run 4: sprintf_s TERMINATES THE PROCESS on
                // overflow, it does not truncate - the run-4 probe died exactly here, printing a
                // ~215-char sample line through a 192-byte buffer, one line before the answer.
                // So: check remaining space BEFORE appending, cap every name with %.40s, and
                // print through a buffer sized for the worst case.
                char line[200]; int ll = 0; line[0] = 0;
                for (int k = 0; k < 12 && ll < 140; ++k) {
                    void* o = nullptr; uint32_t idx = 0; nm[0] = 0;
                    BP_Read(ta.d + (size_t)k * (ta.n / 12), &o, 8);
                    if (o && BP_Read((char*)o + g_bpUNameOff, &idx, 4) && BP_Name(idx, nm, sizeof(nm)))
                        ll += sprintf_s(line + ll, sizeof(line) - ll, "%.40s  ", nm);
                }
                char big[256];
                sprintf_s(big, "[OLVR][BONEPROBE] sample names: %.200s", line);
                OLProxyLog(big);
                return;
            }
            continue;
        }
    }
    OLProxyLog("[OLVR][BONEPROBE] GObjObjects NOT found");
}

static const char* BP_ObjName(void* obj, char* buf, size_t bufn) {
    uint32_t idx = 0;
    if (!obj || !BP_Read((char*)obj + g_bpUNameOff, &idx, 4)) return nullptr;
    return BP_Name(idx, buf, bufn);
}

// One shot, from the camera hook's actor. Walk: the actor itself, every object it points at
// (depth 1), and every object THOSE point at (depth 2). The bone table usually sits two hops out
// (PC -> Pawn -> MeshComponent -> SkeletalMesh asset), so depth-2 objects also get one extra hop
// for the bone scan only.
static void BP_Run(void* actor) {
    char l[160];
    OLProxyLog("[OLVR][BONEPROBE] scanning for the skeleton (one-shot, log-only)...");
    BP_FindGNames();
    if (!g_bpNames) return;
    // Three separate buffers - run 1 used one buffer for all three %s and printed the LAST
    // evaluated call three times, which made a working GNames look broken in the log.
    char n0[64], n1[64], n2[64];
    sprintf_s(l, "[OLVR][BONEPROBE] name sanity: 0='%s' 1='%s' 2='%s'",
              BP_Name(0, n0, sizeof(n0)) ? n0 : "?",
              BP_Name(1, n1, sizeof(n1)) ? n1 : "?",
              BP_Name(2, n2, sizeof(n2)) ? n2 : "?");
    OLProxyLog(l);

    (void)actor;
    BP_FindObjects();
    if (!g_bpObjs) return;

    // Every object named *SkeletalMeshComponent* - the class object and every live instance
    // share the name's base string, and Outlast subclasses (OL...) keep the substring. A tiny
    // verdict cache keyed on the FName INDEX keeps this one string-compare per unique name, not
    // per object, across a registry that can run to hundreds of thousands of entries.
    static uint32_t s_key[16384]; static int8_t s_val[16384];
    memset(s_key, 0xFF, sizeof(s_key)); memset(s_val, 0, sizeof(s_val));
    char nm[64];
    void* comps[64]; int nc = 0;
    int scanned = 0;
    for (int i = 0; i < g_bpObjNum && nc < 64; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        uint32_t idx = 0;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        ++scanned;
        const unsigned h = idx & 16383;
        int verdict;
        if (s_key[h] == idx) verdict = s_val[h];
        else {
            verdict = (BP_Name(idx, nm, sizeof(nm)) && strstr(nm, "SkeletalMeshComponent")) ? 1 : 0;
            s_key[h] = idx; s_val[h] = (int8_t)verdict;
        }
        if (verdict) comps[nc++] = o;
    }
    sprintf_s(l, "[OLVR][BONEPROBE] %d objects scanned, %d *SkeletalMeshComponent* found%s",
              scanned, nc, nc == 64 ? " (capped)" : "");
    OLProxyLog(l);

    // For each component: its name, then every object it points at gets the bone-table scan -
    // the SkeletalMesh asset is one hop from its component. The asset's NAME is logged with the
    // table, because the asset name is what identifies which mesh is Miles's arms.
    int hits = 0;
    static void* s_done[512]; int nd = 0;   // same asset hangs off many components/fields: scan once
    auto done = [&](void* p) -> bool {
        for (int i = 0; i < nd; ++i) if (s_done[i] == p) return true;
        if (nd < 512) s_done[nd++] = p;
        return false;
    };
    for (int c = 0; c < nc; ++c) {
        char cn[64];
        if (!BP_ObjName(comps[c], cn, sizeof(cn))) strcpy_s(cn, "?");
        for (unsigned off = 0; off <= 0xB00; off += 8) {
            void* p = ProbeReadPtr((char*)comps[c] + off);
            if (!p || p == comps[c] || done(p)) continue;
            const int before = g_bpBoneCount;
            char how[160], an[64];
            if (!BP_ObjName(p, an, sizeof(an))) an[0] = 0;
            sprintf_s(how, "comp '%.40s' +0x%X -> '%.40s'", cn, off, an[0] ? an : "?");
            const int f = BP_ScanForBones(p, how);
            hits += f;
            // A table appeared under this component: its transform arrays live ON the component.
            if (f && g_bpBoneCount && g_bpBoneCount != before) {
                char tag[96]; sprintf_s(tag, "comp '%s'", cn);
                BP_ScanForTransforms(comps[c], tag);
            }
        }
    }
    sprintf_s(l, "[OLVR][BONEPROBE] done: %d bone table(s), view-skeleton bone count %d",
              hits, g_bpBoneCount);
    OLProxyLog(l);
}

// ---- [WIGGLE] prove the bone write path: oscillate Miles's right arm on purpose ----------------
// BONEPROBE r5 delivered the map: asset 'Miles_beheaded', 73 bones (R-UpperArm 34, R-Forearm 35,
// R-Hand 36), and the owning component carries two 73-element FBoneAtom arrays at +0x2D8/+0x2E8
// (quat 16 + translation 12 + scale 4 = 0x20 stride; first element reads 0,0,0,1 = identity
// quat). This probe WRITES those arrays - a bounded sine on the right arm's translation - and
// answers the question every bone-driving plan lives or dies on: DO THESE WRITES SURVIVE THE
// FRAME, or does the animation recompose over them?
// The answer is measured, not eyeballed: each frame reads back what it wrote last frame. Value
// unchanged = nobody recomposed (sticky); value changed = the anim rewrote it first (anim).
// The 3s log line carries the ratio. Sway, never a constant offset - a constant is invisible
// on success and drifts on failure; a 0.5 Hz 15 uu sway is unmistakable and self-centring.
// Armed by [Stage3] BoneWiggle (0 off, 1 = +0x2D8 only, 2 = +0x2E8 only, 3 = both). Dev only.
static long  g_boneWiggle = 0;
static void* g_wigComp[4]; static int g_wigCompN = 0;
static long  g_wigNext = 0;            // rescan countdown
static long  g_wigSticky = 0, g_wigAnim = 0;
static float g_wigLast[4][2][3];       // [comp][array][bone] last value written
static float g_wigDelta[4][2][3];      // and the delta inside it
static uint32_t g_wigMilesIdx = 0;     // FName index of 'Miles_beheaded', cached

static const unsigned kWigArr[2]  = { 0x2D8, 0x2E8 };
static const int      kWigBone[3] = { 34, 35, 36 };   // R-UpperArm, R-Forearm, R-Hand

static void WigFind() {
    g_wigCompN = 0;
    if (!g_bpObjs || !g_bpNames) return;
    char nm[64];
    for (int i = 0; i < g_bpObjNum && g_wigCompN < 4; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm)) || !strstr(nm, "SkeletalMeshComponent")) continue;
        void* mesh = ProbeReadPtr((char*)o + 0x238);
        if (!mesh) continue;
        uint32_t mIdx = 0;
        if (!BP_Read((char*)mesh + g_bpUNameOff, &mIdx, 4)) continue;
        if (g_wigMilesIdx) { if (mIdx != g_wigMilesIdx) continue; }
        else {
            char mn[64];
            if (!BP_Name(mIdx, mn, sizeof(mn)) || strcmp(mn, "Miles_beheaded")) continue;
            g_wigMilesIdx = mIdx;
        }
        // Must carry the 73-element atom arrays, or it is a different LOD/purpose component.
        struct { void* d; int n; int mx; } a0, a1;
        if (!BP_Read((char*)o + kWigArr[0], &a0, 16) || a0.n != 73) continue;
        if (!BP_Read((char*)o + kWigArr[1], &a1, 16) || a1.n != 73) continue;
        g_wigComp[g_wigCompN++] = o;
    }
    char l[128];
    sprintf_s(l, "[OLVR][WIGGLE] %d Miles component(s) armed (mode %ld)", g_wigCompN, g_boneWiggle);
    OLProxyLog(l);
    memset(g_wigLast, 0, sizeof(g_wigLast));
    memset(g_wigDelta, 0, sizeof(g_wigDelta));
}

static int WigWrite(void* addr, float v) {
    __try { *(float*)addr = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void WigTick() {
    if (--g_wigNext <= 0) { WigFind(); g_wigNext = 600; }
    if (!g_wigCompN) return;
    const float t = (float)(GetTickCount() % 100000u) * 0.001f;
    const float sway = 15.0f * sinf(t * 3.14159265f);   // 0.5 Hz, +/-15 uu
    for (int c = 0; c < g_wigCompN; ++c) {
        for (int a = 0; a < 2; ++a) {
            if (!(g_boneWiggle & (a + 1))) continue;
            struct { void* d; int n; int mx; } ta;
            if (!BP_Read((char*)g_wigComp[c] + kWigArr[a], &ta, 16) || ta.n != 73 || !ta.d) continue;
            for (int b = 0; b < 3; ++b) {
                // FBoneAtom: quat[0..3], translation[4..6], scale[7]. Wiggle translation Z.
                float* atom = (float*)((char*)ta.d + (size_t)kWigBone[b] * 0x20);
                float cur = 0.0f;
                if (!BP_Read(atom + 6, &cur, 4)) continue;
                // Readback verdict: still exactly the last written value = nothing recomposed
                // since that write.
                if (cur == g_wigLast[c][a][b] && g_wigDelta[c][a][b] != 0.0f) {
                    ++g_wigSticky;
                    cur -= g_wigDelta[c][a][b];   // strip the previous sway before adding the new one
                } else if (g_wigDelta[c][a][b] != 0.0f) {
                    ++g_wigAnim;
                }
                const float nv = cur + sway;
                if (!WigWrite(atom + 6, nv)) continue;
                g_wigLast[c][a][b] = nv;
                g_wigDelta[c][a][b] = sway;
            }
        }
    }
    static long s_rep = 0;
    if (++s_rep >= 180) {
        s_rep = 0;
        char l[160];
        const long st = InterlockedExchange(&g_wigSticky, 0), an = InterlockedExchange(&g_wigAnim, 0);
        sprintf_s(l, "[OLVR][WIGGLE] 3s: sticky=%ld animRewrote=%ld  (sticky-dominant = this write owns the "
                     "value; anim-dominant = write must land later in the frame)", st, an);
        OLProxyLog(l);
    }
}

// ---- [BONEWATCH] who touches Miles's bone arrays, and from where -------------------------------
// WIGGLE verdict: nothing moved, and animRewrote ran at exactly 2 comps x 2 arrays x 3 bones x
// 60 fps. So the arrays are real and the anim recomposes them every frame - but a write from
// the camera hook is invisible: it lands AFTER the composed pose has already been handed
// to the renderer for that frame, and the next compose wipes it. Writing to the right memory at
// the wrong moment. The fix needs two addresses this probe exists to find:
//   - the COMPOSER: the code that writes these arrays each frame (post-hook it so the override
//     lands after the anim and before extraction), and
//   - the EXTRACTOR(S): the code that reads them for rendering (if extraction is inside the same
//     function as compose, pre-hook the extractor instead).
// Method: hardware data breakpoints (DR0-3, read|write, 4 bytes) on the right hand's translation
// in both arrays of both Miles components, installed on EVERY thread of the process by a helper
// that suspends each thread and writes its context. A vectored handler records (thread, RIP,
// value-changed?) for each hit and, for the first hit of each distinct RIP, the FUNCTION START
// and a 3-frame caller chain via RtlLookupFunctionEntry + RtlVirtualUnwind - real unwinding, never
// a stack scan. Nothing is logged inside the handler; the game thread drains the table on its
// tick. Auto-disarms after ~8 s. Function starts are what the next build hooks with MinHook.
// [Stage3] BoneWatch=1. Dev only.
static long   g_boneWatch = 0;
static long   g_bwGameTid = 0;   // only this thread gets debug registers
static volatile long g_bwArmed = 0, g_bwDisarm = 0;
static void*  g_bwAddr[4] = {};
struct BwHit { DWORD tid; uintptr_t rip; uintptr_t fnStart; uintptr_t chain[3]; long hits; long changed; int which; };
static BwHit  g_bwHits[48];
static volatile long g_bwHitN = 0;
static volatile long g_bwTotal = 0;
static float  g_bwLastVal[4] = {};
static PVOID  g_bwVeh = nullptr;

static LONG CALLBACK BwHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 dr6 = c->Dr6;
    int which = -1;
    for (int i = 0; i < 4; ++i) if (dr6 & (1ull << i)) { which = i; break; }
    if (which < 0) return EXCEPTION_CONTINUE_SEARCH;   // a single-step from something other than this hook
    c->Dr6 = 0;
    InterlockedIncrement(&g_bwTotal);
    // Value-changed test: the trapping instruction has already executed, so a write shows here.
    float now = 0.0f; __try { now = *(volatile float*)g_bwAddr[which]; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    const bool changed = (now != g_bwLastVal[which]);
    g_bwLastVal[which] = now;
    const uintptr_t rip = (uintptr_t)c->Rip;
    const DWORD tid = GetCurrentThreadId();
    // Find or add the (tid, rip) row.
    long n = g_bwHitN;
    for (long i = 0; i < n; ++i) {
        if (g_bwHits[i].rip == rip && g_bwHits[i].tid == tid) {
            InterlockedIncrement(&g_bwHits[i].hits);
            if (changed) InterlockedIncrement(&g_bwHits[i].changed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (n < 48) {
        long slot = InterlockedIncrement(&g_bwHitN) - 1;
        if (slot < 48) {
            BwHit& h = g_bwHits[slot];
            h.tid = tid; h.rip = rip; h.hits = 1; h.changed = changed ? 1 : 0; h.which = which;
            h.fnStart = 0; h.chain[0] = h.chain[1] = h.chain[2] = 0;
            // Real unwind for the function start and three callers.
            __try {
                CONTEXT uc = *c;
                for (int f = 0; f < 4; ++f) {
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(uc.Rip, &base, nullptr);
                    if (f == 0) h.fnStart = rf ? (uintptr_t)(base + rf->BeginAddress) : 0;
                    if (!rf) break;
                    PVOID hd = nullptr; DWORD64 est = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, uc.Rip, rf, &uc, &hd, &est, nullptr);
                    if (!uc.Rip) break;
                    if (f < 3) h.chain[f] = (uintptr_t)uc.Rip;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ARMING RULE, learned by crashing the game: NEVER blanket-arm. Run 1 armed read|write
// breakpoints on 300 threads - every read of a bone array on every thread trapped into the VEH,
// and the process died. Now: the GAME THREAD ONLY (skeletal composition is game-thread in UE3),
// WRITE-ONLY traps (the writer is the target, and reads are the overwhelming majority of accesses),
// and if that yields nothing, scope widens deliberately rather than by default.
static void BwSetOnAllThreads(bool arm) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int done = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            if (te.th32ThreadID != (DWORD)g_bwGameTid) continue;   // game thread only - see the rule above
            HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c)) {
                    if (arm) {
                        c.Dr0 = (DWORD64)g_bwAddr[0]; c.Dr1 = (DWORD64)g_bwAddr[1];
                        c.Dr2 = (DWORD64)g_bwAddr[2]; c.Dr3 = (DWORD64)g_bwAddr[3];
                        // L0..L3 enabled; RW=11 (read|write) LEN=11 (4 bytes) for each.
                        DWORD64 dr7 = 0;
                        for (int i = 0; i < 4; ++i) if (g_bwAddr[i]) { dr7 |= (1ull << (i * 2)); dr7 |= (0xDull << (16 + i * 4)); }   // 0xD = LEN 4 bytes, RW WRITE-ONLY
                        c.Dr7 = dr7;
                    } else {
                        c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0; c.Dr7 = 0;
                    }
                    c.Dr6 = 0;
                    if (SetThreadContext(h, &c)) ++done;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    char l[128];
    sprintf_s(l, "[OLVR][BONEWATCH] debug registers %s on %d thread(s)", arm ? "ARMED" : "cleared", done);
    OLProxyLog(l);
}

static DWORD WINAPI BwArmThread(LPVOID) {
    Sleep(50);
    BwSetOnAllThreads(true);
    InterlockedExchange(&g_bwArmed, 1);
    Sleep(8000);
    BwSetOnAllThreads(false);
    InterlockedExchange(&g_bwDisarm, 1);
    return 0;
}

static void BwReport() {
    const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    char l[320];
    auto rel = [&](uintptr_t a, char* out, size_t n) {
        if (!a) { strcpy_s(out, n, "-"); return; }
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m);
        if ((uintptr_t)m == exe) sprintf_s(out, n, "exe+0x%llX", (unsigned long long)(a - exe));
        else if (m) { wchar_t p[MAX_PATH]; GetModuleFileNameW(m, p, MAX_PATH); const wchar_t* b = wcsrchr(p, L'\\'); sprintf_s(out, n, "%ls+0x%llX", b ? b + 1 : p, (unsigned long long)(a - (uintptr_t)m)); }
        else sprintf_s(out, n, "%p", (void*)a);
    };
    const long n = g_bwHitN < 48 ? g_bwHitN : 48;
    sprintf_s(l, "[OLVR][BONEWATCH] %ld total hits, %ld distinct (thread,rip) sites:", g_bwTotal, n);
    OLProxyLog(l);
    for (long i = 0; i < n; ++i) {
        const BwHit& h = g_bwHits[i];
        char r[48], f[48], c0[48], c1[48], c2[48];
        rel(h.rip, r, sizeof(r)); rel(h.fnStart, f, sizeof(f));
        rel(h.chain[0], c0, sizeof(c0)); rel(h.chain[1], c1, sizeof(c1)); rel(h.chain[2], c2, sizeof(c2));
        sprintf_s(l, "[OLVR][BONEWATCH]  tid=%lu dr%d rip=%s fn=%s hits=%ld changed=%ld  <- %s <- %s <- %s",
                  h.tid, h.which, r, f, h.hits, h.changed, c0, c1, c2);
        OLProxyLog(l);
    }
}

static void BwTick() {
    static bool s_started = false, s_reported = false;
    if (!s_started) {
        if (g_wigCompN < 1) { if (--g_wigNext <= 0) { WigFind(); g_wigNext = 600; } return; }
        // DR0/1 = comp0's two arrays, DR2/3 = comp1's (or comp0 again if only one), bone 36 z.
        for (int i = 0; i < 4; ++i) {
            void* comp = g_wigComp[(i / 2) < g_wigCompN ? (i / 2) : 0];
            struct { void* d; int n; int mx; } ta;
            if (!BP_Read((char*)comp + kWigArr[i & 1], &ta, 16) || ta.n != 73 || !ta.d) { g_bwAddr[i] = nullptr; continue; }
            g_bwAddr[i] = (char*)ta.d + (size_t)36 * 0x20 + 6 * 4;   // R-Hand translation.z
        }
        char l[200];
        g_bwGameTid = (long)GetCurrentThreadId();
        sprintf_s(l, "[OLVR][BONEWATCH] gameThread=%lu targets %p %p %p %p (write-only, this thread only)", GetCurrentThreadId(),
                  g_bwAddr[0], g_bwAddr[1], g_bwAddr[2], g_bwAddr[3]);
        OLProxyLog(l);
        g_bwVeh = AddVectoredExceptionHandler(1, BwHandler);
        CreateThread(nullptr, 0, BwArmThread, nullptr, 0, nullptr);
        s_started = true;
        return;
    }
    if (g_bwDisarm && !s_reported) {
        s_reported = true;
        BwReport();
        if (g_bwVeh) { RemoveVectoredExceptionHandler(g_bwVeh); g_bwVeh = nullptr; }
    }
}

// ---- [SKELPROP] read the SkelControl property offsets out of UE3's own reflection ---------------
// SKELCTRL found the prize: Miles's AnimTree already holds LIVE SkelControls on his arms
// (SkelControlLimb on Hero-R-Clavicle = a 2-bone IK effector; SkelControlSingleBone on both upper
// arms and forearms). The animation system ticks them itself, in the correct window - so the
// write-timing problem that beat WIGGLE and BONEWATCH is simply gone.
// To drive one, three field offsets are needed: ControlStrength (float 0..1, the on switch),
// EffectorLocation (vector, for the Limb IK) and BoneTranslation/BoneRotation (for SingleBone).
// GUESSING OFFSETS IS THE MISTAKE THAT COST TODAY, so this reads them from the engine's own
// UProperty chain: every UClass holds a Children list of UProperty objects, each carrying an
// already-resolvable name plus an Offset field. Both link offsets are DISCOVERED by scoring, never
// assumed - the correct Children/Next pair yields a long chain of real property names, and the
// correct Offset field yields values that increase along that chain.
// Log-only, no writes, no threads. [Stage3] SkelPropProbe=1.
static long g_skelProp = 0;

static void* SP_FindClass(const char* want) {
    char nm[64];
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (BP_Name(idx, nm, sizeof(nm)) && !strcmp(nm, want)) return o;
    }
    return nullptr;
}

// Walk a candidate (children,next) pair; returns how many links resolved to names.
static int SP_Walk(void* cls, unsigned childOff, unsigned nextOff,
                   char out[48][64], unsigned outOff[48], unsigned propOffField) {
    void* p = ProbeReadPtr((char*)cls + childOff);
    int n = 0;
    while (p && n < 48) {
        uint32_t idx = 0;
        if (!BP_Read((char*)p + g_bpUNameOff, &idx, 4)) break;
        if (!BP_Name(idx, out[n], 64)) break;
        outOff[n] = 0;
        if (propOffField) { uint32_t v = 0; if (BP_Read((char*)p + propOffField, &v, 4)) outOff[n] = v; }
        ++n;
        void* nx = ProbeReadPtr((char*)p + nextOff);
        if (nx == p) break;
        p = nx;
    }
    return n;
}

static void SkelPropRun() {
    char l[256];
    if (!g_bpObjs || !g_bpNames) { OLProxyLog("[OLVR][SKELPROP] registry not ready"); return; }
    const char* classes[3] = { "SkelControlBase", "SkelControlSingleBone", "SkelControlLimb" };
    for (int ci = 0; ci < 3; ++ci) {
        void* cls = SP_FindClass(classes[ci]);
        if (!cls) { sprintf_s(l, "[OLVR][SKELPROP] class '%.32s' not found", classes[ci]); OLProxyLog(l); continue; }

        unsigned bestC = 0, bestN = 0; int bestLen = 0;
        char names[48][64]; unsigned offs[48];
        for (unsigned co = 0x28; co <= 0x100; co += 8) {
            if (!ProbeReadPtr((char*)cls + co)) continue;
            for (unsigned no = 0x28; no <= 0x80; no += 8) {
                const int len = SP_Walk(cls, co, no, names, offs, 0);
                if (len > bestLen) { bestLen = len; bestC = co; bestN = no; }
            }
        }
        if (bestLen < 3) { sprintf_s(l, "[OLVR][SKELPROP] '%.32s': no property chain found", classes[ci]); OLProxyLog(l); continue; }

        // UProperty::Offset = the uint32 that increases along the chain and stays object-sized.
        unsigned bestOffField = 0; int bestScore = 0;
        for (unsigned of = 0x38; of <= 0xA0; of += 4) {
            const int len = SP_Walk(cls, bestC, bestN, names, offs, of);
            int score = 0, prev = -1, sane = 1;
            for (int i = 0; i < len; ++i) {
                if (offs[i] > 0x800) { sane = 0; break; }
                if ((int)offs[i] > prev) ++score;
                prev = (int)offs[i];
            }
            if (sane && score > bestScore) { bestScore = score; bestOffField = of; }
        }
        const int len = SP_Walk(cls, bestC, bestN, names, offs, bestOffField);
        sprintf_s(l, "[OLVR][SKELPROP] === %.32s: %d props (children+0x%X next+0x%X offField+0x%X) ===",
                  classes[ci], len, bestC, bestN, bestOffField);
        OLProxyLog(l);
        char line[240]; int ll = 0; line[0] = 0;
        for (int i = 0; i < len; ++i) {
            if (ll > 150) { sprintf_s(l, "[OLVR][SKELPROP]   %.200s", line); OLProxyLog(l); ll = 0; line[0] = 0; }
            ll += sprintf_s(line + ll, sizeof(line) - ll, "%.32s@0x%X  ", names[i], offs[i]);
        }
        if (ll) { sprintf_s(l, "[OLVR][SKELPROP]   %.200s", line); OLProxyLog(l); }
    }

    // Cross-check on a LIVE Hero-bound control: whatever the reflection calls ControlStrength must
    // read as a plausible float in a real instance. This line is what proves the offsets usable.
    char nm[64];
    for (int i = 0, shown = 0; i < g_bpObjNum && shown < 3; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm))) continue;
        if (strcmp(nm, "SkelControlSingleBone") && strcmp(nm, "SkelControlLimb")) continue;
        int hero = 0; char bn[64]; bn[0] = 0;
        for (unsigned off = 0x50; off <= 0x300 && !hero; off += 4) {
            uint32_t bi = 0;
            if (BP_Read((char*)o + off, &bi, 4) && BP_Name(bi, bn, sizeof(bn)) && !strncmp(bn, "Hero-", 5)) hero = 1;
        }
        if (!hero) continue;
        ++shown;
        char line[240]; int ll = 0; line[0] = 0;
        for (unsigned off = 0x60; off <= 0x150 && ll < 150; off += 4) {
            float f = 0.0f;
            if (!BP_Read((char*)o + off, &f, 4)) continue;
            if (f != f || fabsf(f) > 1e6f) continue;
            ll += sprintf_s(line + ll, sizeof(line) - ll, "%X:%.2f ", off, f);
        }
        sprintf_s(l, "[OLVR][SKELPROP] live %.28s (%.24s)  %.170s", nm, bn, line);
        OLProxyLog(l);
    }
    OLProxyLog("[OLVR][SKELPROP] done");
}


// ---- [ARMDRIVE] the controller moves Miles's arm -----------------------------------------------
// Everything needed is now measured, none of it guessed:
//   SkelControlLimb   EffectorLocation @0xF4  EffectorLocationSpace @0x10C   (2-bone IK effector)
//   SkelControlBase   ControlStrength  @ found by reflection below (the on switch)
// A SkelControl is ticked BY the animation system, in its own correct window, so there is no hook
// and no write-timing problem - the trap that beat WIGGLE and BONEWATCH. The effector is set to
// where the player's controller is in the world and the strength is raised; the engine solves the
// elbow and moves the arm.
// The base-class property offsets could not be read in the SKELPROP run because SkelControlBase's
// Children chain mixes UFunctions in with UProperties and functions have no Offset field, which
// wrecked the scoring. The subclasses proved the Offset field is at +0x8C, so the base chain is
// re-read with that forced and only sane, increasing values accepted.
// Values must be REWRITTEN EVERY FRAME: the animation system reasserts control state each tick
// (the RE Engine lesson - anything written to a joint is rewritten every frame or it is eaten).
// [Stage3] ArmDrive=1. Hold the right grip to aim; strength ramps so the arm never snaps.
static long  g_armDrive = 0;
static unsigned g_scStrengthOff = 0;      // SkelControlBase::ControlStrength, from reflection
static const unsigned kLimbEffector    = 0xF4;
static const unsigned kLimbEffSpace    = 0x10C;
static const unsigned kLimbJointSpace  = 0x10D;   // JointTargetLocationSpace, from reflection
static const unsigned kLimbJointTarget = 0x120;   // JointTargetLocation,      from reflection
static const unsigned kLimbEffRot      = 0x100;   // EffectorRotation (FRotator: pitch,yaw,roll)
static const unsigned kLimbBools       = 0x11C;   // bUseEffectorRotation & friends share this dword
static unsigned g_bitUseEffRot = 0;               // its bit, read from reflection (never guessed)
// Body estimates in Unreal units at the measured 100 uu/m. The camera is 168 uu above the pawn
// root (CamProbe), so these hang the shoulder a head's height below the eyes.
static const float kShoulderDown  = 25.0f;
static const float kShoulderRight = 18.0f;
static const float kShoulderBack  = 8.0f;
static const float kArmReachUU    = 62.0f;   // upper arm + forearm, ~62 cm
static const float kElbowBack     = 40.0f;
static const float kElbowDown     = 55.0f;
static void* g_armLimb[8]; static int g_armLimbN = 0;
static float g_armRamp = 0.0f;

// A UE3 UBoolProperty packs many bools into one dword and stores WHICH BIT in a BitMask field on
// the property object. The offset of that field is not known, so it is found the same way as
// everything else here: inside the property object, look for the lone power-of-two dword. Guessing
// the bit would mean flipping bAllowStretching or bInvertBoneAxis by accident.
static unsigned SP_FindBoolBit(const char* cls, const char* want) {
    void* c = SP_FindClass(cls);
    if (!c) return 0;
    char names[48][64]; unsigned offs[48];
    const int len = SP_Walk(c, 0x80, 0x60, names, offs, 0x8C);
    // Re-walk to reach the property OBJECT for the wanted name.
    void* pobj = ProbeReadPtr((char*)c + 0x80);
    for (int i = 0; i < len && pobj; ++i) {
        if (!strcmp(names[i], want)) {
            for (unsigned bo = 0x88; bo <= 0xC8; bo += 4) {
                uint32_t v = 0;
                if (!BP_Read((char*)pobj + bo, &v, 4)) continue;
                if (v && !(v & (v - 1)) && v <= 0x80000000u) return v;   // exactly one bit set
            }
            return 0;
        }
        pobj = ProbeReadPtr((char*)pobj + 0x60);
    }
    return 0;
}

static unsigned SP_FindBaseProp(const char* want) {
    void* cls = SP_FindClass("SkelControlBase");
    if (!cls) return 0;
    char names[48][64]; unsigned offs[48];
    const int len = SP_Walk(cls, 0x80, 0x60, names, offs, 0x8C);   // links proven by the subclasses
    for (int i = 0; i < len; ++i)
        if (!strcmp(names[i], want) && offs[i] >= 0x40 && offs[i] < 0xF4) return offs[i];
    return 0;
}

// Every SkelControlLimb carrying a Hero-* bone name: Miles's arm IK. Several exist (LODs and
// mesh variants all share the AnimTree shape); driving them all is simpler than guessing which
// one is on screen, and writing to an unattached template is inert.
static void ArmFind() {
    g_armLimbN = 0;
    char nm[64], bn[64];
    for (int i = 0; i < g_bpObjNum && g_armLimbN < 8; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm)) || strcmp(nm, "SkelControlLimb")) continue;
        // ARM limbs ONLY. The first cut took any Limb carrying a Hero-* name, which also
        // matched the SkelControlLimb bound to Hero-Head - so his head IK was being dragged to
        // the hand target as well. Require a bone on the arm chain and reject the head outright.
        for (unsigned off = 0x100; off <= 0x160; off += 4) {
            uint32_t bi = 0;
            if (!BP_Read((char*)o + off, &bi, 4) || !BP_Name(bi, bn, sizeof(bn))) continue;
            if (strncmp(bn, "Hero-", 5)) continue;
            if (strstr(bn, "Head") || strstr(bn, "Neck") || strstr(bn, "Spine")) break;
            if (strstr(bn, "Clavicle") || strstr(bn, "UpperArm") ||
                strstr(bn, "Forearm")  || strstr(bn, "Hand")) { g_armLimb[g_armLimbN++] = o; break; }
        }
    }
    char l[160];
    sprintf_s(l, "[OLVR][ARMDRIVE] %d Hero arm IK control(s), ControlStrength@0x%X, bUseEffectorRotation bit=0x%X",
              g_armLimbN, g_scStrengthOff, g_bitUseEffRot);
    OLProxyLog(l);
}

static int ArmWriteF(void* addr, float v) {
    __try { *(float*)addr = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ArmWriteB(void* addr, unsigned char v) {
    __try { *(unsigned char*)addr = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// camLoc/baseYaw come from the camera hook, so the target is built in the same body frame the
// lean already uses and at the same measured 100 uu/m.
static void ArmTick(const float* camLoc, float baseYaw) {
    if (!g_armLimbN || !g_scStrengthOff) return;
    OLXrPad pad;
    const bool have = OLXR_MotionControlsReady() && OLXR_GetPad(&pad) && (pad.handValid & 2u);
    // Ramp so raising and lowering the arm is a blend, never a snap.
    const float target = have ? 1.0f : 0.0f;
    g_armRamp += (target - g_armRamp) * 0.2f;
    if (g_armRamp < 0.01f) { g_armRamp = 0.0f; }
    if (!have && g_armRamp == 0.0f) {
        for (int i = 0; i < g_armLimbN; ++i) ArmWriteF((char*)g_armLimb[i] + g_scStrengthOff, 0.0f);
        return;
    }
    // Right hand, recenter frame (metres) -> world Unreal units in the body frame.
    const float hx = pad.handPose[1].px, hy = pad.handPose[1].py, hz = pad.handPose[1].pz;
    const float fx = cosf(baseYaw), fy = sinf(baseYaw);
    const float rx = -sinf(baseYaw), ry = cosf(baseYaw);
    const float dR = hx * kMetersToUU, dU = hy * kMetersToUU, dF = -hz * kMetersToUU;
    float wx = camLoc[0] + rx * dR + fx * dF;
    float wy = camLoc[1] + ry * dR + fy * dF;
    float wz = camLoc[2] + dU;

    // REACH CLAMP. Miles's shoulder sits below and right of the eyes; the solver has no idea the
    // arm is only so long, so an out-of-range effector either stretches the limb or wrenches the
    // whole shoulder - a large part of what read as broken IK. Estimate the shoulder in the body
    // frame, then keep the target inside arm's length of it.
    const float shx = camLoc[0] + rx * kShoulderRight - fx * kShoulderBack;
    const float shy = camLoc[1] + ry * kShoulderRight - fy * kShoulderBack;
    const float shz = camLoc[2] - kShoulderDown;
    {
        const float ax = wx - shx, ay = wy - shy, az = wz - shz;
        const float d = sqrtf(ax * ax + ay * ay + az * az);
        if (d > kArmReachUU && d > 0.001f) {
            const float k = kArmReachUU / d;
            wx = shx + ax * k; wy = shy + ay * k; wz = shz + az * k;
        }
    }
    // JOINT TARGET = where the elbow is asked to point. Unset, the solver is free to spin the
    // elbow anywhere on the cone around the shoulder-to-hand axis, which is the flailing. Hang it
    // below and behind the shoulder, which is where a human elbow actually goes.
    const float jx = shx - fx * kElbowBack;
    const float jy = shy - fy * kElbowBack;
    const float jz = shz - kElbowDown;

    // HAND ORIENTATION. Position alone is why it did not feel like a real hand - a real hand rolls
    // and pitches with the wrist. The controller quaternion (recenter frame) is decomposed with
    // the SAME extraction the head uses, then carried into world yaw by the body yaw, and written
    // as a UE3 FRotator (int16-style units, 65536 = 360 deg, order pitch/yaw/roll).
    const float qx = pad.handPose[1].qx, qy = pad.handPose[1].qy;
    const float qz = pad.handPose[1].qz, qw = pad.handPose[1].qw;
    const float hfx = -2.0f * (qx * qz + qw * qy);
    const float hfy = -2.0f * (qy * qz - qw * qx);
    const float hfz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    const float hpc = (hfy < -1.0f) ? -1.0f : (hfy > 1.0f ? 1.0f : hfy);
    const float handYaw   = atan2f(hfx, -hfz);
    const float handPitch = asinf(hpc);
    const float handRoll  = atan2f(2.0f * (qx * qy + qw * qz), 1.0f - 2.0f * (qx * qx + qz * qz));
    const float kToUU = 65536.0f / 6.28318531f;
    const int rotPitch = (int)(handPitch * kToUU);
    const int rotYaw   = (int)((baseYaw + handYaw) * kToUU);
    const int rotRoll  = (int)(handRoll * kToUU);

    for (int i = 0; i < g_armLimbN; ++i) {
        char* c = (char*)g_armLimb[i];
        // Rotation must be ARMED as well as written: bUseEffectorRotation lives in a packed bool
        // dword, so set exactly the bit reflection named and leave every neighbour alone.
        if (g_bitUseEffRot) {
            __try {
                *(unsigned*)(c + kLimbBools) |= g_bitUseEffRot;
                *(int*)(c + kLimbEffRot + 0) = rotPitch;
                *(int*)(c + kLimbEffRot + 4) = rotYaw;
                *(int*)(c + kLimbEffRot + 8) = rotRoll;
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ArmWriteB(c + kLimbEffSpace, 0);          // EffectorLocationSpace  = BCS_WorldSpace
        ArmWriteF(c + kLimbEffector + 0, wx);
        ArmWriteF(c + kLimbEffector + 4, wy);
        ArmWriteF(c + kLimbEffector + 8, wz);
        ArmWriteB(c + kLimbJointSpace, 0);        // JointTargetLocationSpace = BCS_WorldSpace
        ArmWriteF(c + kLimbJointTarget + 0, jx);
        ArmWriteF(c + kLimbJointTarget + 4, jy);
        ArmWriteF(c + kLimbJointTarget + 8, jz);
        ArmWriteF(c + g_scStrengthOff, g_armRamp);
    }
    static long s_rep = 0;
    if ((++s_rep % 180) == 1) {
        char l[200];
        const float rr = sqrtf((wx-shx)*(wx-shx) + (wy-shy)*(wy-shy) + (wz-shz)*(wz-shz));
        sprintf_s(l, "[OLVR][ARMDRIVE] n=%d str=%.2f eff=(%.0f %.0f %.0f) reach=%.0f/%.0f hand=(%.2f %.2f %.2f)m rot=(%.0f %.0f %.0f)deg",
                  g_armLimbN, g_armRamp, wx, wy, wz, rr, kArmReachUU, hx, hy, hz,
                  handPitch * 57.2957795f, (baseYaw + handYaw) * 57.2957795f, handRoll * 57.2957795f);
        OLProxyLog(l);
    }
}

// ---- [SKELCTRL] the UE3-native way to move a bone, which should have been the first approach ---
// Two failures point at one mistake. WIGGLE wrote SpaceBases/LocalAtoms directly and the anim
// recomposed over it (right memory, wrong moment). BONEWATCH then tried to find that moment with
// hardware breakpoints and crashed the game - 300 threads armed at once, without counting first.
// Both were fighting the animation system. UE3 has a supported way to bend a bone that the
// animation system APPLIES ITSELF, every frame, in the correct window, by design: SkelControl.
// The exe carries the whole family (SkelControlSingleBone, SkelControlLimb - a 2-bone IK with an
// effector, i.e. "put this hand here" - plus BoneRotation/ControlStrength property names).
// If Miles's AnimTree already contains SkelControls on the arms, driving them needs NO hooking
// and NO timing games: set ControlStrength and the target, and the engine does the rest.
// This probe just looks. For every object whose name contains SkelControl or AnimTree it logs
// the name, and - the useful part - scans the object for FName fields resolving to a 'Hero-*'
// string, which is what identifies a control bound to MILES's skeleton rather than an NPC's.
// It also dumps raw floats from a bone-atom array so the ATOM STRIDE can be confirmed rather
// than assumed: WIGGLE used 0x20, and if the real stride is 0x30 then every write went into a
// finger instead of the arm, which would look exactly like "nothing moved".
// [Stage3] SkelCtrlProbe=1. Log-only. No threads, no breakpoints, no writes.
static long g_skelProbe = 0;

static void SkelProbeRun() {
    char l[300], nm[64];
    if (!g_bpObjs || !g_bpNames) { OLProxyLog("[OLVR][SKELCTRL] registry not ready"); return; }

    // --- 1. atom stride, measured. A real atom array is quat-then-translation, so at the correct
    // stride every element's first 4 floats are a UNIT quaternion. Score each candidate stride by
    // how many of 40 elements have |q| ~ 1.
    for (int c = 0; c < g_wigCompN && c < 1; ++c) {
        for (int a = 0; a < 2; ++a) {
            struct { void* d; int n; int mx; } ta;
            if (!BP_Read((char*)g_wigComp[c] + kWigArr[a], &ta, 16) || ta.n != 73 || !ta.d) continue;
            int bestS = 0, bestHit = 0;
            const unsigned strides[] = { 0x20, 0x30, 0x40 };
            for (unsigned si = 0; si < 3; ++si) {
                int hit = 0;
                for (int i = 0; i < 40; ++i) {
                    float q[4];
                    if (!BP_Read((char*)ta.d + (size_t)i * strides[si], q, 16)) break;
                    const float m = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
                    if (m > 0.97f && m < 1.03f) ++hit;
                }
                if (hit > bestHit) { bestHit = hit; bestS = (int)strides[si]; }
            }
            sprintf_s(l, "[OLVR][SKELCTRL] atom array +0x%X: best stride 0x%X (%d/40 unit quats)"
                         "%s", kWigArr[a], bestS, bestHit,
                      bestS == 0x20 ? "  <- WIGGLE assumed 0x20, confirmed"
                                    : "  <- WIGGLE ASSUMED 0x20 AND WAS WRONG");
            OLProxyLog(l);
            // Show the bone actually hit at the assumed stride vs the correct one.
            if (bestS) {
                float t20[3] = {}, tb[3] = {};
                BP_Read((char*)ta.d + (size_t)36 * 0x20 + 16, t20, 12);
                BP_Read((char*)ta.d + (size_t)36 * (size_t)bestS + 16, tb, 12);
                sprintf_s(l, "[OLVR][SKELCTRL]   bone36 translation @0x20-stride (%.1f %.1f %.1f) "
                             "vs @0x%X-stride (%.1f %.1f %.1f)",
                          t20[0], t20[1], t20[2], bestS, tb[0], tb[1], tb[2]);
                OLProxyLog(l);
            }
        }
    }

    // --- 2. every SkelControl / AnimTree object, and which skeleton it is bound to.
    int nSkel = 0, nHero = 0;
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm))) continue;
        const bool isSkel = strstr(nm, "SkelControl") != nullptr;
        const bool isTree = strstr(nm, "AnimTree") != nullptr;
        if (!isSkel && !isTree) continue;
        ++nSkel;
        // Bound bone: any FName field on this object resolving to a Hero-* / NPC* bone string.
        char bound[64]; bound[0] = 0; unsigned boundOff = 0;
        for (unsigned off = 0x50; off <= 0x300 && !bound[0]; off += 4) {
            uint32_t bi = 0; char bn[64];
            if (!BP_Read((char*)o + off, &bi, 4)) continue;
            if (!BP_Name(bi, bn, sizeof(bn))) continue;
            if (strncmp(bn, "Hero-", 5) == 0 || strncmp(bn, "NPC", 3) == 0) {
                strcpy_s(bound, bn); boundOff = off;
            }
        }
        if (bound[0] && strncmp(bound, "Hero-", 5) == 0) ++nHero;
        if (nSkel <= 40 || bound[0]) {
            sprintf_s(l, "[OLVR][SKELCTRL] %p '%.40s'%s%.40s%s", o, nm,
                      bound[0] ? "  bone=" : "", bound[0] ? bound : "",
                      bound[0] ? "" : "  (no bone name found)");
            if (bound[0]) {
                char l2[300];
                sprintf_s(l2, "%s  @+0x%X", l, boundOff);
                OLProxyLog(l2);
            } else OLProxyLog(l);
        }
    }
    sprintf_s(l, "[OLVR][SKELCTRL] done: %d SkelControl/AnimTree objects, %d bound to Hero-* bones",
              nSkel, nHero);
    OLProxyLog(l);
}

// MENU CAMERA FREEZE. While the menu is open the world must hold still, or you cannot read it.
// Outlast is UE3 and reads the mouse through DirectInput, which a window-message swallow cannot
// see (ME1 learned this and froze the camera in the ENGINE instead). This hook already owns the
// camera source, so the same approach applies: latch the game's view the instant the menu opens and replay it every
// frame. Head tracking and the eye offset still apply ON TOP, so you can glance around the panel
// and it stays in stereo - only the mouse/stick look is frozen.
static long  g_menuLatched = 0;
static float g_menuLoc[3] = { 0, 0, 0 };
static int   g_menuRot[3] = { 0, 0, 0 };

static void __fastcall Hook_GetViewPoint(void* actor, float* outLoc, int* outRot) {
    o_GetViewPoint(actor, outLoc, outRot);
    // [NVLIGHT] The engine's own answer, before ANY injection - this is the gamepad/body
    // aim, and it is what tells the spotlight hook which light is the player's. Captured on every
    // call (including the tick calls otherwise ignored), so it is never stale.
    if (outRot) {
        g_baseViewRot[0] = outRot[0]; g_baseViewRot[1] = outRot[1]; g_baseViewRot[2] = outRot[2];
        InterlockedExchange(&g_baseViewFresh, 30);
    }
    if (!g_eyePhase || !outLoc || !outRot) return;
    // [CAMPROBE] one row per frame (first eye only), BASE values before any injection below.
    if (g_camProbe && g_eyePhase == 1) ProbeTick(actor, outLoc, outRot);
    // [BONEPROBE] one shot, ~12s into gameplay so every level actor exists.
    if (g_boneProbe && g_eyePhase == 1) {
        static long s_bpFrames = 0;
        if (++s_bpFrames == 700) BP_Run(actor);
    }
    
// [WIGGLE] / [BONEWATCH] / [SKELCTRL] every frame once the level is up; all need the registry
    // first. EVERY probe that lives in this block MUST be named in this gate - SKELCTRL was added
    // inside it but left out of the condition, so arming it alone made it silently unreachable and
    // burned a whole test run producing an empty log.
    if ((g_boneWiggle || g_boneWatch || g_skelProbe || g_skelProp || g_armDrive) && g_eyePhase == 1) {
        static long s_wg = 0; static bool s_boot = false;
        if (++s_wg > 300) {
            if (!s_boot) {
                s_boot = true;
                if (!g_bpNames) BP_FindGNames();
                if (g_bpNames && !g_bpObjs) BP_FindObjects();
            }
            if (g_bpObjs) {
                if (g_boneWiggle) WigTick();
                if (g_boneWatch)  BwTick();
                if (g_skelProbe) {
                    static bool s_sk = false;
                    if (!s_sk) { s_sk = true; if (!g_wigCompN) WigFind(); SkelProbeRun(); }
                }
                if (g_skelProp) {
                    static bool s_sp = false;
                    if (!s_sp) { s_sp = true; SkelPropRun(); }
                }
                if (g_armDrive) {
                    static bool s_ad = false;
                    if (!s_ad) { s_ad = true; g_scStrengthOff = SP_FindBaseProp("ControlStrength");
                                  g_bitUseEffRot = SP_FindBoolBit("SkelControlLimb", "bUseEffectorRotation");
                                  ArmFind(); }
                    ArmTick(outLoc, (float)(outRot[1] & 0xFFFF) * (6.28318531f / 65536.0f));
                }
            }
        }
    }
    __try {
        if (OLMenu_Visible()) {
            if (!g_menuLatched) {
                g_menuLatched = 1;
                for (int i = 0; i < 3; ++i) { g_menuLoc[i] = outLoc[i]; g_menuRot[i] = outRot[i]; }
            }
            for (int i = 0; i < 3; ++i) { outLoc[i] = g_menuLoc[i]; outRot[i] = g_menuRot[i]; }
        } else {
            g_menuLatched = 0;
        }
        // UE3 FRotator is int16-style units: 65536 = 360 deg. Left-handed, Z-up, X-forward,
        // Y-right. With roll ignored, the camera's right axis is (-sin(yaw), cos(yaw), 0).
        const float kToRad = 6.28318531f / 65536.0f;
        const float kToUnits = 65536.0f / 6.28318531f;

        // The head offset is applied in the BASE (gamepad) frame, so it must be read before the
        // head rotation goes in - that is what makes leaning sideways stay sideways relative to
        // where the stick is pointing, instead of rotating with your own head.
        const float baseYaw = (float)(outRot[1] & 0xFFFF) * kToRad;

        const float injW = g_injW;   // one read; the tick thread may move it between calls
        if (g_headTrack && g_olPoseValid && injW > 0.0f) {
            outRot[0] += (int)(g_olPitchRad * injW * kToUnits);   // up is positive in both systems
            // YAW SIGN, corrected 2026-07-22 after left/right was found inverted in testing. UE3 yaw
            // rotates the forward vector from +X toward +Y - and in UE3 **+Y IS RIGHT** (the
            // line above says so), so increasing yaw turns you RIGHT. In OpenXR a left turn is a
            // positive rotation about +Y, which makes g_olYawRad = atan2(fwd.x,-fwd.z) NEGATIVE.
            // Left turn -> negative reading -> UE3 needs a negative delta to turn left. Straight
            // addition. (The first version subtracted: right derivation of the XR side, wrong
            // reading of which world direction UE3's +Y points.)
            outRot[1] += (int)(g_olYawRad   * injW * kToUnits);
            if (g_headRoll) outRot[2] += (int)(g_olRollRad * injW * kToUnits);   // off by default

            // [LEAN100] Positional tracking at the MEASURED scale (100 uu/m - see the note at
            // kMetersToUU; every earlier round rendered half the declared movement and the
            // mismatch is what read as swimming). Raw position, applied in the body (gamepad)
            // frame read before the head rotation, gated on the armed baseline. The declared
            // pose is untouched: at gain 1.0 declared == rendered by construction.
            if (g_headPos && OLXR_PosArmed()) {
                float hx = g_olHeadX, hy = g_olHeadY, hz = g_olHeadZ;   // metres, recenter frame
                if (hx < -kPosSanityM) hx = -kPosSanityM; else if (hx > kPosSanityM) hx = kPosSanityM;
                if (hy < -kPosSanityM) hy = -kPosSanityM; else if (hy > kPosSanityM) hy = kPosSanityM;
                if (hz < -kPosSanityM) hz = -kPosSanityM; else if (hz > kPosSanityM) hz = kPosSanityM;
                const float gain = g_leanGain * injW * kMetersToUU;
                const float dR = hx * gain;                             // +right
                const float dU = hy * gain;                             // +up
                const float dF = -hz * gain;                            // XR -z = forward, 1:1
                const float fx = cosf(baseYaw), fy = sinf(baseYaw);     // forward (body, pre-head)
                const float rx = -sinf(baseYaw), ry = cosf(baseYaw);    // right
                outLoc[0] += rx * dR + fx * dF;
                outLoc[1] += ry * dR + fy * dF;
                outLoc[2] += dU;
            }
        }

        // Eye separation goes along the FINAL view direction, head rotation included.
        const float yaw = (float)(outRot[1] & 0xFFFF) * kToRad;
        const float erx = -sinf(yaw), ery = cosf(yaw);
        float side = (g_eyePhase == 2) ? 1.0f : -1.0f;
        float d = g_halfEyeUU * side * (g_eyeSwap ? -1.0f : 1.0f);
        outLoc[0] += erx * d;
        outLoc[1] += ery * d;
        // Z deliberately untouched: eyes separate horizontally in the world, never vertically.
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---- the Outlast-custom occlusion tracker ------------------------------------------------
// Outlast added an engine source file stock UE3 does not have: PrimitiveSceneProxyOcclusionTracker
// (16 path strings in OLGame_R.exe). Ghidra (2026-07-22, this session): the tracker's
// UpdateAndGetCoverage - R 0x670940, fingerprinted into shipping at 0x29AEF0 by its two
// 0x20000000000 flag tests + the 0x7FC/0x800 config pair, then confirmed by a line-for-line
// decompile match - reads the primitive's hardware occlusion query, projects the bounds to
// screen, and smooths coverage into a SINGLE float at tracker+0x50. One value per OBJECT,
// not per view. Its 511-byte caller (a DrawDynamicElements-shaped virtual) SKIPS THE DRAW
// when it returns 0.
//
// That is the same-frame flicker mechanism: two views per frame feed one smoothed coverage
// two conflicting query answers, it oscillates, and discrete props (chairs/windows/doors -
// the primitives with these proxies) wink in and out. Ground/BSP has no such tracker and
// never flickers. AER is one view per frame = one answer = clean. It also explains why the
// per-eye view-state fixes (section 4.2/section 4.3 in the handoff) changed nothing: the shared value
// lives on the OBJECT, not in any view state.
//
// The test: force coverage = 1.0 and "updated" so nothing is ever skipped or faded by the
// tracker. If the flicker dies with this ON and returns with it OFF, the cause is confirmed
// and a view-aware gate can replace the blunt force. Costs the tracker's culling/fade while on.
typedef unsigned long long (__fastcall *OcclUpdate_t)(void*, void*, void*, void*,
                                                      unsigned long long, unsigned long long);
static OcclUpdate_t o_OcclUpdate = nullptr;
static uintptr_t g_occlRva = 0;
static volatile long g_occlForce = 0;      // seeded from the ini, OcclForce
static volatile long g_occlCalls = 0, g_occlForced = 0;

// FALSIFIED 2026-07-22 (live): never called in gameplay (calls stayed 0 across a full run).
// Kept as a passive counter so a scene that DOES use it would show up in the toggle log.
static unsigned long long __fastcall Hook_OcclUpdate(void* tracker, void* p2, void* p3, void* view,
                                                     unsigned long long p5, unsigned long long p6) {
    long n = InterlockedIncrement(&g_occlCalls);
    if (n == 1) OLProxyLog("[OLVR][OCCL] occlusion tracker is LIVE (first UpdateAndGetCoverage call)");
    return o_OcclUpdate(tracker, p2, p3, view, p5, p6);
}

// ---- precomputed visibility (PVS) --------------------------------------------------------
// 2026-07-22. The tracker hypothesis was FALSIFIED live: hook installed, force ON,
// calls=0 - the tracker is never invoked in gameplay. Video evidence instead: whole meshes
// (the window assembly) skipped in the RIGHT eye only, camera stationary, quasi-periodic.
//
// New mechanism, read out of OLGame_R 0x1111030 (SceneVisibility.cpp, PrecomputedVisibility
// strings): UE3's PRECOMPUTED per-cell visibility. The view's ORIGIN picks a baked cell; the
// cell's zlib chunk holds a bitfield of which static primitives are visible from that cell;
// the caller (InitViews) culls whole meshes with it. The eyes' origins differ by the stereo
// offset, so near a cell boundary - with the camcorder sway moving the camera - one eye's
// lookup lands in a different cell whose baked data hides the mesh. One view per frame (AER)
// = one verdict = clean; two views = two verdicts = props wink out in one eye.
//
// Fingerprinted into shipping at 0x8A7810 (unique match on the scene+0x5178 handler read +
// view+0x3A0 origin reads; decompile-verified, single caller = InitViews at 0x8CE050).
//
// The test: return NULL - the legitimate "no baked data for this cell" answer, which the
// caller must treat as fully visible. Shares the OcclForce flag with the tracker bypass above.
typedef long long (__fastcall *PvsGet_t)(void*, void*, void*);
static PvsGet_t o_PvsGet = nullptr;
static uintptr_t g_pvsRva = 0;
static volatile long g_pvsCalls = 0, g_pvsForced = 0;

// FALSIFIED 2026-07-22 (live): bypassed for whole toggle windows (forced counter ran),
// flicker unchanged. Passive counter only now.
static long long __fastcall Hook_PvsGet(void* state, void* view, void* scene) {
    long n = InterlockedIncrement(&g_pvsCalls);
    if (n == 1) OLProxyLog("[OLVR][PVS] precomputed visibility is LIVE (first lookup)");
    return o_PvsGet(state, view, scene);
}

// OcclForce now arms exactly ONE thing: the d3d9-level hardware occlusion-query result
// force in d3d9_proxy.cpp (see [HWQ] there). One experiment at a time, per the standing rule.
extern "C" int OLUE3_OcclBypassOn() { return g_occlForce ? 1 : 0; }
extern "C" void OLProxy_HwqStats(long* created, long* forced);

void OLUE3_ToggleOcclForce() {
    g_occlForce = g_occlForce ? 0 : 1;
    long qc = 0, qf = 0;
    OLProxy_HwqStats(&qc, &qf);
    char line[260];
    sprintf_s(line, "[OLVR][HWQ] occlusion-query bypass %s (queries created=%ld, results forced=%ld; "
                    "tracker calls=%ld, PVS calls=%ld) - %s",
              g_occlForce ? "ON" : "OFF", qc, qf, g_occlCalls, g_pvsCalls,
              g_occlForce ? "every query answers 'visible', nothing query-culled"
                          : "game's query culling back in charge");
    OLProxyLog(line);
}

// ---- diagnostic: which VIEW SLOT is the artifact attached to? ---------------------------
// CalcSceneView appends each view it builds to the family, so the FIRST one built becomes
// Views[0] (the primary) and the second becomes Views[1] (the secondary). If the flicker and
// colour drift really are "the engine under-processes the secondary view", then building the
// RIGHT eye first must move the artifact to the LEFT eye. If the artifact stays on the right
// regardless of slot, the secondary-view theory is wrong and the cause is on this mod's side --
// the eye offset, the capture, or the submit. One toggle, one run, and the answer redirects
// everything after it.
static volatile long g_buildRightFirst = 0;

void OLUE3_ToggleBuildOrder() {
    g_buildRightFirst = g_buildRightFirst ? 0 : 1;
    char line[200];
    sprintf_s(line, "[OLVR][S3] build order: %s is now the PRIMARY view (Views[0])",
              g_buildRightFirst ? "RIGHT eye" : "LEFT eye");
    OLProxyLog(line);
}

// Live A/B between the two stereo mechanisms, so one run can answer whether the flicker needs
// two views to exist at all:
//   SPLIT = two views per frame (both eyes in one frame, half-width each)
//   AER   = ONE view per frame, full screen, camera alternating left/right
// If the flicker survives AER, it cannot be caused by having two views - and every remaining
// theory I have is about having two views.
void OLUE3_ToggleAer() {
    if (!g_drawLo) { OLProxyLog("[OLVR][AER] needs DrawRva - not available"); return; }
    g_aerMode = g_aerMode ? 0 : 1;
    if (g_aerMode) g_splitOn = 0;          // mutually exclusive
    char line[160];
    sprintf_s(line, "[OLVR] MODE = %s", g_aerMode ? "AER (one full-screen view per frame)"
                                                  : "off (mono, no stereo)");
    OLProxyLog(line);
}

void OLUE3_ToggleSplit() {
    g_splitOn = g_splitOn ? 0 : 1;
    if (g_splitOn) g_aerMode = 0;          // mutually exclusive
    char line[160];
    sprintf_s(line, "[OLVR][S3] SPLIT %s (frames split=%ld, failed=%ld)",
              g_splitOn ? "ON - expect side-by-side" : "off", g_splitFrames, g_splitFail);
    OLProxyLog(line);
}

// ---- SFR: Same-Frame Rendering ------------------------------------------------------------
// The split renders one PRIMARY view and one SECONDARY view into halves of a frame. UE3 does
// not treat those two equally - post-processing, exposure and assorted per-view effects get the
// full treatment on the primary and a partial one on the secondary. That is the whole family of
// "wrong on the right eye only" artifacts (ME1 hit it as right-eye bloom and black characters),
// and it is NOT fixable by giving the secondary its own view state - tried, it changed nothing.
//
// SFR renders the whole frame TWICE per present, each pass a full-screen PRIMARY view with the
// camera shifted for that eye. Neither pass is ever a secondary, so there is nothing to
// mistreat. Costs a second full render; buys correctness. This is the shape ME1 settled on
// permanently after fighting the same artifacts.
//
// Pass 1 (left) is copied off the backbuffer before pass 2 (right) overwrites it.
typedef void* (__fastcall *Draw_t)(void*, void*, void*);
static Draw_t o_Draw = nullptr;
static uintptr_t g_drawRva = 0;
static volatile long g_sfrMode = 0;      // set when Mode=sfr
static volatile long g_inDraw = 0;
static long g_sfrFrames = 0, g_sfrMiss = 0;

extern "C" int OLProxy_CaptureLeftEye();
extern "C" int OLUE3_StereoMode() { return g_aerMode ? 3 : (g_sfrMode ? 2 : (g_splitOn ? 1 : 0)); }

static void* __fastcall Hook_Draw(void* self, void* viewport, void* canvas) {
    if (!g_sfrMode || g_inDraw) return o_Draw(self, viewport, canvas);

    InterlockedExchange(&g_inDraw, 1);
    void* stateL = nullptr;

    // pass 1 - LEFT eye, full screen, primary view
    InterlockedExchange(&g_eyePhase, 1);
    o_Draw(self, viewport, canvas);
    int got = OLProxy_CaptureLeftEye();

    // pass 2 - RIGHT eye, full screen, primary view. Its own view state so the two passes do
    // not ping-pong each other's exposure/occlusion history.
    InterlockedExchange(&g_eyePhase, 2);
    void* lp = g_localPlayer;
    if (g_stateR && lp) { stateL = SafeReadPtr(lp, kViewStateOff); SafeWritePtr(lp, kViewStateOff, g_stateR); }
    void* r = o_Draw(self, viewport, canvas);
    if (stateL && lp) SafeWritePtr(lp, kViewStateOff, stateL);

    InterlockedExchange(&g_eyePhase, 0);
    InterlockedExchange(&g_inDraw, 0);

    if (got) { ++g_sfrFrames; InterlockedExchange(&g_splitFresh, 30); } else ++g_sfrMiss;
    if (g_sfrFrames == 1) OLProxyLog("[OLVR][SFR] first same-frame pair rendered (two full primary views)");
    if (g_sfrFrames && (g_sfrFrames % 600) == 0) {
        char l[160]; sprintf_s(l, "[OLVR][SFR] alive: pairs=%ld captureMiss=%ld", g_sfrFrames, g_sfrMiss);
        OLProxyLog(l);
    }
    return r;
}

// ---- AER: alternate-eye rendering ----------------------------------------------------------
// Evidence (2026-07-22): swapping which eye is built first did NOT move the flicker, so it is
// not the engine mistreating a secondary view. The artifact follows the right HALF OF THE
// SCREEN - UE3's post chain misbehaves in the second half of a side-by-side frame. ME1 hit the
// same thing and its notes rule out patching the SBS data side.
//
// So: stop rendering halves. AER renders ONE full-screen view per frame with the camera on the
// left eye, then the next frame on the right, and the headset holds the other eye's last image.
// Every frame is an ordinary full-viewport primary render - the exact thing the engine is happy
// with - which sidesteps both the half-viewport post bug and the double-Draw black screen.
//
// The cost is honest: the two eyes are one frame apart, so fast motion can shear or shimmer
// unless the frame rate is steady. That is a real trade, not a free win.
extern "C" int OLUE3_CurrentEye() { return g_aerEye; }

// ---- the hook -------------------------------------------------------------------------
static void* __fastcall Hook_CalcSceneView(void* self, void* family, void* outLoc, void* outRot,
                                           void* viewport, void* drawer) {
    long n = InterlockedIncrement(&g_calls);
    g_localPlayer = self;

    // AER: single full-screen render, camera alternating per frame. No rect rewrite, no second
    // CalcSceneView call - the frame is exactly what the game would render on its own, just from
    // one eye's position.
    if (g_aerMode) {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        if (ra >= g_drawLo && ra < g_drawHi) {
            long f = InterlockedIncrement(&g_aerFrame);
            long eye = (f & 1) ? 1 : 0;
            InterlockedExchange(&g_aerEye, eye);
            InterlockedExchange(&g_eyePhase, eye ? 2 : 1);   // shifts the camera +/- halfEye
            void* v = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
            InterlockedExchange(&g_eyePhase, 0);
            CaptureRenderedFov(v);
            InterlockedExchange(&g_splitFresh, 30);
            if (f == 1) OLProxyLog("[OLVR][AER] alternate-eye rendering live (full-screen per frame)");
            return v;
        }
    }

    // SFR: no rect rewrite at all - each pass is already a full-screen primary view. Only the
    // eye offset (applied in GetPlayerViewPoint) and the rendered FOV are needed.
    if (g_sfrMode) {
        if (!g_stateTried && o_AllocViewState) {
            g_stateTried = true;
            __try { g_stateR = o_AllocViewState(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_stateR = nullptr; }
            char l[160];
            sprintf_s(l, "[OLVR][SFR] second view state %s (%p)", g_stateR ? "allocated" : "FAILED", g_stateR);
            OLProxyLog(l);
        }
        void* v = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
        CaptureRenderedFov(v);
        return v;
    }

    if (g_splitOn && !g_inSplit) {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        if (ra >= g_drawLo && ra < g_drawHi) {
            float save[4] = { 0, 0, 0, 0 };
            if (SafeReadBytes((char*)self + kRectOff, save, sizeof(save)) &&
                save[2] > 0.9f && save[3] > 0.9f) {          // only split a full-viewport player
                InterlockedExchange(&g_inSplit, 1);
                const float L[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
                const float R[4] = { 0.5f, 0.0f, 0.5f, 1.0f };
                void* left = nullptr; void* right = nullptr;
                void* stateL = SafeReadPtr(self, kViewStateOff);   // the game's own state = left eye
                if (!g_stateTried && o_AllocViewState && stateL) {
                    g_stateTried = true;
                    __try { g_stateR = o_AllocViewState(); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { g_stateR = nullptr; }
                    char l[160];
                    sprintf_s(l, "[OLVR][S3] right-eye view state %s (%p) - each eye now keeps its own "
                                 "exposure/occlusion history", g_stateR ? "ALLOCATED" : "alloc FAILED", g_stateR);
                    OLProxyLog(l);
                }

                const float* firstRect  = g_buildRightFirst ? R : L;
                const float* secondRect = g_buildRightFirst ? L : R;
                const long   firstPhase  = g_buildRightFirst ? 2 : 1;
                const long   secondPhase = g_buildRightFirst ? 1 : 2;
                void** firstOut  = g_buildRightFirst ? &right : &left;
                void** secondOut = g_buildRightFirst ? &left  : &right;

                if (SafeWriteRect(self, firstRect)) {
                    InterlockedExchange(&g_eyePhase, firstPhase);
                    *firstOut = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
                }
                if (SafeWriteRect(self, secondRect)) {
                    InterlockedExchange(&g_eyePhase, secondPhase);
                    if (g_stateR) SafeWritePtr(self, kViewStateOff, g_stateR);
                    *secondOut = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
                }
                InterlockedExchange(&g_eyePhase, 0);
                if (g_stateR && stateL) SafeWritePtr(self, kViewStateOff, stateL);   // ALWAYS restore
                SafeWriteRect(self, save);                    // ALWAYS restore, even on failure
                InterlockedExchange(&g_inSplit, 0);
                if (left && right) {
                    ++g_splitFrames;
                    CaptureRenderedFov(right);       // both eyes share it; right is the last built
                    InterlockedExchange(&g_splitFresh, 30);
                } else ++g_splitFail;
                if (g_splitFrames == 1)
                    OLProxyLog("[OLVR][S3] first split done - two views built in one frame");
                if ((g_splitFrames % 600) == 0) {
                    char l[160]; sprintf_s(l, "[OLVR][S3] split alive: ok=%ld fail=%ld", g_splitFrames, g_splitFail);
                    OLProxyLog(l);
                }
                if (left) return left;                        // hand the engine the LEFT view
            }
        }
    }

    // Pass-through path. Also the layout dump: the first few calls log where the rect and the
    // FSceneView matrices actually live, so the offsets above are re-confirmed on every run
    // rather than trusted from a previous session.
    {
        void* view = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
        if (n <= 3 && InterlockedIncrement(&g_logged) <= 3) {
            char line[256];
            sprintf_s(line, "[OLVR][S3] CalcSceneView call#%ld this=%p family=%p viewport=%p -> view=%p", n, self, family, viewport, view);
            OLProxyLog(line);
            {   // which view-state source is live?
                unsigned char flag = 0; SafeReadBytes((char*)self + 0xC8, &flag, 1);
                unsigned int gflag = 0; SafeReadBytes((const void*)(g_base + 0x200A3DC), &gflag, 4);
                sprintf_s(line, "[OLVR][S3] viewstate branch: this+0xC8&1=%u globalFlag=%u -> %s "
                                "(sharedGetter hits=%ld, oursReturned=%ld)",
                          (unsigned)(flag & 1), gflag,
                          ((flag & 1) == 0 || gflag != 0) ? "LocalPlayer+0xB0" : "SHARED global",
                          g_sharedHits, g_sharedGaveOurs);
                OLProxyLog(line);
            }
            ScanForRect(self);
            ScanSceneView(view);
        }
        if ((n % 600) == 0) { char l[128]; sprintf_s(l, "[OLVR][S3] CalcSceneView calls=%ld (alive)", n); OLProxyLog(l); }
        return view;
    }

    return o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
}

// ---- setup ----------------------------------------------------------------------------
// An RVA is only valid for the exact build it came from. Hooking base+RVA in the WRONG exe
// lands on an arbitrary function and crashes the game. Two gates before anything is touched:
//   1. the running exe's filename must match the ini's Exe= key
//   2. the bytes at base+RVA must match the ini's Prologue= key (as read from that exe on disk)
static wchar_t g_wantExe[64] = { 0 };
static unsigned char g_wantBytes[32] = { 0 };
static int g_wantLen = 0;

static bool ExeMatches() {
    if (!g_wantExe[0]) return true;               // no constraint set
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return _wcsicmp(name, g_wantExe) == 0;
}
static bool PrologueMatches(const void* target) {
    if (g_wantLen <= 0) return true;              // no constraint set
    unsigned char got[32] = { 0 };
    if (!SafeReadBytes(target, got, (size_t)g_wantLen)) return false;
    return memcmp(got, g_wantBytes, (size_t)g_wantLen) == 0;
}
static int ParseHexInto(const wchar_t* hex, unsigned char* out, int cap) {
    int len = 0;
    for (const wchar_t* p = hex; p[0] && p[1] && len < cap; ) {
        if (*p == L' ') { ++p; continue; }
        wchar_t byte[3] = { p[0], p[1], 0 };
        out[len++] = (unsigned char)wcstoul(byte, nullptr, 16);
        p += 2;
    }
    return len;
}
static void ParseHex(const wchar_t* hex) { g_wantLen = ParseHexInto(hex, g_wantBytes, 32); }

// The occlusion-tracker hook has its own prologue gate: a second RVA means a second chance to
// hook garbage on a wrong build, so it gets the same exe+prologue treatment as CalcSceneView.
static unsigned char g_occlWantBytes[32] = { 0 };
static int g_occlWantLen = 0;
static unsigned char g_pvsWantBytes[32] = { 0 };
static int g_pvsWantLen = 0;

static void ReadConfig() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%soutlastvr.ini", exe);

    wchar_t val[64] = { 0 };
    GetPrivateProfileStringW(L"Stage3", L"CalcSceneViewRva", L"0", val, 64, ini);
    g_cvRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"Mode", L"probe", val, 64, ini);
    g_splitMode = (_wcsicmp(val, L"split") == 0);
    g_sfrMode   = (_wcsicmp(val, L"sfr") == 0) ? 1 : 0;
    g_aerMode   = (_wcsicmp(val, L"aer") == 0) ? 1 : 0;
    GetPrivateProfileStringW(L"Stage3", L"SharedStateRva", L"0", val, 64, ini);
    g_sharedRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"StateCreatorRva", L"0", val, 64, ini);
    g_creatorRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"AllocViewStateRva", L"0", val, 64, ini);
    g_avsRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"HalfEyeUU", L"1.6", val, 64, ini);
    g_halfEyeUU = (float)_wtof(val);
    GetPrivateProfileStringW(L"Stage3", L"WorldToMeters", L"50", val, 64, ini);
    { float w = (float)_wtof(val); if (w >= 5.0f && w <= 500.0f) g_worldToMeters = w; }
    g_headTrack = GetPrivateProfileIntW(L"Stage3", L"HeadTracking", 1, ini) ? 1 : 0;
    g_headRoll  = GetPrivateProfileIntW(L"Stage3", L"HeadRoll", 0, ini) ? 1 : 0;
    g_headPos   = GetPrivateProfileIntW(L"Stage3", L"HeadPosition", 1, ini) ? 1 : 0;
    g_camProbe  = GetPrivateProfileIntW(L"Stage3", L"CamProbe", 0, ini) ? 1 : 0;   // [CAMPROBE] dev only
    g_boneProbe = GetPrivateProfileIntW(L"Stage3", L"BoneProbe", 0, ini) ? 1 : 0;  // [BONEPROBE] dev only
    g_boneWiggle = GetPrivateProfileIntW(L"Stage3", L"BoneWiggle", 0, ini);        // [WIGGLE] dev only
    g_boneWatch = GetPrivateProfileIntW(L"Stage3", L"BoneWatch", 0, ini) ? 1 : 0;  // [BONEWATCH] dev only
    g_skelProbe = GetPrivateProfileIntW(L"Stage3", L"SkelCtrlProbe", 0, ini) ? 1 : 0;  // [SKELCTRL] dev only
    g_skelProp = GetPrivateProfileIntW(L"Stage3", L"SkelPropProbe", 0, ini) ? 1 : 0;  // [SKELPROP] dev only
    g_armDrive = GetPrivateProfileIntW(L"Stage3", L"ArmDrive", 0, ini) ? 1 : 0;   // [ARMDRIVE]
    GetPrivateProfileStringW(L"Stage3", L"LeanGain", L"1.0", val, 64, ini);
    OLUE3_SetLeanGain((float)_wtof(val));
    GetPrivateProfileStringW(L"Stage3", L"GetViewPointRva", L"0", val, 64, ini);
    g_vpRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"DrawRva", L"0", val, 64, ini);
    uintptr_t drawRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"DrawSize", L"0", val, 64, ini);
    uintptr_t drawSize = (uintptr_t)wcstoull(val, nullptr, 0);
    g_drawRva = drawRva;
    g_drawLo = drawRva ? g_base + drawRva : 0;
    g_drawHi = drawRva ? g_base + drawRva + drawSize : 0;
    GetPrivateProfileStringW(L"Stage3", L"Exe", L"", g_wantExe, 64, ini);
    wchar_t hex[80] = { 0 };
    GetPrivateProfileStringW(L"Stage3", L"Prologue", L"", hex, 80, ini);
    ParseHex(hex);
    GetPrivateProfileStringW(L"Stage3", L"OcclRva", L"0", val, 64, ini);
    g_occlRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"OcclPrologue", L"", hex, 80, ini);
    g_occlWantLen = ParseHexInto(hex, g_occlWantBytes, 32);
    g_occlForce = GetPrivateProfileIntW(L"Stage3", L"OcclForce", 0, ini) ? 1 : 0;
    g_cineFovOn = GetPrivateProfileIntW(L"Stage3", L"CineFov", 1, ini) ? 1 : 0;   // [CINEFOV]
    g_wideFov = GetPrivateProfileIntW(L"Render", L"WideFov", 0, ini) ? 1 : 0;     // [WIDEFOV]
    if (g_wideFov) OLProxyLog("[OLVR][WIDEFOV] ON - rendering at the headset's own width "
                              "(costs sharpness, fills the eye; [Render] WideFov=0 reverts)");
    GetPrivateProfileStringW(L"Stage3", L"PvsRva", L"0", val, 64, ini);
    g_pvsRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"PvsPrologue", L"", hex, 80, ini);
    g_pvsWantLen = ParseHexInto(hex, g_pvsWantBytes, 32);

    char line[512];
    char inia[MAX_PATH]; size_t n = 0; wcstombs_s(&n, inia, ini, _TRUNCATE);
    sprintf_s(line, "[OLVR][S3] config %s -> CalcSceneViewRva=0x%llx mode=%s drawRange=[%p,%p)",
              inia, (unsigned long long)g_cvRva, g_splitMode ? "split" : "probe",
              (void*)g_drawLo, (void*)g_drawHi);
    OLProxyLog(line);
}

void OLUE3_Init() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    char line[256];
    sprintf_s(line, "[OLVR][S3] module base=%p", (void*)g_base);
    OLProxyLog(line);

    ReadConfig();
    if (!g_cvRva) { OLProxyLog("[OLVR][S3] no CalcSceneViewRva set -> Stage 3 idle (flat XR only)"); return; }

    if (!ExeMatches()) {
        char want[64]; size_t wn = 0; wcstombs_s(&wn, want, g_wantExe, _TRUNCATE);
        sprintf_s(line, "[OLVR][S3] SKIPPED: this RVA belongs to %s, not the exe that's running", want);
        OLProxyLog(line); return;
    }
    void* target = (void*)(g_base + g_cvRva);
    if (!PrologueMatches(target)) {
        OLProxyLog("[OLVR][S3] SKIPPED: bytes at base+RVA do not match the expected prologue (wrong build?)");
        return;
    }
    OLProxyLog("[OLVR][S3] target verified (exe + prologue match)");
    if (g_aerMode && !g_drawLo) {
        OLProxyLog("[OLVR][AER] Mode=aer needs DrawRva to identify the render call site -> AER off");
        g_aerMode = 0;
    }
    if (g_aerMode) OLProxyLog("[OLVR][AER] mode=aer: one full-screen eye per frame");
    if (g_splitMode && !g_drawLo) {
        OLProxyLog("[OLVR][S3] Mode=split but no DrawRva set -> split DISABLED (it would split "
                   "every call site, including the query views). Set DrawRva/DrawSize.");
        g_splitMode = false;
    }
    g_splitOn = g_splitMode ? 1 : 0;
    // ALREADY_INITIALIZED is success, not failure: the resolution hooks in d3d9_proxy.cpp run
    // first and initialise MinHook. Treating it as fatal here skipped the ENTIRE engine hook,
    // which killed the split and left only a mono flat panel - "no VR anymore", 2026-07-22.
    {
        MH_STATUS ms = MH_Initialize();
        if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
            char l[128]; sprintf_s(l, "[OLVR][S3] MH_Initialize FAILED (status=%d)", (int)ms);
            OLProxyLog(l); return;
        }
    }
    if (MH_CreateHook(target, (void*)&Hook_CalcSceneView, (void**)&o_CalcSceneView) != MH_OK) {
        sprintf_s(line, "[OLVR][S3] MH_CreateHook FAILED at %p", target); OLProxyLog(line); return;
    }
    if (MH_EnableHook(target) != MH_OK) { OLProxyLog("[OLVR][S3] MH_EnableHook FAILED"); return; }
    sprintf_s(line, "[OLVR][S3] CalcSceneView hooked at %p (base+0x%llx) mode=%s",
              target, (unsigned long long)g_cvRva, g_splitMode ? "split" : "probe");
    OLProxyLog(line);

    // GetPlayerViewPoint = the parallax injection point. Without it the split still works, it
    // just has no depth, so a failure here degrades to flat rather than breaking the split.
    if (g_vpRva) {
        void* vp = (void*)(g_base + g_vpRva);
        if (MH_CreateHook(vp, (void*)&Hook_GetViewPoint, (void**)&o_GetViewPoint) == MH_OK &&
            MH_EnableHook(vp) == MH_OK) {
            sprintf_s(line, "[OLVR][S3] GetPlayerViewPoint hooked at %p (base+0x%llx) halfEye=%.2fuu",
                      vp, (unsigned long long)g_vpRva, g_halfEyeUU);
        } else {
            sprintf_s(line, "[OLVR][S3] GetPlayerViewPoint hook FAILED at %p - split will be flat", vp);
        }
        OLProxyLog(line);
    } else {
        OLProxyLog("[OLVR][S3] no GetViewPointRva -> split will render two identical views (no depth)");
    }

    // [CINEFOV] the FOV getter CalcSceneView calls (0x60F4F0, from the disassembly). Hooking
    // the RETURN VALUE is why this needs no memory scan and no restore. Prologue-verified, so a
    // patched or different build simply leaves it uninstalled instead of hooking a wrong address.
    {
        void* fg = (void*)(g_base + kFovGetterRva);
        unsigned char have[sizeof(kFovGetterPrologue)] = { 0 };
        if (!SafeReadBytes((char*)fg, have, sizeof(have)) ||
            memcmp(have, kFovGetterPrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][CINEFOV] FOV getter prologue MISMATCH at %p - not hooking "
                            "(cutscenes stay at the game's own width)", fg);
        } else if (MH_CreateHook(fg, (void*)&Hook_GetFovAngle, (void**)&o_GetFovAngle) == MH_OK &&
                   MH_EnableHook(fg) == MH_OK) {
            g_fovHookOk = 1;
            sprintf_s(line, "[OLVR][CINEFOV] FOV getter hooked at %p (base+0x%llX) - cutscenes and "
                            "menus will render %.0f deg/eye like gameplay",
                      fg, (unsigned long long)kFovGetterRva, kCineTargetDeg);
        } else {
            sprintf_s(line, "[OLVR][CINEFOV] FOV getter hook FAILED at %p", fg);
        }
        OLProxyLog(line);
    }

    // [NVLIGHT] USpotLightComponent::SetRotation - lets the camcorder/NV light follow the head.
    // Prologue-verified like every other hook here, so a different build declines instead of
    // patching a wrong address.
    {
        void* sr = (void*)(g_base + kSpotSetRotRva);
        unsigned char have[sizeof(kSpotSetRotPrologue)] = { 0 };
        if (!SafeReadBytes((char*)sr, have, sizeof(have)) ||
            memcmp(have, kSpotSetRotPrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][NVLIGHT] SpotLight SetRotation prologue MISMATCH at %p - not "
                            "hooking (the NV light will not follow the head)", sr);
        } else if (MH_CreateHook(sr, (void*)&Hook_SpotSetRotation, (void**)&o_SpotSetRot) == MH_OK &&
                   MH_EnableHook(sr) == MH_OK) {
            sprintf_s(line, "[OLVR][NVLIGHT] SpotLight SetRotation hooked at %p (base+0x%llX) - the "
                            "camcorder/NV light will follow your head", sr, (unsigned long long)kSpotSetRotRva);
        } else {
            sprintf_s(line, "[OLVR][NVLIGHT] SpotLight SetRotation hook FAILED at %p", sr);
        }
        OLProxyLog(line);
    }

    // SFR hooks Draw itself so it can run the whole render twice.
    if (g_sfrMode) {
        if (!g_drawRva) {
            OLProxyLog("[OLVR][SFR] Mode=sfr but no DrawRva -> SFR DISABLED");
            g_sfrMode = 0;
        } else {
            void* dt = (void*)(g_base + g_drawRva);
            if (MH_CreateHook(dt, (void*)&Hook_Draw, (void**)&o_Draw) == MH_OK && MH_EnableHook(dt) == MH_OK) {
                sprintf_s(line, "[OLVR][SFR] Draw hooked at %p (base+0x%llx) - two full primary passes per frame",
                          dt, (unsigned long long)g_drawRva);
            } else {
                sprintf_s(line, "[OLVR][SFR] Draw hook FAILED at %p -> SFR off", dt);
                g_sfrMode = 0;
            }
            OLProxyLog(line);
        }
    }

    if (g_creatorRva) o_StateCreator = (StateCreator_t)(g_base + g_creatorRva);
    if (g_sharedRva) {
        void* gs = (void*)(g_base + g_sharedRva);
        if (MH_CreateHook(gs, (void*)&Hook_GetSharedState, (void**)&o_GetSharedState) == MH_OK &&
            MH_EnableHook(gs) == MH_OK) {
            sprintf_s(line, "[OLVR][S3] shared view-state getter hooked at base+0x%llx", (unsigned long long)g_sharedRva);
        } else {
            sprintf_s(line, "[OLVR][S3] shared view-state getter hook FAILED");
        }
        OLProxyLog(line);
    }

    // The Outlast-custom occlusion tracker (see Hook_OcclUpdate above). A second RVA gets its
    // own prologue gate - same reasoning as CalcSceneView's.
    if (g_occlRva) {
        void* ot = (void*)(g_base + g_occlRva);
        bool ok = true;
        if (g_occlWantLen > 0) {
            unsigned char got[32] = { 0 };
            ok = SafeReadBytes(ot, got, (size_t)g_occlWantLen) &&
                 memcmp(got, g_occlWantBytes, (size_t)g_occlWantLen) == 0;
        }
        if (!ok) {
            OLProxyLog("[OLVR][OCCL] SKIPPED: bytes at base+OcclRva do not match OcclPrologue (wrong build?)");
        } else if (MH_CreateHook(ot, (void*)&Hook_OcclUpdate, (void**)&o_OcclUpdate) == MH_OK &&
                   MH_EnableHook(ot) == MH_OK) {
            sprintf_s(line, "[OLVR][OCCL] UpdateAndGetCoverage hooked at base+0x%llx, force-visible %s", (unsigned long long)g_occlRva, g_occlForce ? "ON" : "off");
            OLProxyLog(line);
        } else {
            OLProxyLog("[OLVR][OCCL] hook FAILED");
        }
    }

    // Precomputed visibility (see Hook_PvsGet above). Same per-RVA prologue gate.
    if (g_pvsRva) {
        void* pt = (void*)(g_base + g_pvsRva);
        bool ok = true;
        if (g_pvsWantLen > 0) {
            unsigned char got[32] = { 0 };
            ok = SafeReadBytes(pt, got, (size_t)g_pvsWantLen) &&
                 memcmp(got, g_pvsWantBytes, (size_t)g_pvsWantLen) == 0;
        }
        if (!ok) {
            OLProxyLog("[OLVR][PVS] SKIPPED: bytes at base+PvsRva do not match PvsPrologue (wrong build?)");
        } else if (MH_CreateHook(pt, (void*)&Hook_PvsGet, (void**)&o_PvsGet) == MH_OK &&
                   MH_EnableHook(pt) == MH_OK) {
            sprintf_s(line, "[OLVR][PVS] GetPrecomputedVisibilityData hooked at base+0x%llx, bypass %s", (unsigned long long)g_pvsRva, g_occlForce ? "ON" : "off");
            OLProxyLog(line);
        } else {
            OLProxyLog("[OLVR][PVS] hook FAILED");
        }
    }

    // AllocateViewState: resolved, not hooked. It just needs to be callable once.
    if (g_avsRva) {
        o_AllocViewState = (AllocViewState_t)(g_base + g_avsRva);
        sprintf_s(line, "[OLVR][S3] AllocateViewState at base+0x%llx", (unsigned long long)g_avsRva);
    } else {
        sprintf_s(line, "[OLVR][S3] no AllocViewStateRva -> both eyes share one view state (expect "
                        "flicker + per-eye colour drift)");
    }
    OLProxyLog(line);
}
