// d3d9_proxy.cpp - Outlast (x64) d3d9.dll proxy: frame hook + OpenXR flat presentation.
// =============================================================================
// OLGame.exe (Binaries\Win64) statically imports exactly 4 names from d3d9.dll:
//   Direct3DCreate9, D3DPERF_BeginEvent, D3DPERF_EndEvent, D3DPERF_SetOptions
// so dropping this DLL next to the exe loads it first; it forwards everything to the
// real system d3d9.dll and patch two vtable slots:
//   IDirect3D9::CreateDevice   (slot 16) -> grab the device, install device hooks
//   IDirect3DDevice9::Present  (slot 17) -> per-frame XR submit
//   IDirect3DDevice9::Reset    (slot 16) -> log res changes
//
// Vtable-slot patching (not MinHook) is deliberate: it's what the Dishonored D3D9 probe
// uses and D3D9 has none of the flip-model/overlay vtable contention that forced the
// swapchain-wrapper approach on the DXGI games.
//
// Keys (polled on a dedicated thread; the game keeps all its input). This is the WHOLE list, and it
// is short on purpose: everything a player can press has to be something a stranger can press
// safely, so the fourteen developer chords that used to live here were removed for release.
//   INSERT = open and close the settings menu (arrows / WASD / mouse / d-pad inside it)
//   G      = recenter, rebindable in the menu, saved to [Input] RecenterKey
//   R3 x2  = recenter from the controller, rebindable, saved to [Input] RecenterPad
// =============================================================================

#include <Windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include <ShlObj.h>
#include <math.h>
#include "ol_xr.h"
#include "ol_ue3.h"
#include "ol_menu.h"
#include "MinHook.h"

// ---------------------------------------------------------------------------
// logging  ->  %LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;

static void OpenLog() {
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) || !base) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf_s(dir, L"%s\\OutlastVR", base);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(path, L"%s\\OutlastVR_Log.txt", dir);
    CoTaskMemFree(base);
    // Share the handle for reading. _wfopen_s locks the file for the whole session, so the log
    // cannot be read WHILE the game runs - and this log is the only instrument for diagnosing a
    // live session. _wfsopen with _SH_DENYWR keeps exclusive write access but lets a reader in.
    g_log = _wfsopen(path, L"w", _SH_DENYWR);
}
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}
extern "C" void OLProxyLog(const char* line) { Log("%s", line); }

// ---------------------------------------------------------------------------
// real d3d9.dll
// ---------------------------------------------------------------------------
static HMODULE g_realD3D9 = nullptr;
static void EnsureRealD3D9() {
    if (g_realD3D9) return;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\d3d9.dll", sys);
    g_realD3D9 = LoadLibraryW(path);
}

// ---------------------------------------------------------------------------
// [SCREQ] render ABOVE the desktop resolution
// ---------------------------------------------------------------------------
// The VR image can never be sharper than the frame the game actually rendered, and the split
// halves that horizontally: at the observed 2560x1440 windowed backbuffer each eye gets only
// 1280x1440 before it is stretched across ~90 degrees of headset.
//
// Outlast boots 3840x2160 exclusive-fullscreen and then Resets to a WINDOWED backbuffer sized
// like the desktop of the monitor its window is on. ME1's mechanism (spoof GetMonitorInfoW,
// [[me1-render-above-display-res]]) does NOT port: the live log showed 11 spoofed rects and the
// game still asked 2560x1440. So the monitor-rect spoof stays (harmless, may help elsewhere)
// but the real lever is the D3D-side desktop query, GetAdapterDisplayMode - hooked below.
// Windowed D3D9 has no display-mode list to satisfy, so a backbuffer larger than the desktop is
// legal - the window simply overhangs the screen, which is irrelevant because the backbuffer
// is what gets captured, not the window.
//
// HARD RULE learned 2026-07-22 (the broken-resolution/no-VR build): the backbuffer must
// always be the size the ENGINE asked for. Forcing pp bigger at Reset makes the engine render
// its believed size into the corner of the bigger buffer, and the half-split then cuts through
// the middle of the right eye. Make the engine ask bigger; never resize behind its back.
//
// Armed ONLY when the ini asks for more than the real desktop; at or below native every hook is
// a pass-through, so a smaller monitor is never harmed. Only the PRIMARY rect is rewritten, so a
// second display stays honest.
typedef BOOL (WINAPI *GetMonitorInfoW_t)(HMONITOR, LPMONITORINFO);
typedef BOOL (WINAPI *GetMonitorInfoA_t)(HMONITOR, LPMONITORINFO);
static GetMonitorInfoW_t o_GetMonitorInfoW = nullptr;
static GetMonitorInfoA_t o_GetMonitorInfoA = nullptr;
static LONG g_reqW = 0, g_reqH = 0;         // what the ini asked for
static LONG g_spoofArmed = 0;
static volatile LONG g_dispqHits = 0;

// Spoof ANY monitor that is smaller than the request, not just the primary.
// Live log 2026-07-22 proved why: the primary IS 3840x2160, but the game resets to 2560x1440 -
// the size of the SECOND monitor, because that is where its window lives. A primary-only spoof
// rewrote a rect the game never asked about. Monitors already big enough are left honest, so
// this stays a pass-through wherever it isn't needed.
static void SpoofPrimary(LPMONITORINFO mi) {
    if (!g_spoofArmed || !mi) return;
    const LONG w = mi->rcMonitor.right - mi->rcMonitor.left;
    const LONG h = mi->rcMonitor.bottom - mi->rcMonitor.top;
    if (w >= g_reqW && h >= g_reqH) return;
    mi->rcMonitor.right  = mi->rcMonitor.left + g_reqW;
    mi->rcMonitor.bottom = mi->rcMonitor.top  + g_reqH;
    mi->rcWork = mi->rcMonitor;              // rcWork must follow or the engine clamps on that
    InterlockedIncrement(&g_dispqHits);
}
static BOOL WINAPI Hook_GetMonitorInfoW(HMONITOR h, LPMONITORINFO mi) {
    BOOL r = o_GetMonitorInfoW(h, mi);
    if (r) SpoofPrimary(mi);
    return r;
}
static BOOL WINAPI Hook_GetMonitorInfoA(HMONITOR h, LPMONITORINFO mi) {
    BOOL r = o_GetMonitorInfoA(h, mi);
    if (r) SpoofPrimary(mi);
    return r;
}

// The monitor-rect spoof alone was NOT it: 11 rects were rewritten and the game still asked
// 2560x1440 (live log 2026-07-22). The D3D-native way a UE3 D3D9 game reads "the desktop
// resolution" for windowed mode is IDirect3D9::GetAdapterDisplayMode - a vtable already owned.
// Raise any reported mode smaller than the request so the ENGINE itself asks for the big
// backbuffer; then its scene viewport, its UI, and the buffer all agree by construction.
typedef HRESULT (STDMETHODCALLTYPE *GetAdapterDisplayMode_t)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
static GetAdapterDisplayMode_t o_GetAdapterDisplayMode = nullptr;
static volatile LONG g_admHits = 0;
static HRESULT STDMETHODCALLTYPE Hook_GetAdapterDisplayMode(IDirect3D9* This, UINT adapter, D3DDISPLAYMODE* m) {
    HRESULT hr = o_GetAdapterDisplayMode(This, adapter, m);
    if (SUCCEEDED(hr) && m && g_spoofArmed &&
        ((LONG)m->Width < g_reqW || (LONG)m->Height < g_reqH)) {
        LONG n = InterlockedIncrement(&g_admHits);
        if (n <= 3)
            Log("[OLVR][SCREQ] GetAdapterDisplayMode(adapter=%u) %ux%u -> spoofed %ldx%ld",
                adapter, m->Width, m->Height, g_reqW, g_reqH);
        m->Width = (UINT)g_reqW; m->Height = (UINT)g_reqH;
    }
    return hr;
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *CreateQuery_t)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT (STDMETHODCALLTYPE *QueryGetData_t)(IDirect3DQuery9*, void*, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static CreateDevice_t o_CreateDevice = nullptr;
static Present_t      o_Present = nullptr;
static Reset_t        o_Reset = nullptr;
static LONG           g_d3d9HookDone = 0;
static LONG           g_devHookDone = 0;
static volatile LONG  g_presentCount = 0;
static volatile LONG  g_firstRunPending = 0;      // open the settings menu once, ever
static const LONG     kFirstRunOpenAtPresent = 400;
static volatile LONG  g_xrEnabled = 1;      // XR submit on by default; a crash-safe off switch
static volatile LONG  g_xrAllowed = 1;      // outlastvr.ini [XR] Enabled - set 0 to run flat
static volatile LONG  g_keepVsync = 0;      // [XR] KeepGameVsync=1 - leave the game's vsync alive (see [PACING])

// ---- SFR support: hold the device, and keep a copy of the left-eye frame ----------------
// SFR renders the whole frame twice per present, each pass a full-screen PRIMARY view. Pass 1
// (left eye) has to be saved off the backbuffer before pass 2 overwrites it.
static IDirect3DDevice9*  g_dev = nullptr;
static IDirect3DSurface9* g_leftRT = nullptr;
static UINT g_leftW = 0, g_leftH = 0;

extern "C" IDirect3DSurface9* OLProxy_LeftEye() { return g_leftRT; }

extern "C" int OLProxy_CaptureLeftEye() {
    if (!g_dev) return 0;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return 0;
    D3DSURFACE_DESC sd;
    if (FAILED(bb->GetDesc(&sd))) { bb->Release(); return 0; }
    if (!g_leftRT || g_leftW != sd.Width || g_leftH != sd.Height) {
        if (g_leftRT) { g_leftRT->Release(); g_leftRT = nullptr; }
        if (FAILED(g_dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0,
                                             FALSE, &g_leftRT, nullptr))) {
            g_leftRT = nullptr; bb->Release();
            Log("[OLVR][SFR] left-eye RT create FAILED %ux%u", sd.Width, sd.Height);
            return 0;
        }
        g_leftW = sd.Width; g_leftH = sd.Height;
        Log("[OLVR][SFR] left-eye RT %ux%u", sd.Width, sd.Height);
    }
    HRESULT hr = g_dev->StretchRect(bb, nullptr, g_leftRT, nullptr, D3DTEXF_NONE);
    bb->Release();
    return SUCCEEDED(hr) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// [UIMIRROR] draw the UI into BOTH eyes instead of chopping it in half
// ---------------------------------------------------------------------------
// The split renders the WORLD twice, into the left and right halves of the backbuffer. The UI
// is not part of that: UE3 draws the HUD and the menus ONCE, across the full frame, after both
// views are done. The frame is then cut down the middle, so the left half of the UI goes to the
// left eye and the right half to the right eye - each eye sees a DIFFERENT piece of the menu,
// centred. The brain fuses them, so left-of-centre content appears where right-of-centre content
// should be. That is exactly the "small and cross eyed, left is on the right" symptom, and it is
// the same class of bug ME2 hit ("HUD was RIGHT-EYE ONLY") - see [[me2-vr-ui-catchers]].
//
// Fix: catch UI draws and issue each one TWICE, once into a box inside the left half and once
// into the matching box in the right half, instead of once across the whole frame. Both eyes
// then get the COMPLETE UI at the same place, so it fuses at screen depth.
//
// The box is aspect-corrected rather than just "the half". An eye's image is (bbW/2 x bbH)
// pixels stretched across the rendered FOV, so squeezing a 16:9 UI into the raw half would show
// it stretched - ME2's [UIRATIO] bug. Solving for equal angular aspect gives
//     boxH = bbH * tan(fovH/2) / (tan(fovV/2) * uiAspect)
// which at 90x96 deg and a 16:9 source is ~51% of the height, centred.
//
// Classifier (ME2's, which they validated): a UI draw is alpha-blended with dest=INV_SRC_ALPHA
// and depth testing OFF. Full-screen post/darken/multiply quads fail that test and are left
// alone, so they cannot leak into one eye. On top of that the draw must span the FULL
// backbuffer width: during a split the engine renders each eye through a HALF-width viewport, so
// anything still full-width is by construction not part of either eye's scene. That second gate
// comes free from the split itself and is the strong one.
static UINT g_bbW = 0, g_bbH = 0;
// MODES: 0 = off, 1 = ON (default), 2 = MENUS-ONLY. Set in the ini, [Render] UiMirror.
//
// History, because this has now failed in two opposite directions in one day:
//  * Mirroring every candidate WASHED THE COLOURS OUT. Cause found: the classifier treated an
//    untextured draw as UI, so Outlast's own full-screen darkening/fade passes were relocated
//    into the half-height box and the frame stopped being darkened. Fixed properly in
//    ClassifySourceTex - a HUD element is TEXTURED, a darkening pass is not.
//  * Volume-gating it to menus (27+ catches/frame) then LOST THE CAMCORDER RECORD ICON, because
//    the in-game HUD is exactly the low-volume case that gate was designed to exclude. Volume
//    was never the right question; it was a proxy for a classifier that was broken.
// So: mirror on the corrected classifier, all the time. Mode 2 keeps the volume gate as a
// fallback in case some other overlay slips through - it is a diagnostic, not the plan.
#define UIMIRROR_OFF 0
#define UIMIRROR_ON 1
#define UIMIRROR_MENUS 2
static volatile LONG g_uiMirror = UIMIRROR_ON;
static volatile LONG g_uiMirrored = 0, g_uiSeen = 0;
static volatile LONG g_uiFrameHits = 0, g_uiFrameHitsLast = 0;
static volatile LONG g_uiRejNoTex = 0, g_uiRejFull = 0;   // why candidates were passed through
static volatile LONG g_uiAutoArmed = 0;      // mode 2 only: is a menu currently open?
static LONG g_uiQuietFrames = 0;
static const LONG kUiMenuOn   = 15;
static const LONG kUiMenuOff  = 5;
static const LONG kUiQuietMax = 30;

// Called once per Present with the count of UI candidates seen in the frame just finished.
static void UiMirrorAutoTick(LONG hits) {
    if (g_uiMirror != UIMIRROR_MENUS) { g_uiAutoArmed = 0; g_uiQuietFrames = 0; return; }
    if (hits >= kUiMenuOn) {
        g_uiQuietFrames = 0;
        if (!InterlockedExchange(&g_uiAutoArmed, 1))
            Log("[OLVR][UIMIRROR] menus-only: armed (%ld UI draws in one frame)", hits);
    } else if (g_uiAutoArmed && hits < kUiMenuOff) {
        if (++g_uiQuietFrames >= kUiQuietMax) {
            InterlockedExchange(&g_uiAutoArmed, 0);
            g_uiQuietFrames = 0;
            Log("[OLVR][UIMIRROR] menus-only: disarmed (gameplay volume)");
        }
    } else if (g_uiAutoArmed) {
        g_uiQuietFrames = 0;
    }
}
static bool UiMirrorActiveNow() {
    const LONG m = g_uiMirror;
    return m == UIMIRROR_ON || (m == UIMIRROR_MENUS && g_uiAutoArmed);
}

typedef HRESULT (STDMETHODCALLTYPE *SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

// FALSIFIED 2026-07-22, live: gating on "the viewport spans the full backbuffer" does NOT
// identify UI in this engine. Outlast renders each eye's scene FULL-SIZE into an offscreen
// target and only places it into a half at composite time, so every post-process pass is
// full-width too. Mirroring them halved the frame once per pass and compounded: 2 copies, then
// 4, then 8 tiny scenes across the screen.
//
// The gate that actually separates them is the RENDER TARGET. The scene and its whole post
// chain run on offscreen targets; only the final composite and the UI are drawn onto the BACK
// BUFFER. Composite is opaque, UI is alpha-blended - so "render target is the backbuffer" AND
// "alpha blended" AND "depth off" isolates the UI without relying on viewport geometry at all.
// If Outlast turns out to draw its UI offscreen too, this gate simply never fires: the mirror
// no-ops and nothing else is harmed, which is the correct way for this to fail.
typedef HRESULT (STDMETHODCALLTYPE *SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
static SetRenderTarget_t o_SetRenderTarget = nullptr;
static volatile LONG g_rtIsBackbuffer = 1;   // frame starts on the backbuffer

// Also the only honest way to tell whether UE3's ScreenPercentage supersampling actually took:
// if it did, the scene render target is LARGER than the backbuffer. Reported once per new
// biggest size, so the log answers "is the game rendering more pixels than it presents?".
static LONG g_maxRtW = 0, g_maxRtH = 0;

static HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* This, DWORD idx, IDirect3DSurface9* surf) {
    HRESULT hr = o_SetRenderTarget(This, idx, surf);
    if (idx == 0) {
        LONG isBb = 0;
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(This->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            isBb = (bb == surf) ? 1 : 0;
            bb->Release();
        }
        InterlockedExchange(&g_rtIsBackbuffer, isBb);
        if (!isBb && surf && g_bbW > 0) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(surf->GetDesc(&d)) && (LONG)d.Width * (LONG)d.Height > g_maxRtW * g_maxRtH) {
                g_maxRtW = (LONG)d.Width; g_maxRtH = (LONG)d.Height;
                Log("[OLVR][SS] largest scene render target now %ux%u (backbuffer %ux%u) - "
                    "%s", d.Width, d.Height, g_bbW, g_bbH,
                    ((LONG)d.Width > (LONG)g_bbW) ? "SUPERSAMPLING is live"
                                                  : "at or below backbuffer size");
            }
        }
    }
    return hr;
}

static SetRenderState_t         o_SetRenderState = nullptr;
static DrawPrimitive_t          o_DrawPrimitive = nullptr;
static DrawIndexedPrimitive_t   o_DrawIndexedPrimitive = nullptr;
static DrawPrimitiveUP_t        o_DrawPrimitiveUP = nullptr;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP = nullptr;

// Mirror the three states classified here rather than calling GetRenderState per draw - there are
// thousands of draws a frame and each COM call would be pure overhead.
static volatile LONG g_rsZ = 1, g_rsBlend = 0, g_rsDest = 0;

static HRESULT STDMETHODCALLTYPE Hook_SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE s, DWORD v) {
    switch (s) {
        case D3DRS_ZENABLE:          InterlockedExchange(&g_rsZ, (LONG)v); break;
        case D3DRS_ALPHABLENDENABLE: InterlockedExchange(&g_rsBlend, (LONG)v); break;
        case D3DRS_DESTBLEND:        InterlockedExchange(&g_rsDest, (LONG)v); break;
        default: break;
    }
    return o_SetRenderState(This, s, v);
}

extern "C" int OLUE3_StereoMode();
extern "C" int OLUE3_SplitActive();
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy);

// The final scene composite is ALSO a full-screen alpha-blended depth-off quad on the backbuffer,
// so the state test alone cannot tell it from UI. Mirroring it squeezed the whole already-SBS
// frame into each half and put TWO views in every eye (live, 2026-07-22).
//
// What separates them is the SOURCE TEXTURE: the composite samples a backbuffer-sized scene
// texture, while HUD and menu elements sample comparatively small atlases. Anything sampling a
// near-fullscreen texture is scene work, not UI.
// Three-way, because "not a fullscreen texture" was never the same thing as "is UI".
//
// 2026-07-22, the washed-out regression: the old two-way test returned false for a draw with NO
// texture bound at all, so every untextured full-screen fill - Outlast's fades, darkening and
// vignette passes - was classified as UI and got relocated into the half-height box. The frame
// then lost most of its darkening, which is what "washed out" was.
//
// A HUD element is a TEXTURED quad (icon, glyph atlas, battery bar). A screen darkening pass is
// either untextured or samples a scene-sized target. So: no texture = not UI, scene-sized
// texture = not UI, everything else = UI candidate.
enum UiTexVerdict { UITEX_NONE = 0, UITEX_SMALL = 1, UITEX_FULLSCREEN = 2 };
static UiTexVerdict ClassifySourceTex(IDirect3DDevice9* dev, UINT* outW, UINT* outH) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    IDirect3DBaseTexture9* bt = nullptr;
    if (FAILED(dev->GetTexture(0, &bt)) || !bt) return UITEX_NONE;
    UiTexVerdict v = UITEX_NONE;
    if (bt->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(((IDirect3DTexture9*)bt)->GetLevelDesc(0, &d))) {
            if (outW) *outW = d.Width;
            if (outH) *outH = d.Height;
            v = ((d.Width * 10 >= g_bbW * 6) && (d.Height * 10 >= g_bbH * 6)) ? UITEX_FULLSCREEN
                                                                             : UITEX_SMALL;
        }
    }
    bt->Release();
    return v;
}

static bool UiDrawState() {
    return g_rtIsBackbuffer && g_rsZ == D3DZB_FALSE && g_rsBlend != 0 && g_rsDest == D3DBLEND_INVSRCALPHA;
}

extern "C" int  OLProxy_GetUiMirror() { return g_uiMirror; }
extern "C" void OLProxy_SetUiMirror(int v) {
    if (v < 0 || v > 2) v = UIMIRROR_ON;
    g_uiMirror = v;
    if (v != UIMIRROR_MENUS) { g_uiAutoArmed = 0; g_uiQuietFrames = 0; }
}

void OLProxy_ToggleUiMirror() {
    LONG m = (g_uiMirror + 1) % 3;
    g_uiMirror = m;
    if (m != UIMIRROR_MENUS) { g_uiAutoArmed = 0; g_uiQuietFrames = 0; }
    static const char* kWhat[3] = {
        "OFF - UI drawn once across the frame (the split chops it: half per eye)",
        "ON - mirror every TEXTURED small-source UI draw (HUD + menus in both eyes)",
        "MENUS-ONLY - fallback: mirror only during menu-volume bursts (loses the in-game HUD)"
    };
    Log("[OLVR][UIMIRROR] mode %ld = %s (candidates seen=%ld, mirrored=%ld)", m, kWhat[m], g_uiSeen, g_uiMirrored);
}

// ---- [HUDFIT] pull the HUD into the part of the frame a headset can actually see -------------
// Requirement: the camcorder HUD (battery, record status, etc.) must stay readable when the
// camcorder is active. The camcorder HUD (battery, record dot, timecode) is drawn at the FRAME EDGES, and a headset
// never sees the edges of the frame - the lens cuts them off. The mirror already relocates each
// UI draw into a per-eye box (that is how it gets UI into both eyes), so the box is exactly the
// right place to shrink and nudge: scaling it pulls every HUD corner inward into the readable
// area, and the offsets let a specific element be centred by eye.
// Uniform scale keeps the HUD's own proportions - the ME1 lesson is that stretching UI to "fit"
// is what makes it look wrong. Percent-of-box units so it survives resolution changes.
// Touches the UI viewport ONLY: no scene draw, no eye offset, no projection. The stereo depth
// this build is kept for cannot be affected by anything in here.
static volatile LONG g_hudScalePct = 82;    // 40..100 - 100 = the old edge-to-edge behaviour
static volatile LONG g_hudOffXPct  = 0;     // -40..+40, percent of the eye's half width
static volatile LONG g_hudOffYPct  = 0;     // -40..+40, percent of the box height (+ = down)

extern "C" void OLProxy_GetHudFit(float* scale, float* offX, float* offY) {
    if (scale) *scale = (float)g_hudScalePct * 0.01f;
    if (offX)  *offX  = (float)g_hudOffXPct  * 0.01f;
    if (offY)  *offY  = (float)g_hudOffYPct  * 0.01f;
}
extern "C" void OLProxy_SetHudFit(float scale, float offX, float offY) {
    LONG s = (LONG)(scale * 100.0f + 0.5f);   if (s < 40) s = 40;  if (s > 100) s = 100;
    LONG x = (LONG)(offX  * 100.0f + (offX  < 0 ? -0.5f : 0.5f));
    LONG y = (LONG)(offY  * 100.0f + (offY  < 0 ? -0.5f : 0.5f));
    if (x < -40) x = -40; if (x > 40) x = 40;
    if (y < -40) y = -40; if (y > 40) y = 40;
    InterlockedExchange(&g_hudScalePct, s);
    InterlockedExchange(&g_hudOffXPct,  x);
    InterlockedExchange(&g_hudOffYPct,  y);
}

template <class F>
static HRESULT MirrorUiDraw(IDirect3DDevice9* dev, F&& draw) {
    if (g_bbW < 8 || OLUE3_StereoMode() != 1 || !OLUE3_SplitActive() || !UiDrawState())
        return draw();

    UINT texW = 0, texH = 0;
    const UiTexVerdict tex = ClassifySourceTex(dev, &texW, &texH);
    if (tex == UITEX_NONE)       { InterlockedIncrement(&g_uiRejNoTex); return draw(); }  // fade/darken
    if (tex == UITEX_FULLSCREEN) { InterlockedIncrement(&g_uiRejFull);  return draw(); }  // composite/post

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return draw();
    LONG n = InterlockedIncrement(&g_uiSeen);
    // Count BEFORE deciding: mode 2's volume gate needs the signal even while it is disarmed.
    if (!UiMirrorActiveNow()) { InterlockedIncrement(&g_uiFrameHits); return draw(); }
    // PER-FRAME, reset at Present. The first version counted per SESSION and tripped at 4001,
    // which is simply a minute of ordinary UI drawing - it disarmed a mirror that was working.
    // A real cascade shows up as hundreds of catches in a SINGLE frame.
    LONG f = InterlockedIncrement(&g_uiFrameHits);
    if (f > 600) {
        InterlockedExchange(&g_uiMirror, UIMIRROR_OFF);
        InterlockedExchange(&g_uiAutoArmed, 0);
        Log("[OLVR][UIMIRROR] SAFETY OFF: %ld candidate draws in ONE frame - the classifier is "
            "catching non-UI draws. Mirror disarmed, everything else untouched.", f);
        return draw();
    }
    // Texture dimensions are in here because they are the classifier's whole basis now: if an
    // overlay ever slips through again, this line names it.
    if (n <= 12)
        Log("[OLVR][UIMIRROR] candidate #%ld tex=%ux%u vp=%ux%u@%u,%u bb=%ux%u zEnable=%ld blend=%ld dest=%ld",
            n, texW, texH, vp.Width, vp.Height, vp.X, vp.Y, g_bbW, g_bbH, g_rsZ, g_rsBlend, g_rsDest);

    float fx = 0.0f, fy = 0.0f;
    OLUE3_GetEyeFov(&fx, &fy);
    const UINT halfW = g_bbW / 2;
    UINT boxH = g_bbH;
    if (fx > 0.1f && fy > 0.1f) {
        const float uiAspect = (float)g_bbW / (float)g_bbH;
        float h = (float)g_bbH * tanf(fx * 0.5f) / (tanf(fy * 0.5f) * uiAspect);
        if (h < g_bbH * 0.2f) h = g_bbH * 0.2f;
        if (h > (float)g_bbH)  h = (float)g_bbH;
        boxH = (UINT)h;
    }
    const UINT boxY = vp.Y + ((g_bbH > boxH) ? (g_bbH - boxH) / 2 : 0);

    // [HUDFIT] shrink the box and nudge it, so edge-drawn HUD lands where the lens can see it.
    const float hs = (float)g_hudScalePct * 0.01f;
    UINT drawW = (UINT)((float)halfW * hs); if (drawW < 16) drawW = 16;
    UINT drawH = (UINT)((float)boxH  * hs); if (drawH < 16) drawH = 16;
    const int padX = ((int)halfW - (int)drawW) / 2 + (int)((float)halfW * (float)g_hudOffXPct * 0.01f);
    const int padY = ((int)boxH  - (int)drawH) / 2 + (int)((float)boxH  * (float)g_hudOffYPct * 0.01f);

    // Clamp into the backbuffer vertically, and into THIS EYE'S OWN HALF horizontally - a
    // viewport outside the render target is a silent no-draw, and one that crosses the seam
    // would put this eye's HUD into the other eye's picture.
    int yy = (int)boxY + padY;
    if (yy < 0) yy = 0;
    if (yy + (int)drawH > (int)g_bbH) yy = (int)g_bbH - (int)drawH;

    D3DVIEWPORT9 half = vp;
    half.Y = (UINT)yy; half.Height = drawH; half.Width = drawW;

    HRESULT hr = D3D_OK;
    for (int e = 0; e < 2; ++e) {
        const int lo = e ? (int)halfW : 0;
        int xx = lo + padX;
        if (xx < lo) xx = lo;
        if (xx + (int)drawW > lo + (int)halfW) xx = lo + (int)halfW - (int)drawW;
        half.X = (UINT)xx;
        dev->SetViewport(&half);
        const HRESULT h = draw();
        if (e == 0) hr = h;
    }
    dev->SetViewport(&vp);
    InterlockedIncrement(&g_uiMirrored);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE t, UINT s, UINT c) {
    return MirrorUiDraw(This, [&] { return o_DrawPrimitive(This, t, s, c); });
}
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE t,
        INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    return MirrorUiDraw(This, [&] { return o_DrawIndexedPrimitive(This, t, bv, mi, nv, si, pc); });
}
static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(IDirect3DDevice9* This, D3DPRIMITIVETYPE t,
        UINT pc, const void* d, UINT stride) {
    return MirrorUiDraw(This, [&] { return o_DrawPrimitiveUP(This, t, pc, d, stride); });
}
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* This, D3DPRIMITIVETYPE t,
        UINT mvi, UINT nv, UINT pc, const void* id, D3DFORMAT idf, const void* vd, UINT stride) {
    return MirrorUiDraw(This, [&] { return o_DrawIndexedPrimitiveUP(This, t, mvi, nv, pc, id, idf, vd, stride); });
}

static void* HookSlot(void** vtbl, int index, void* hook) {
    DWORD prot = 0;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot);
    void* orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), prot, &prot);
    return orig;
}

static void RunWindowFixIfDue();       // [SCREQ] window lever, defined with Hook_Reset below
static void RunFullscreenFixIfDue();   // [FSFIX] fullscreen re-assert, defined with Hook_Reset below

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* This, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    LONG n = InterlockedIncrement(&g_presentCount);
    if (n == 1)   Log("[OLVR] first Present - frame loop is live");
    if (n == 120) Log("[OLVR] 120 presents - hook stable");
    g_uiFrameHitsLast = InterlockedExchange(&g_uiFrameHits, 0);   // per-frame UI catch count
    UiMirrorAutoTick(g_uiFrameHitsLast);
    RunWindowFixIfDue();
    RunFullscreenFixIfDue();
    // FIRST RUN: open the menu once, by itself, so the player discovers it exists. Waiting a few
    // hundred frames keeps it out of the startup logos and lands it on the title screen, where
    // the game is drawing normally and pausing costs nothing.
    if (g_firstRunPending && n >= kFirstRunOpenAtPresent) {
        g_firstRunPending = 0;
        if (!OLMenu_Visible()) { OLMenu_Toggle(); Log("[OLVR][MENU] opened for the first run"); }
    }
    if ((n % 600) == 0)
        Log("[OLVR][UIMIRROR] alive: %ld candidates last frame, %ld mirrored total, passed through: "
            "%ld untextured (fades/darkening) + %ld scene-sized (mode=%ld armed=%ld)",
            g_uiFrameHitsLast, g_uiMirrored, g_uiRejNoTex, g_uiRejFull, g_uiMirror, g_uiAutoArmed);
    // The menu is drawn into the BACKBUFFER before the XR capture reads it, so it rides along
    // into the headset with the rest of the frame - no second layer, no separate pose, and it
    // lands in both eyes because it draws itself into both halves.
    __try { OLMenu_Render(This, g_bbW, g_bbH); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[OLVR][MENU] EXCEPTION while drawing - menu skipped"); }
    if (g_xrAllowed && g_xrEnabled) {
        __try { OLXR_OnPresent(This); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&g_xrEnabled, 0);
            Log("[OLVR] EXCEPTION in XR submit - XR disabled, game continues flat");
        }
    }
    return o_Present(This, a, b, c, d);
}

// [SCREQ] the WINDOW lever. Live evidence 2026-07-22: the post-intro windowed size tracks the
// window's real client rect - GetMonitorInfo was spoofed AND GetAdapterDisplayMode never called
// (spoofs=0), yet the game asked exactly its monitor's 2560x1440. Windows itself clamps the
// window; the engine just measures it. So change what it measures: after the game's own windowed
// Reset comes up short, resize its window to the requested client size (overhanging the desktop
// is fine - the headset consumes the backbuffer, not the screen). If UE3 follows its client rect
// it re-Resets ITSELF to the request and the buffer stays honest; if it doesn't, nothing breaks.
// Two attempts max so a game that fights back doesn't cause a resize war.
static HWND g_gameWnd = nullptr;
static LONG g_winFixAtPresent = 0;     // present # to fire at (0 = disarmed)
static LONG g_winFixTries = 0;
static void ArmWindowFix() {
    if (!g_gameWnd || g_winFixTries >= 2) return;
    g_winFixAtPresent = g_presentCount + 30;   // let the Reset settle first
}
static void RunWindowFixIfDue() {
    if (!g_winFixAtPresent || g_presentCount < g_winFixAtPresent) return;
    g_winFixAtPresent = 0;
    ++g_winFixTries;
    RECT cr = { 0, 0, g_reqW, g_reqH };
    const LONG_PTR style   = GetWindowLongPtrW(g_gameWnd, GWL_STYLE);
    const LONG_PTR exstyle = GetWindowLongPtrW(g_gameWnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&cr, (DWORD)style & ~WS_OVERLAPPED, FALSE, (DWORD)exstyle);
    // Primary monitor origin is (0,0) by definition; the 4K display is the primary here.
    SetWindowPos(g_gameWnd, nullptr, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Log("[OLVR][SCREQ] window fix #%ld: moved game window to primary at client %ldx%ld - "
        "if the engine follows its client rect, the NEXT Reset line is the verdict",
        g_winFixTries, g_reqW, g_reqH);
}

// [FSFIX] re-assert exclusive fullscreen 4K after the VR compositor knocks the game windowed.
// THE FINDING (2026-07-23, from the SCRDIAG log): the game boots 3840x2160 EXCLUSIVE FULLSCREEN
// and that is TRUE 4K in the headset (eye 1728x2160, confirmed). When the XR session starts the
// game loses exclusive fullscreen (focus/display grab) and UE3 falls back to WINDOWED, where the
// backbuffer is DPI/work-area-capped at 3633x2044 no matter what the window is set to.
// Re-taking fullscreen from the video menu restores true 4K and it STICKS - so the fix is to
// make the game re-run ITS OWN fullscreen mode change once the compositor has settled.
//
// The trigger is ALT+ENTER: UE3's viewport window proc toggles fullscreen on WM_SYSKEYDOWN
// VK_RETURN with the ALT context bit. That runs the ENGINE's mode change, so the scene resizes
// to the fullscreen resolution - NO forced backbuffer, so none of the cross-eyed 4.11 risk. It
// is exactly what the video menu does. If UE3 does not handle it, nothing happens (safe).
static volatile LONG g_isWindowed   = 0;
static volatile LONG g_autoFs       = 1;     // outlastvr.ini [Render] AutoFullscreen (default on)
static LONG g_fsFixAtPresent = 0;            // present # to attempt at (0 = disarmed)
static LONG g_fsFixCount     = 0;            // attempts since the last windowed drop

static void SendFullscreenToggle(const char* why) {
    if (!g_gameWnd) { Log("[OLVR][FSFIX] no game window yet - cannot toggle"); return; }
    // WM_SYSKEYDOWN/UP for VK_RETURN. lParam bit 29 = ALT context, bits 16-23 = Enter scan 0x1C.
    const LPARAM downL = (LPARAM)(0x00000001u | (0x1Cu << 16) | (1u << 29));
    const LPARAM upL   = (LPARAM)(0x00000001u | (0x1Cu << 16) | (1u << 29) | (1u << 30) | (1u << 31));
    PostMessageW(g_gameWnd, WM_SYSKEYDOWN, VK_RETURN, downL);
    PostMessageW(g_gameWnd, WM_SYSKEYUP,   VK_RETURN, upL);
    ++g_fsFixCount;
    Log("[OLVR][FSFIX] ALT+ENTER -> game (%s) attempt #%ld - the win is a 'Reset -> 3840x2160 windowed=0' line next",
        why, g_fsFixCount);
}

static void RunFullscreenFixIfDue() {
    if (!g_autoFs || !g_fsFixAtPresent || g_presentCount < g_fsFixAtPresent) return;
    g_fsFixAtPresent = 0;
    if (!g_isWindowed) return;             // already fullscreen: nothing to do
    if (g_fsFixCount >= 3) {               // 3 tries is enough; leave the manual chord as backup
        Log("[OLVR][FSFIX] gave up after 3 auto attempts - the picture will be softer than it could be");
        return;
    }
    SendFullscreenToggle("auto, windowed after the compositor settled");
    g_fsFixAtPresent = g_presentCount + 600;   // still windowed a few seconds later? try once more
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pp) {
    if (pp) {
        Log("[OLVR] Reset -> %ux%u windowed=%d (monitor-rect spoofs=%ld, display-mode spoofs=%ld)",
            pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->Windowed, g_dispqHits, g_admHits);
        // The 2026-07-22 build FORCED pp up to the ini request here. That was wrong by
        // construction: the engine sizes its scene viewport from the resolution IT asked for, so
        // it rendered 2560x1440 into the corner of the forced 3840x2160 buffer and the XR
        // half-split cut through the middle of the right eye's view - cross-eyed garbage in the
        // headset. The buffer must always equal what the engine believes; to get a BIGGER buffer,
        // make the ENGINE ask bigger (the GetAdapterDisplayMode spoof) - never resize it behind
        // the engine's back.
        g_isWindowed = pp->Windowed ? 1 : 0;
        if (pp->hDeviceWindow) g_gameWnd = pp->hDeviceWindow;

        if (!pp->Windowed) {
            // Back in exclusive fullscreen (either the boot mode or the re-assert took): disarm
            // the fullscreen fix and reset its budget for the next time the compositor drops it.
            g_fsFixAtPresent = 0; g_fsFixCount = 0;
        } else if (g_autoFs) {
            // Windowed = the compositor knocked the game out of fullscreen. Re-assert it once
            // the compositor has settled (~a few seconds of presents). The window lever is the
            // OLD strategy and only ever reached 3633x2044; fullscreen is strictly better, so
            // the window is NOT also fought when AutoFullscreen is on.
            if (!g_fsFixAtPresent && g_fsFixCount < 3)
                g_fsFixAtPresent = g_presentCount + 300;
            Log("[OLVR][SCREQ] game went windowed %ux%u - AutoFullscreen armed (will re-assert 4K fullscreen)",
                pp->BackBufferWidth, pp->BackBufferHeight);
        } else if (g_spoofArmed &&
                   ((LONG)pp->BackBufferWidth < g_reqW || (LONG)pp->BackBufferHeight < g_reqH)) {
            Log("[OLVR][SCREQ] game asked %ux%u (< request %ldx%ld) - staying coherent at the "
                "game's size and trying the window lever",
                pp->BackBufferWidth, pp->BackBufferHeight, g_reqW, g_reqH);
            ArmWindowFix();
        }
        g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
    }
    if (pp && g_xrAllowed && !g_keepVsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        Log("[OLVR][PACING] Reset PresentationInterval 0x%08lX -> IMMEDIATE", pp->PresentationInterval);
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (g_leftRT) { g_leftRT->Release(); g_leftRT = nullptr; g_leftW = g_leftH = 0; }  // DEFAULT pool dies on Reset
    OLMenu_OnDeviceLost();   // the menu's ImGui/RT objects are DEFAULT pool too - drop before Reset
    HRESULT hr = o_Reset(This, pp);
    if (SUCCEEDED(hr)) OLMenu_OnDeviceReset();
    return hr;
}

// ---- [GAMMA] SetGammaRamp capture ----------------------------------------------------------
// Outlast's brightness calibration ("adjust until the logo is barely visible") works through
// IDirect3DDevice9::SetGammaRamp, which reprograms the MONITOR's output curve. This capture
// reads the backbuffer BEFORE that curve is applied, so the headset never saw that
// brightness calibration - one of the two halves of "the colors are weird / blacks are not good"
// (the other is the sRGB-vs-2.2 decode mismatch, handled by the gamma LUT in ol_xr.cpp).
// Capture the ramp here; ol_xr.cpp folds it into its blit LUT so headset == calibrated TV.
typedef void (STDMETHODCALLTYPE *SetGammaRamp_t)(IDirect3DDevice9*, UINT, DWORD, const D3DGAMMARAMP*);
static SetGammaRamp_t o_SetGammaRamp = nullptr;
static D3DGAMMARAMP  g_gammaRamp = {};
static volatile long g_gammaRampGen = 0;    // 0 = none/identity; bumps on every non-identity set

extern "C" int OLProxy_GammaRampGen() { return g_gammaRampGen; }
extern "C" int OLProxy_GetGammaRamp(unsigned short* r, unsigned short* g, unsigned short* b) {
    if (!g_gammaRampGen || !r || !g || !b) return 0;
    memcpy(r, g_gammaRamp.red,   256 * sizeof(unsigned short));
    memcpy(g, g_gammaRamp.green, 256 * sizeof(unsigned short));
    memcpy(b, g_gammaRamp.blue,  256 * sizeof(unsigned short));
    return g_gammaRampGen;
}

static void STDMETHODCALLTYPE Hook_SetGammaRamp(IDirect3DDevice9* This, UINT sc, DWORD flags, const D3DGAMMARAMP* ramp) {
    if (ramp) {
        // Identity = value[i] == i*257 (0..65535 over 256 entries). Tolerate rounding slop.
        bool identity = true;
        for (int i = 0; i < 256 && identity; ++i) {
            const int want = i * 257;
            if (abs((int)ramp->red[i]   - want) > 384 ||
                abs((int)ramp->green[i] - want) > 384 ||
                abs((int)ramp->blue[i]  - want) > 384) identity = false;
        }
        if (!identity) {
            memcpy(&g_gammaRamp, ramp, sizeof(g_gammaRamp));
            LONG gen = InterlockedIncrement(&g_gammaRampGen);
            if (gen <= 3)
                Log("[OLVR][GAMMA] game gamma ramp captured (gen=%ld, mid red=%u) - folded into the headset blit",
                    gen, (unsigned)ramp->red[128]);
        } else if (g_gammaRampGen) {
            InterlockedExchange(&g_gammaRampGen, 0);   // game reset to identity
            Log("[OLVR][GAMMA] game gamma ramp reset to identity");
        }
    }
    if (o_SetGammaRamp) o_SetGammaRamp(This, sc, flags, ramp);
}

// ---- hardware occlusion queries -------------------------------------------------------
// Third hypothesis in the same-frame flicker investigation (tracker: never called; precomputed visibility:
// bypassed live, no change). The remaining per-object visibility machine is D3D9 hardware
// occlusion queries: the engine draws bounding boxes, asks the GPU how many pixels passed,
// and culls meshes whose LAST answer was 0. The query handles come from a pool shared per
// scene and recycled per frame - with TWO views per frame the second view can fetch results
// that belong to other queries, so its verdicts are garbage and props strobe. One view per
// frame (AER) never contends the pool, which matches AER being clean.
//
// This module IS d3d9.dll, so no exe fingerprint is needed: hook CreateQuery (device vtable 118),
// then GetData (query vtable 7 - one vtable shared by every query). While the bypass
// is ON every occlusion result becomes "lots of pixels visible" and NOTHING is ever culled
// by a query, corrupt or not. Costs the culling perf; answers the question.
extern "C" int OLUE3_OcclBypassOn();   // the OcclForce flag, lives in ol_ue3.cpp
static CreateQuery_t  o_CreateQuery = nullptr;
static QueryGetData_t o_QueryGetData = nullptr;
static volatile LONG g_queryVtblHooked = 0;
static LONG g_hwqCreated = 0, g_hwqForced = 0;
extern "C" void OLProxy_HwqStats(long* created, long* forced) {
    if (created) *created = g_hwqCreated;
    if (forced)  *forced = g_hwqForced;
}

static HRESULT STDMETHODCALLTYPE Hook_QueryGetData(IDirect3DQuery9* This, void* pData, DWORD size, DWORD flags) {
    HRESULT hr = o_QueryGetData(This, pData, size, flags);
    if (hr == S_OK && pData && size >= sizeof(DWORD) && OLUE3_OcclBypassOn() &&
        This->GetType() == D3DQUERYTYPE_OCCLUSION) {
        *(DWORD*)pData = 0x7FFFFF;          // "many pixels" -> the engine never culls on it
        InterlockedIncrement(&g_hwqForced);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateQuery(IDirect3DDevice9* This, D3DQUERYTYPE type, IDirect3DQuery9** ppQ) {
    HRESULT hr = o_CreateQuery(This, type, ppQ);
    if (SUCCEEDED(hr) && ppQ && *ppQ && type == D3DQUERYTYPE_OCCLUSION) {
        LONG n = InterlockedIncrement(&g_hwqCreated);
        if (InterlockedCompareExchange(&g_queryVtblHooked, 1, 0) == 0) {
            void** qvt = *(void***)*ppQ;    // shared by all IDirect3DQuery9 instances
            o_QueryGetData = (QueryGetData_t)HookSlot(qvt, 7, (void*)&Hook_QueryGetData);
            Log("[OLVR][HWQ] occlusion-query GetData hooked via first query %p", (void*)*ppQ);
        }
        if (n == 1) Log("[OLVR][HWQ] first occlusion query created - hardware occlusion is in use");
    }
    return hr;
}

static void InstallDeviceHooks(IDirect3DDevice9* dev) {
    if (InterlockedCompareExchange(&g_devHookDone, 1, 0) != 0) return;
    void** vt = *(void***)dev;
    g_dev = dev;
    o_Reset   = (Reset_t)  HookSlot(vt, 16, (void*)&Hook_Reset);
    o_Present = (Present_t)HookSlot(vt, 17, (void*)&Hook_Present);
    o_SetGammaRamp = (SetGammaRamp_t)HookSlot(vt, 21, (void*)&Hook_SetGammaRamp);   // [GAMMA]
    o_CreateQuery = (CreateQuery_t)HookSlot(vt, 118, (void*)&Hook_CreateQuery);
    // [UIMIRROR]: 57 = SetRenderState (state cache), 81..84 = the four draw entry points.
    o_SetRenderTarget        = (SetRenderTarget_t)       HookSlot(vt, 37, (void*)&Hook_SetRenderTarget);
    o_SetRenderState         = (SetRenderState_t)        HookSlot(vt, 57, (void*)&Hook_SetRenderState);
    o_DrawPrimitive          = (DrawPrimitive_t)         HookSlot(vt, 81, (void*)&Hook_DrawPrimitive);
    o_DrawIndexedPrimitive   = (DrawIndexedPrimitive_t)  HookSlot(vt, 82, (void*)&Hook_DrawIndexedPrimitive);
    o_DrawPrimitiveUP        = (DrawPrimitiveUP_t)       HookSlot(vt, 83, (void*)&Hook_DrawPrimitiveUP);
    o_DrawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)HookSlot(vt, 84, (void*)&Hook_DrawIndexedPrimitiveUP);
    // The camera census that used to hook SetVertexShaderConstantF (vtable 94) is NOT installed
    // any more. It was a research probe from the days of hunting the camera through the renderer,
    // the engine-side route won instead, and it never surfaced anything useful after that. It
    // was also the single most expensive thing in this dll: the game pushes about 14,000 vertex
    // constants per frame and every one of them was going through this hook to be
    // pattern-matched. Removing
    // it costs nothing and gives those cycles back.
    Log("[OLVR] device hooks installed dev=%p (CreateQuery 118, SetRenderState 57, draws 81-84)", (void*)dev);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocus,
                                                   DWORD Behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDev) {
    if (pp && g_xrAllowed && !g_keepVsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        Log("[OLVR][PACING] CreateDevice PresentationInterval 0x%08lX -> IMMEDIATE", pp->PresentationInterval);
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    HRESULT hr = o_CreateDevice(This, Adapter, DeviceType, hFocus, Behavior, pp, ppDev);
    if (hFocus) g_gameWnd = hFocus;
    if (pp) {
        Log("[OLVR] CreateDevice %ux%u bbFmt=%d windowed=%d behavior=0x%08lX hr=0x%08lX",
            pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat, (int)pp->Windowed, Behavior, hr);
        g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
        if (g_spoofArmed)
            Log("[OLVR][SCREQ] asked %ldx%ld, game took %ux%u (monitor queries spoofed=%ld)",
                g_reqW, g_reqH, g_bbW, g_bbH, g_dispqHits);
    }
    if (SUCCEEDED(hr) && ppDev && *ppDev) InstallDeviceHooks(*ppDev);
    return hr;
}

static void InstallD3D9Hook(IDirect3D9* d3d) {
    if (InterlockedCompareExchange(&g_d3d9HookDone, 1, 0) != 0) return;
    void** vt = *(void***)d3d;
    o_CreateDevice = (CreateDevice_t)HookSlot(vt, 16, (void*)&Hook_CreateDevice);
    o_GetAdapterDisplayMode = (GetAdapterDisplayMode_t)HookSlot(vt, 8, (void*)&Hook_GetAdapterDisplayMode);
    Log("[OLVR] IDirect3D9 CreateDevice(16) + GetAdapterDisplayMode(8) hooks installed d3d9=%p", (void*)d3d);
}

// ---------------------------------------------------------------------------
// exports (the 4 names OLGame.exe imports)
// ---------------------------------------------------------------------------
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    EnsureRealD3D9();
    Direct3DCreate9_t real = g_realD3D9 ? (Direct3DCreate9_t)GetProcAddress(g_realD3D9, "Direct3DCreate9") : nullptr;
    if (!real) { Log("[OLVR] ERROR real Direct3DCreate9 missing"); return nullptr; }
    IDirect3D9* d3d = real(SDKVersion);
    Log("[OLVR] Direct3DCreate9 sdk=%u real=%p", SDKVersion, (void*)d3d);
    if (d3d) InstallD3D9Hook(d3d);
    return d3d;
}
typedef int  (WINAPI *D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int  (WINAPI *D3DPERF_EndEvent_t)(void);
typedef void (WINAPI *D3DPERF_SetOptions_t)(DWORD);
extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) { EnsureRealD3D9();
    static D3DPERF_BeginEvent_t r = g_realD3D9 ? (D3DPERF_BeginEvent_t)GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent") : nullptr; return r ? r(col, name) : 0; }
extern "C" int WINAPI D3DPERF_EndEvent(void) { EnsureRealD3D9();
    static D3DPERF_EndEvent_t r = g_realD3D9 ? (D3DPERF_EndEvent_t)GetProcAddress(g_realD3D9, "D3DPERF_EndEvent") : nullptr; return r ? r() : 0; }
extern "C" void WINAPI D3DPERF_SetOptions(DWORD opt) { EnsureRealD3D9();
    static D3DPERF_SetOptions_t r = g_realD3D9 ? (D3DPERF_SetOptions_t)GetProcAddress(g_realD3D9, "D3DPERF_SetOptions") : nullptr; if (r) r(opt); }

// ---------------------------------------------------------------------------
// key poller + boot
// The MENU keys are bare, unlike every probe hotkey, and that is a deliberate exception. INSERT
// is not bound by Outlast, and the arrows only do anything while the menu is open - at which
// point you are standing still reading it. Nothing else is bound at all, because a key
// the game also handles makes its own test unreadable (two runs were lost learning that).
static bool Bare(int vk, bool& prev) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fired = down && !prev;
    prev = down;
    return fired;
}

// ---- [RECENTER] one key, and a controller shortcut, both rebindable ---------------------------
// Recentering is the one thing a player needs mid-game without opening a menu: sit differently,
// drift, press it, carry on. So it gets a bare key of its own.
//
// The default is G, chosen after reading Outlast's OWN bindings out of OLInput.ini and
// DefaultInput.ini rather than guessing: W A S D, Q, E, F, R, J, N, Tab, Space, Escape, Ctrl,
// Shift, the arrows and the mouse are all taken. G is free and sits beside F (night vision),
// so the hand is already there. R was the first choice and was WRONG: R is OLA_Reload, the
// camcorder battery. The keypress is never swallowed, so any key the game also uses would fire
// both actions, and burning a battery to recentre is not a trade worth making.
//
// The controller shortcut is a DOUBLE press of R3, and both halves of that were chosen against
// the game's real bindings rather than by taste.
// DOUBLE, because a single press is too easy to hit by accident and having your view yanked
// straight while something is chasing you is the worst possible moment for it.
// R3, because every pad button in this game is bound to something and R3 is the safest of them:
//   * it is OLA_ToggleNightVision, a TOGGLE, so two presses land back in the state you started
//     in, and with the camcorder down it does nothing at all
//   * nobody presses it twice quickly in normal play
// L1 was tried first and is WORSE despite looking harmless: it is OLA_Run, the button players
// mash and re-grip during every chase, which is precisely the accidental double tap this design
// is trying to avoid. L3 is bound to ToggleDebugCamera and was rejected outright - if that is
// live in the shipping build it detaches the camera, which is far worse than a night-vision
// blink. Both bindings are rebindable from the menu and saved to the ini.
static volatile LONG g_recenterVk  = 'G';                 // 0 = no key bound
static volatile LONG g_recenterPad = 0x0100;              // XINPUT_GAMEPAD_LEFT_SHOULDER (L1)
// The menu key itself is rebindable too. INSERT is a fine default (Outlast does not use it) but
// it is missing or awkward on plenty of compact and laptop keyboards, and a settings menu you
// cannot open is the one binding with no way to fix itself from inside the game.
static volatile LONG g_menuVk      = VK_INSERT;
static volatile LONG g_bindCapture = 0;                   // 0 idle, 1 key, 2 pad, 3 menu key
static const DWORD kPadDoubleMs = 400;

extern "C" int  OLProxy_GetRecenterKey() { return (int)g_recenterVk; }
extern "C" int  OLProxy_GetRecenterPad() { return (int)g_recenterPad; }
extern "C" int  OLProxy_GetMenuKey()     { return (int)g_menuVk; }
extern "C" int  OLProxy_GetBindCapture() { return (int)g_bindCapture; }
extern "C" void OLProxy_StartBindCapture(int which) { InterlockedExchange(&g_bindCapture, which); }

// Human names, because "VK 0x52" is not something to show a player.
extern "C" const char* OLProxy_KeyName(int vk) {
    static char buf[64];
    if (vk == 0) { strcpy_s(buf, "none"); return buf; }
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    switch (vk) {
        case VK_INSERT: strcpy_s(buf, "Insert");    return buf;   // the menu default: name it
        case VK_SPACE:  strcpy_s(buf, "Space");     return buf;
        case VK_TAB:    strcpy_s(buf, "Tab");       return buf;
        case VK_BACK:   strcpy_s(buf, "Backspace"); return buf;
        case VK_HOME:   strcpy_s(buf, "Home");      return buf;
        case VK_END:    strcpy_s(buf, "End");       return buf;
        case VK_DELETE: strcpy_s(buf, "Delete");    return buf;
        case VK_PRIOR:  strcpy_s(buf, "Page Up");   return buf;
        case VK_NEXT:   strcpy_s(buf, "Page Down"); return buf;
        case VK_OEM_3:  strcpy_s(buf, "`");         return buf;
        case VK_OEM_MINUS:  strcpy_s(buf, "-");     return buf;
        case VK_OEM_PLUS:   strcpy_s(buf, "=");     return buf;
        case VK_OEM_4:  strcpy_s(buf, "[");         return buf;
        case VK_OEM_6:  strcpy_s(buf, "]");         return buf;
        case VK_OEM_1:  strcpy_s(buf, ";");         return buf;
        case VK_OEM_7:  strcpy_s(buf, "'");         return buf;
        case VK_OEM_COMMA:  strcpy_s(buf, ",");     return buf;
        case VK_OEM_PERIOD: strcpy_s(buf, ".");     return buf;
        case VK_OEM_2:  strcpy_s(buf, "/");         return buf;
        case VK_OEM_5:  strcpy_s(buf, "\\");        return buf;
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { sprintf_s(buf, "Numpad %d", vk - VK_NUMPAD0); return buf; }
    sprintf_s(buf, "key %d", vk);
    return buf;
}

extern "C" const char* OLProxy_PadName(int mask) {
    switch (mask) {
        case 0x0020: return "Back / Select";
        case 0x0010: return "Start";
        case 0x0040: return "L3 (left stick click)";
        case 0x0080: return "R3 (right stick click)";
        case 0x0100: return "L1 (left bumper)";
        case 0x0200: return "R1 (right bumper)";
        case 0x1000: return "A";
        case 0x2000: return "B";
        case 0x4000: return "X";
        case 0x8000: return "Y";
        case 0x0001: return "D-pad up";
        case 0x0002: return "D-pad down";
        case 0x0004: return "D-pad left";
        case 0x0008: return "D-pad right";
        case 0:      return "none";
        default:     return "button";
    }
}

// XInput, loaded the same lazy way the menu does it. Absent controller = every read is zero,
// so the whole controller path simply never fires.
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void*);
static XInputGetState_t g_padGet = nullptr;
static bool g_padTried = false;

static WORD PadButtons() {
    if (!g_padTried) {
        g_padTried = true;
        const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !g_padGet; ++i) {
            HMODULE m = LoadLibraryW(dlls[i]);
            if (m) g_padGet = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
        }
        Log("[OLVR][RECENTER] controller support %s", g_padGet ? "ready" : "unavailable (no XInput)");
    }
    if (!g_padGet) return 0;
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st = {};
    if (g_padGet(0, &st) != ERROR_SUCCESS) return 0;
    return st.buttons;
}

// ---- [PADSTUB] motion controls, step 1: can a FABRICATED pad drive this game? ------------------
// The whole motion-control plan rests on one unproven assumption - that this hook can BE the controller,
// not merely add to one. The lean build only ever merged into a pad that was already plugged in
// (and it worked, so the hook and the game's reading of merged values are already proven). The
// open question is the NO-PAD case: UE3's WinDrv polls XInputGetState per tick, and some builds
// stop polling a slot that answered ERROR_DEVICE_NOT_CONNECTED at startup. If this game does
// that, no synthetic pad can ever reach it and the OpenXR action set would be a week spent on a
// dead end. One afternoon here answers it.
//
// Two measurements, both from this one hook:
//   1. DOES IT EVEN ASK? the poll counters below, reported every ~3 s from KeyThread (an
//      independent timer - counting from inside the hook could not distinguish "polling stopped"
//      from "never started"). Slot 0 and all-slots are counted separately, so "polls other slots
//      but not 0" is distinguishable from silence.
//   2. DOES IT OBEY? with [Stage3] PadStub=1 and no physical pad, answer ERROR_SUCCESS with a
//      stick that walks forward 1.5 s then back 1.5 s, forever. Miles pacing on his own with
//      nothing plugged in is the proof.
// The stick OSCILLATES rather than holding forward, deliberately: a constant walk is
// indistinguishable from a stuck analog stick or a slope, and it would walk him off a ledge while
// you watch. Alternating is unmistakably this hook's doing and returns Miles to roughly where he started.
// A real connected pad ALWAYS wins - the stub only fills a slot the runtime says is empty - so
// this can never fight a controller you are holding.
struct OLXiGamepad { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                     SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct OLXiState   { DWORD dwPacketNumber; OLXiGamepad Gamepad; };
typedef DWORD (WINAPI *XiGetState_t)(DWORD, OLXiState*);
static XiGetState_t  o_XiGetState = nullptr;
static volatile long g_padStub     = 0;   // [Stage3] PadStub, dev only, default off
static volatile long g_padPolls0   = 0;   // slot-0 polls since the last report
static volatile long g_padPollsAll = 0;   // polls of ANY slot since the last report
static volatile long g_padRealOk   = 0;   // did the real device answer ERROR_SUCCESS last time
static volatile long g_padFaked    = 0;   // frames answered for an empty slot

// [XRINPUT] THE MAP. One table, one place - the whole point of publishing a fabricated bitmask from
// ol_xr.cpp instead of XInput's. Chosen against Outlast's own bindings (OLInput.ini):
//   A = OLA_Jump      B = OLA_CrouchToggle   X = OLA_Use        Y = OLA_Reload (camcorder battery)
//   LB = OLA_Run      RB = OLA_ToggleCamcorder                  R3 = OLA_ToggleNightVision
// So on Touch: right A/B are jump/crouch, left X/Y are use/battery, grips are run (the natural
// "hold to sprint"), right stick click is night vision, and the triggers keep the game's own
// analog lean. Menu opens the pause menu (Start).
static WORD MapPadButtons(const OLXrPad& p) {
    WORD b = 0;
    if (p.buttons & OLXR_BTN_A) b |= 0x1000;              // A  = OLA_Jump
    if (p.buttons & OLXR_BTN_B) b |= 0x2000;              // B  = OLA_CrouchToggle
    if (p.buttons & OLXR_BTN_X) b |= 0x4000;              // X  = OLA_Use
    if (p.buttons & OLXR_BTN_Y) b |= 0x8000;              // Y  = OLA_Reload (camcorder battery)
    if (p.buttons & OLXR_BTN_RSTICK) b |= 0x0080;         // R3 = OLA_ToggleNightVision
    if (p.buttons & OLXR_BTN_LSTICK) b |= 0x0040;         // L3 (unbound in game; harmless)
    if (p.buttons & OLXR_BTN_MENU)   b |= 0x0010;         // START = pause menu
    if (p.gripL > 0.6f) b |= 0x0100;                      // LB = OLA_Run (hold to sprint)
    // RIGHT GRIP is handled by the caller, not here: it is a HOLD that has to send the game's
    // toggle exactly once on press and once on release ([HANDCAM]), which needs edge state.
    return b;
}

// [HANDCAM] Right grip = hold the camcorder up. The game's binding is a TOGGLE (RB =
// OLA_ToggleCamcorder, and UE3 fires bindings on press), so a hold is built from two edges: send
// RB once when the grip closes, once more when it opens. In between, the camera belongs to the
// hand. Hysteresis on the threshold so a grip resting near the trip point cannot chatter the
// camcorder on and off.
static bool  g_gripHeld = false;
static DWORD g_gripEdgeUntil = 0;   // keep RB pressed briefly so the game cannot miss the edge
extern "C" void OLXR_SetHandCamActive(int on);
static WORD CamcorderGripButton(const OLXrPad& p) {
    const bool was = g_gripHeld;
    if (!g_gripHeld && p.gripR > 0.65f) g_gripHeld = true;
    else if (g_gripHeld && p.gripR < 0.45f) g_gripHeld = false;
    if (was != g_gripHeld) {
        // An edge either way toggles the game's camcorder. Held for ~80 ms: the game samples
        // input once per frame at ~60 fps, and a single-frame press can land between samples.
        g_gripEdgeUntil = GetTickCount() + 80;
        OLXR_SetHandCamActive(g_gripHeld ? 1 : 0);
    }
    return (GetTickCount() < g_gripEdgeUntil) ? 0x0200 : 0;   // RIGHT_SHOULDER
}
static SHORT StickAxis(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    const float s = v * 32767.0f;
    return (SHORT)(s > 32767.0f ? 32767 : (s < -32768.0f ? -32768 : s));
}

static DWORD WINAPI Hook_XiGetState(DWORD idx, OLXiState* st) {
    InterlockedIncrement(&g_padPollsAll);
    const DWORD r = o_XiGetState ? o_XiGetState(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (idx != 0 || !st) return r;
    InterlockedIncrement(&g_padPolls0);
    InterlockedExchange(&g_padRealOk, r == ERROR_SUCCESS ? 1 : 0);

    // [XRINPUT] VR controllers become the pad. Merged ADDITIVELY on top of whatever the real
    // device reported, so a physical pad, the keyboard and the controllers all work at once and
    // nobody's existing setup breaks - the release rule for this mod.
    OLXrPad p;
    if (OLXR_MotionControlsReady() && OLXR_GetPad(&p)) {
        static DWORD s_packet = 0;
        if (r != ERROR_SUCCESS) { ZeroMemory(st, sizeof(*st)); }   // empty slot: this hook IS the pad
        st->Gamepad.wButtons |= MapPadButtons(p) | CamcorderGripButton(p);
        // Sticks: take the fabricated values only when actually deflected, so a real stick is never fought.
        if (fabsf(p.moveX) > 0.12f || fabsf(p.moveY) > 0.12f) {
            st->Gamepad.sThumbLX = StickAxis(p.moveX);
            st->Gamepad.sThumbLY = StickAxis(p.moveY);
        }
        if (fabsf(p.lookX) > 0.12f || fabsf(p.lookY) > 0.12f) {
            st->Gamepad.sThumbRX = StickAxis(p.lookX);
            st->Gamepad.sThumbRY = StickAxis(p.lookY);
        }
        const BYTE lt = (BYTE)(p.trigL * 255.0f), rt = (BYTE)(p.trigR * 255.0f);
        if (lt > st->Gamepad.bLeftTrigger)  st->Gamepad.bLeftTrigger  = lt;
        if (rt > st->Gamepad.bRightTrigger) st->Gamepad.bRightTrigger = rt;
        st->dwPacketNumber = ++s_packet;   // must change or a deduping reader skips this update
        InterlockedIncrement(&g_padFaked);
        return ERROR_SUCCESS;
    }

    if (!g_padStub || r == ERROR_SUCCESS) return r;   // a real pad always wins
    static DWORD s_stubPacket = 0;
    const DWORD phase = (GetTickCount() / 1500u) & 1u;
    ZeroMemory(st, sizeof(*st));
    // The packet number MUST change or an input layer that dedupes on it will skip the read -
    // which would look exactly like the game ignoring the fabricated pad and give a false negative.
    st->dwPacketNumber = ++s_stubPacket;
    st->Gamepad.sThumbLY = phase ? (SHORT)-24000 : (SHORT)24000;   // back / forward, well past deadzone
    InterlockedIncrement(&g_padFaked);
    return ERROR_SUCCESS;
}

// Watch every key for the first fresh press and take it as the new binding. Modifiers and the
// menu's own navigation keys are skipped so a rebind cannot leave the menu unusable.
// forMenu picks which binding is being set, and it also decides whether INSERT is allowed: it
// has to be, or someone who rebinds the menu away from INSERT could never choose it again.
static void CaptureKeyBind(bool forMenu) {
    static const int kSkip[] = { VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
                                 VK_ESCAPE, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_RETURN };
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
        if (!forMenu && vk == VK_INSERT) continue;
        bool skip = false;
        for (int i = 0; i < (int)(sizeof(kSkip) / sizeof(kSkip[0])); ++i) if (kSkip[i] == vk) skip = true;
        if (skip) continue;
        if (forMenu) {
            InterlockedExchange(&g_menuVk, vk);
            // Both on one key would open the menu and recenter at the same time. The menu wins,
            // and the recenter key steps aside rather than firing invisibly underneath it.
            if (g_recenterVk == vk) InterlockedExchange(&g_recenterVk, 0);
            Log("[OLVR][MENU] menu key rebound to %s", OLProxy_KeyName(vk));
        } else {
            InterlockedExchange(&g_recenterVk, vk);
            if (g_menuVk == vk) InterlockedExchange(&g_menuVk, VK_INSERT);
            Log("[OLVR][RECENTER] key rebound to %s", OLProxy_KeyName(vk));
        }
        InterlockedExchange(&g_bindCapture, 0);
        return;
    }
}

static void CapturePadBind() {
    const WORD b = PadButtons();
    if (!b) return;
    for (int bit = 0; bit < 16; ++bit) {
        const WORD mask = (WORD)(1 << bit);
        if (!(b & mask)) continue;
        InterlockedExchange(&g_recenterPad, mask);
        InterlockedExchange(&g_bindCapture, 0);
        Log("[OLVR][RECENTER] controller shortcut rebound to %s", OLProxy_PadName(mask));
        return;
    }
}

static DWORD WINAPI KeyThread(LPVOID) {
    bool pins = false, pup = false, pdn = false, plf = false, prt = false;
    bool pw = false, ps = false, pa = false, pd = false, pen = false, psp = false;
    bool prec = false, ppad = false;
    DWORD lastPadTapMs = 0;
    int  padTick = 0; bool padReported = false;   // [PADSTUB] report cadence
    for (;;) {
        // [RECENTER] rebind capture first: while it is armed, the pressed key is a binding, not
        // a command, so nothing else in this loop should act on it.
        const LONG cap = g_bindCapture;
        if (cap == 1) { CaptureKeyBind(false); Sleep(30); continue; }
        if (cap == 2) { CapturePadBind();      Sleep(30); continue; }
        if (cap == 3) { CaptureKeyBind(true);  Sleep(30); continue; }

        // Recenter key. Not while the menu is open: the menu has its own button, and the letter
        // keys are menu navigation there.
        if (g_recenterVk && !OLMenu_Visible() && Bare((int)g_recenterVk, prec)) {
            Log("[OLVR][RECENTER] %s pressed", OLProxy_KeyName((int)g_recenterVk));
            OLXR_Recenter();
        }
        // Controller: two presses of the bound button inside the double-press window.
        if (g_recenterPad) {
            const bool padDown = (PadButtons() & (WORD)g_recenterPad) != 0;
            const bool padTap = padDown && !ppad;
            ppad = padDown;
            if (padTap) {
                const DWORD now = GetTickCount();
                if (lastPadTapMs && (now - lastPadTapMs) <= kPadDoubleMs) {
                    lastPadTapMs = 0;
                    Log("[OLVR][RECENTER] %s double press", OLProxy_PadName((int)g_recenterPad));
                    OLXR_Recenter();
                } else lastPadTapMs = now;
            }
        }

        if (g_menuVk && Bare((int)g_menuVk, pins)) OLMenu_Toggle();
        if (OLMenu_Visible()) {
            // Arrows are the primary control (the camera is frozen while this is open, so a
            // keyboard menu is fully usable). Also accept W/A/S/D + Enter, since a hand is
            // usually already there, and the gamepad d-pad maps to the arrows on most setups.
            if (Bare(VK_UP,    pup) || Bare('W', pw)) OLMenu_Nav(-1);
            if (Bare(VK_DOWN,  pdn) || Bare('S', ps)) OLMenu_Nav(+1);
            if (Bare(VK_LEFT,  plf) || Bare('A', pa)) OLMenu_Adjust(-1);
            if (Bare(VK_RIGHT, prt) || Bare('D', pd)) OLMenu_Adjust(+1);
            if (Bare(VK_RETURN, pen) || Bare(VK_SPACE, psp)) OLMenu_Adjust(+1);   // activate rows
        }
        // EVERY DEVELOPER HOTKEY IS GONE (2026-08-03, release). There used to be fourteen
        // CTRL+ALT chords here: stereo split on and off, the alternate-eye render mode, the
        // occlusion bypass, eye separation nudges, the eye-swap, the build-order flip, the
        // camera nudge, the layer-type flip, the UI mirror cycle and a fullscreen re-assert.
        // They were the tools that built this, and several of them visibly break the picture.
        // A player who finds one has no idea what it did and no way back, so they do not ship.
        // Everything a player actually needs is in the menu on INSERT, and the two recenter
        // bindings above. Anything left here must be something a stranger can press safely.

        // [PADSTUB] poll report every ~3 s, timed HERE rather than in the hook: a counter that
        // only advances when the hook runs cannot tell "the game stopped asking" from "the game
        // never asked", and that distinction is the entire experiment.
        if (++padTick >= 100) {
            padTick = 0;
            const long p0  = InterlockedExchange(&g_padPolls0, 0);
            const long pa  = InterlockedExchange(&g_padPollsAll, 0);
            const long fk  = InterlockedExchange(&g_padFaked, 0);
            if (p0 || pa || padReported) {
                padReported = true;
                Log("[OLVR][PADSTUB] slot0=%ld/3s allSlots=%ld/3s realPad=%s stub=%s faked=%ld/3s",
                    p0, pa, g_padRealOk ? "connected" : "absent",
                    g_padStub ? "on" : "off", fk);
            }
        }
        Sleep(30);
    }
}

// ---- [SETTINGS] tune in the headset, press one button, keep it -------------------------------
extern "C" float OLUE3_GetHalfEye();        extern "C" void OLUE3_SetHalfEye(float v);
extern "C" float OLUE3_GetWorldToMeters();  extern "C" void OLUE3_SetWorldToMeters(float v);
extern "C" int   OLUE3_GetHeadTrack();      extern "C" int  OLUE3_GetHeadPos();
extern "C" int   OLUE3_GetHeadRoll();       extern "C" float OLUE3_GetLeanGain();
extern "C" int   OLUE3_GetEyeSwap();        extern "C" void OLUE3_SetEyeSwap(int v);
extern "C" int   OLUE3_GetCineFov();
extern "C" int   OLUE3_GetNvLight();        extern "C" void OLUE3_SetNvLight(int v);
extern "C" float OLXR_GetConvergence();     extern "C" void OLXR_SetConvergence(float v);
extern "C" int   OLXR_GetStereoModel();     extern "C" void OLXR_SetStereoModel(int v);
extern "C" float OLXR_GetSmoothing();       extern "C" void OLXR_SetSmoothing(float v);
extern "C" float OLXR_GetGamma();           extern "C" void OLXR_SetGamma(float v);
extern "C" float OLXR_GetScreenBelow();     extern "C" void OLXR_SetScreenBelow(float v);
extern "C" int   OLXR_GetHandCam();         extern "C" float OLXR_GetHandCamStrength();

// Everything worth tuning is tuned WEARING the headset, where writing a number down by hand is
// not an option. Save writes the live values to the same keys the loaders read, so save/restore
// is symmetric and the ini stays the single source of truth.
//
// UNITS WARNING (cost a near-miss 2026-08-03): Convergence here is THIS build's knob - slider
// -3..+3, one unit = 8% of the eye width, default 0.5. A later build used a different scale
// (-0.06..0.06) on the SAME key name, so an ini written by that build reads as ~zero
// convergence here and silently flattens the depth. The loader logs what it applied; if depth
// ever looks flat straight after a launch, read that line FIRST.
static bool IniPath(wchar_t* out, size_t n) {
    wchar_t exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    return swprintf_s(out, n, L"%soutlastvr.ini", exe) > 0;
}

static void WriteF(const wchar_t* sec, const wchar_t* key, float val, const wchar_t* ini) {
    wchar_t v[64]; swprintf_s(v, L"%.3f", val); WritePrivateProfileStringW(sec, key, v, ini);
}
static void WriteI(const wchar_t* sec, const wchar_t* key, long val, const wchar_t* ini) {
    wchar_t v[32]; swprintf_s(v, L"%ld", val); WritePrivateProfileStringW(sec, key, v, ini);
}

// The tunables no other loader already covers. Called after OLUE3_Init so its defaults cannot
// overwrite a saved value. A key that is absent leaves the built-in default alone.
static void LoadSavedSettings() {
    wchar_t ini[MAX_PATH]; if (!IniPath(ini, MAX_PATH)) return;
    wchar_t v[64];
    GetPrivateProfileStringW(L"Render", L"Convergence", L"", v, 64, ini);
    if (v[0]) OLXR_SetConvergence((float)_wtof(v));
    GetPrivateProfileStringW(L"Render", L"StereoModel", L"", v, 64, ini);
    if (v[0]) OLXR_SetStereoModel(_wtoi(v) ? 1 : 0);
    GetPrivateProfileStringW(L"Render", L"Smoothing", L"", v, 64, ini);
    if (v[0]) OLXR_SetSmoothing((float)_wtof(v));
    // Gamma is deliberately NOT read. It blacked out a headset, the control is gone from the
    // menu, and an ini key left over from an older build must not be able to bring it back.
    GetPrivateProfileStringW(L"Render", L"CinemaScreenBelowDeg", L"", v, 64, ini);
    if (v[0]) OLXR_SetScreenBelow((float)_wtof(v));
    GetPrivateProfileStringW(L"Stage3", L"EyeSwap", L"", v, 64, ini);
    if (v[0]) OLUE3_SetEyeSwap(_wtoi(v) ? 1 : 0);
    GetPrivateProfileStringW(L"Stage3", L"NvLight", L"", v, 64, ini);
    if (v[0]) OLUE3_SetNvLight(_wtoi(v) ? 1 : 0);
    { LONG s = (LONG)GetPrivateProfileIntW(L"Render", L"HudScalePct",   g_hudScalePct, ini);
      LONG x = (LONG)GetPrivateProfileIntW(L"Render", L"HudOffsetXPct", g_hudOffXPct,  ini);
      LONG y = (LONG)GetPrivateProfileIntW(L"Render", L"HudOffsetYPct", g_hudOffYPct,  ini);
      OLProxy_SetHudFit((float)s * 0.01f, (float)x * 0.01f, (float)y * 0.01f); }
    { LONG k = (LONG)GetPrivateProfileIntW(L"Input", L"RecenterKey",    g_recenterVk,  ini);
      LONG p = (LONG)GetPrivateProfileIntW(L"Input", L"RecenterPad",    g_recenterPad, ini);
      LONG m = (LONG)GetPrivateProfileIntW(L"Input", L"MenuKey",        g_menuVk,      ini);
      if (k >= 0 && k <= 0xFE) InterlockedExchange(&g_recenterVk, k);
      if (p >= 0 && p <= 0xFFFF) InterlockedExchange(&g_recenterPad, p);
      // A zero here would leave no way to open the menu at all, so it falls back rather than
      // being obeyed. This is the one binding that cannot be repaired from inside the game.
      if (m > 0 && m <= 0xFE) InterlockedExchange(&g_menuVk, m);
      else InterlockedExchange(&g_menuVk, VK_INSERT); }

    // FIRST RUN. Nobody reads a readme, so the first time this mod is ever launched the settings
    // menu opens itself once. That is the only way a player finds out it exists, what it can do,
    // and which key brings it back. The flag is written immediately, not on exit, so a crash or
    // an alt-F4 during that first session still counts as shown.
    g_firstRunPending = GetPrivateProfileIntW(L"Menu", L"Introduced", 0, ini) ? 0 : 1;
    if (g_firstRunPending) {
        WriteI(L"Menu", L"Introduced", 1, ini);
        Log("[OLVR][MENU] first run on this install: the settings menu will open once by itself");
    }
    Log("[OLVR][SETTINGS] loaded: IPD half=%.2fuu conv=%.2f (this build: -3..3, 8%%/unit) "
        "smooth=%.2f gamma=%.2f hud=%ld%%(%ld%%,%ld%%)",
        OLUE3_GetHalfEye(), OLXR_GetConvergence(), OLXR_GetSmoothing(), OLXR_GetGamma(),
        g_hudScalePct, g_hudOffXPct, g_hudOffYPct);
}

// [PADSTUB] Install the counter/stub hook. Hooks the EXPORT by ordinal, not an IAT entry by
// name: OLGame.exe imports XINPUT1_3.dll ordinals 2 and 3 only, with no name imports at all
// (dumpbin, 2026-08-17), so a name-based IAT patch - the usual approach, and what CP2077's
// InstallXInputHook does - would find nothing to patch here.
// The mod's own pad readers (menu navigation, the recenter double-tap) load xinput1_4 first, a
// different module, so they are not affected by this and keep seeing the raw device.
extern "C" void OLProxy_InstallPadProbe() {
    wchar_t ini[MAX_PATH];
    if (IniPath(ini, MAX_PATH))
        g_padStub = GetPrivateProfileIntW(L"Stage3", L"PadStub", 0, ini) ? 1 : 0;
    HMODULE m = GetModuleHandleW(L"xinput1_3.dll");
    if (!m) {
        Log("[OLVR][PADSTUB] xinput1_3.dll is not loaded in this process - the game has not "
            "touched XInput at all");
        return;
    }
    void* fn = (void*)GetProcAddress(m, MAKEINTRESOURCEA(2));   // ordinal 2 = XInputGetState
    if (!fn) { Log("[OLVR][PADSTUB] xinput1_3 ordinal 2 not found"); return; }
    MH_STATUS ms = MH_Initialize();   // another site may already have done this
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[OLVR][PADSTUB] MH_Initialize failed (%d)", (int)ms); return;
    }
    if (MH_CreateHook(fn, (void*)&Hook_XiGetState, (void**)&o_XiGetState) == MH_OK &&
        MH_EnableHook(fn) == MH_OK) {
        Log("[OLVR][PADSTUB] watching the game's XInput reads (stub %s). This build answers "
            "for an EMPTY slot 0 only; a real controller always wins.",
            g_padStub ? "ON - empty slot 0 will walk forward 1.5s / back 1.5s" : "off, counting only");
    } else {
        Log("[OLVR][PADSTUB] hook failed");
    }
}

extern "C" int OLProxy_SaveSettings() {
    wchar_t ini[MAX_PATH]; if (!IniPath(ini, MAX_PATH)) return 0;
    WriteF(L"Stage3", L"HalfEyeUU",     OLUE3_GetHalfEye(),       ini);
    WriteF(L"Stage3", L"WorldToMeters", OLUE3_GetWorldToMeters(), ini);
    WriteI(L"Stage3", L"HeadTracking",  OLUE3_GetHeadTrack(),     ini);
    WriteI(L"Stage3", L"HeadPosition",  OLUE3_GetHeadPos(),       ini);
    WriteF(L"Stage3", L"LeanGain",      OLUE3_GetLeanGain(),      ini);
    WriteI(L"Stage3", L"HeadRoll",      OLUE3_GetHeadRoll(),      ini);
    WriteI(L"Stage3", L"EyeSwap",       OLUE3_GetEyeSwap(),       ini);
    WriteI(L"Stage3", L"CineFov",       OLUE3_GetCineFov(),       ini);
    WriteI(L"Stage3", L"NvLight",       OLUE3_GetNvLight(),       ini);
    WriteF(L"Render", L"Convergence",   OLXR_GetConvergence(),    ini);
    WriteI(L"Render", L"StereoModel",   OLXR_GetStereoModel(),    ini);
    WriteF(L"Render", L"Smoothing",     OLXR_GetSmoothing(),      ini);
    WriteF(L"Render", L"CinemaScreenBelowDeg", OLXR_GetScreenBelow(), ini);
    WriteI(L"Render", L"UiMirror",      g_uiMirror,               ini);
    WriteI(L"Render", L"HudScalePct",   g_hudScalePct,            ini);
    WriteI(L"Render", L"HudOffsetXPct", g_hudOffXPct,             ini);
    WriteI(L"Render", L"HudOffsetYPct", g_hudOffYPct,             ini);
    WriteI(L"Input",  L"HandCamcorder", OLXR_GetHandCam(),        ini);
    WriteF(L"Input",  L"HandCamcorderStrength", OLXR_GetHandCamStrength(), ini);
    WriteI(L"Input",  L"RecenterKey",   g_recenterVk,             ini);
    WriteI(L"Input",  L"RecenterPad",   g_recenterPad,            ini);
    WriteI(L"Input",  L"MenuKey",       g_menuVk,                 ini);
    Log("[OLVR][SETTINGS] SAVED as defaults: depth=%.2f focus=%.2f steadying=%.2f "
        "hud=%ld%%(%ld%%,%ld%%)", OLUE3_GetHalfEye(), OLXR_GetConvergence(), OLXR_GetSmoothing(),
        g_hudScalePct, g_hudOffXPct, g_hudOffYPct);
    return 1;
}

// The stereo work happens in the GAME's renderer; the headset is just a consumer of it. When
// something is wrong in the rendered frame there is no reason to wear a headset to look at it,
// so XR can be switched off entirely and the whole thing debugged on the monitor.
static void ReadXrConfig() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%soutlastvr.ini", exe);
    wchar_t val[32] = { 0 };
    GetPrivateProfileStringW(L"XR", L"Enabled", L"1", val, 32, ini);
    g_xrAllowed = (_wcsicmp(val, L"0") == 0 || _wcsicmp(val, L"false") == 0) ? 0 : 1;
    Log("[OLVR] XR %s (outlastvr.ini [XR] Enabled)", g_xrAllowed ? "ENABLED" : "DISABLED - flat, no headset needed");
    // [PACING] In VR the game must present on the HEADSET's clock (xrWaitFrame), not the
    // monitor's. With the game's own vsync live there are TWO serial blocking waits on two
    // drifting clocks, the submit slides past the compositor's deadline on the beat frequency,
    // and SteamVR logs it as endless frame timeouts (510 in a 16s session, measured 2026-08-04)
    // while cross-fading the game semi-transparent into its backdrop - it never leaves its
    // startup fade. So when XR is on, the game's vsync is forced OFF at CreateDevice/Reset and
    // xrWaitFrame becomes the only pacer. [XR] KeepGameVsync=1 restores the old behaviour.
    g_keepVsync = GetPrivateProfileIntW(L"XR", L"KeepGameVsync", 0, ini) ? 1 : 0;
    if (g_xrAllowed)
        Log("[OLVR][PACING] game vsync %s (headset clock %s the pacer)",
            g_keepVsync ? "KEPT by ini - two clocks will fight" : "forced OFF",
            g_keepVsync ? "is NOT" : "is");
    { LONG m = (LONG)GetPrivateProfileIntW(L"Render", L"UiMirror", UIMIRROR_ON, ini);
      g_uiMirror = (m >= 0 && m <= 2) ? m : UIMIRROR_ON; }
    Log("[OLVR][UIMIRROR] mode=%ld (0=off 1=on 2=menus-only), from the ini", g_uiMirror);
}

// Must run BEFORE the game creates its device, so it is called from the attach-time worker.
static void InstallDisplayHooks() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%soutlastvr.ini", exe);
    g_reqW = (LONG)GetPrivateProfileIntW(L"Render", L"ResX", 0, ini);
    g_reqH = (LONG)GetPrivateProfileIntW(L"Render", L"ResY", 0, ini);
    g_autoFs = GetPrivateProfileIntW(L"Render", L"AutoFullscreen", 1, ini) ? 1 : 0;   // [FSFIX]
    Log("[OLVR][FSFIX] AutoFullscreen %s (re-assert exclusive-fullscreen after the compositor "
        "drops it)", g_autoFs ? "ON" : "off");

    const LONG realW = GetSystemMetrics(SM_CXSCREEN), realH = GetSystemMetrics(SM_CYSCREEN);
    // RESOLUTION IS AUTOMATIC, and there is genuinely no choice to offer. The desktop size is a
    // hard ceiling here: this engine sizes its backbuffer from its own window client rect and
    // ignores every display query, and forcing a buffer above the desktop was measured to come
    // back at a broken aspect ratio, stretched and worse than 4K in every way. Below the desktop
    // is simply a worse picture. So the right number is always "the desktop", and asking a player
    // to pick it would be a menu of one good answer and a list of downgrades.
    // ResX/ResY remain readable for the one real case, a machine that cannot hold framerate at
    // full size, and for testing. Absent or zero means auto.
    if (g_reqW <= 0 || g_reqH <= 0) {
        g_reqW = realW; g_reqH = realH;
        Log("[OLVR][SCREQ] resolution AUTO -> matching the desktop, %ldx%ld", g_reqW, g_reqH);
    } else {
        Log("[OLVR][SCREQ] resolution from the ini: %ldx%ld (desktop is %ldx%ld)", g_reqW, g_reqH, realW, realH);
    }
    if (g_reqW <= 0 || g_reqH <= 0) { Log("[OLVR][SCREQ] no usable desktop size -> resolution untouched"); return; }
    // Arm whenever a size is asked for. Do NOT compare against the primary here: the game may be
    // windowed on a SMALLER second monitor (it was), so a primary-sized request still matters.
    // The per-monitor check inside the hook keeps big-enough displays untouched.
    if (MH_Initialize() == MH_ERROR_MEMORY_ALLOC) { Log("[OLVR][SCREQ] MH_Initialize failed"); return; }
    bool ok = MH_CreateHookApi(L"user32", "GetMonitorInfoW", (void*)&Hook_GetMonitorInfoW, (void**)&o_GetMonitorInfoW) == MH_OK
           && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    if (ok && MH_CreateHookApi(L"user32", "GetMonitorInfoA", (void*)&Hook_GetMonitorInfoA, (void**)&o_GetMonitorInfoA) == MH_OK)
        MH_EnableHook(MH_ALL_HOOKS);
    g_spoofArmed = ok ? 1 : 0;
    Log("[OLVR][SCREQ] primary-monitor spoof %s -> the game should size its windowed backbuffer %ldx%ld (%ldx%ld per eye)",
        ok ? "ARMED" : "FAILED", g_reqW, g_reqH, g_reqW / 2, g_reqH);
}

static DWORD WINAPI Worker(LPVOID) {
    OpenLog();
    Log("[OLVR] ==== Outlast FlatXR probe (Stage 2: flat panel in headset) ====");
    Log("[OLVR] built=" __DATE__ " " __TIME__ " bitness=x64");
    wchar_t exe[MAX_PATH] = { 0 }; GetModuleFileNameW(NULL, exe, MAX_PATH);
    Log("[OLVR] exe=%ls pid=%lu", exe, GetCurrentProcessId());
    Log("[OLVR] keys: INSERT opens the settings menu, G recenters, double tap R3 recenters");
    ReadXrConfig();
    InstallDisplayHooks();   // BEFORE the game's CreateDevice, or the clamp has already happened
    CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr);
    OLUE3_Init();   // Stage 3 engine hook; no-ops unless outlastvr.ini arms it
    LoadSavedSettings();   // AFTER OLUE3_Init, so saved values are not overwritten by its defaults
    OLProxy_InstallPadProbe();   // [PADSTUB] motion controls step 1: does the game read a fabricated pad
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        OLMenu_StartInput(hInst);   // low-level mouse hook needs the module's HINSTANCE
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        OLXR_Shutdown();
        if (g_log) { Log("[OLVR] ==== detach: presents=%ld ====", g_presentCount); fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
