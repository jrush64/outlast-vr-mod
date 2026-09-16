// ol_menu.cpp - in-headset ImGui menu (INSERT), same tech as the Mass Effect / Dishonored menus.
// =============================================================================
// This is a REAL ImGui window: small, movable, resizable. The earlier hand-rolled GDI panel could
// never look or behave like the ME one because it was the wrong tool; ME1 and the Dishonored D3D9
// probe both use ImGui, so this does too.
//
// Outlast packs BOTH eyes into one backbuffer (the SBS split), so - unlike Dishonored, which drew
// ImGui straight onto the backbuffer + a sidecar - ImGui renders into an OFFSCREEN target here
// and that target is drawn into each half. The window floats over the game with a transparent
// surround.
//
// Input: a low-level mouse hook (WH_MOUSE_LL) drives a virtual cursor and reads the buttons below
// DirectInput, so it works even though Outlast owns the mouse; the game camera is frozen in the
// engine while the menu is open (ol_ue3.cpp), so nothing leaks. Gamepad + keyboard nav are fed to
// ImGui too.
// =============================================================================

#include <Windows.h>
#include <d3d9.h>
#include <xinput.h>
#include <cstdio>
#include <math.h>
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "ol_menu.h"

extern "C" void OLProxyLog(const char* line);
extern "C" int  OLUE3_StereoMode();
extern "C" int  OLUE3_SplitActive();
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy);

// ---- knobs owned elsewhere -------------------------------------------------------------------
extern "C" float OLUE3_GetHalfEye();      extern "C" void OLUE3_SetHalfEye(float v);
extern "C" float OLUE3_GetWorldToMeters();extern "C" void OLUE3_SetWorldToMeters(float v);
extern "C" int   OLUE3_GetHeadTrack();    extern "C" void OLUE3_SetHeadTrack(int v);
extern "C" int   OLUE3_GetHeadPos();      extern "C" void OLUE3_SetHeadPos(int v);
extern "C" int   OLXR_PosArmed();         extern "C" float OLXR_GetRtEyeSep();
extern "C" float OLUE3_GetLeanGain();     extern "C" void OLUE3_SetLeanGain(float v);
extern "C" int   OLXR_GetHandCam();       extern "C" void OLXR_SetHandCam(int v);
extern "C" float OLXR_GetHandCamStrength(); extern "C" void OLXR_SetHandCamStrength(float v);
extern "C" int   OLUE3_GetHeadRoll();     extern "C" void OLUE3_SetHeadRoll(int v);
extern "C" int   OLUE3_GetEyeSwap();      extern "C" void OLUE3_SetEyeSwap(int v);
extern "C" int   OLUE3_GetCineFov();      extern "C" void OLUE3_SetCineFov(int v);
extern "C" int   OLUE3_CineFovState();
extern "C" int   OLUE3_GetNvLight();      extern "C" void OLUE3_SetNvLight(int v);
extern "C" float OLXR_GetConvergence();   extern "C" void OLXR_SetConvergence(float v);
extern "C" int   OLXR_GetStereoModel();   extern "C" void OLXR_SetStereoModel(int v);
extern "C" float OLXR_GetSmoothing();     extern "C" void OLXR_SetSmoothing(float v);
extern "C" float OLXR_GetScreenBelow();   extern "C" void OLXR_SetScreenBelow(float v);
extern "C" float OLXR_GetGamma();         extern "C" void OLXR_SetGamma(float v);
extern "C" void  OLXR_ResetGamma();
extern "C" int   OLXR_GammaRampApplied();
extern "C" void  OLXR_GetEyeInfo(int* w, int* h, float* fovX, float* fovY);
extern "C" int   OLProxy_GetUiMirror();   extern "C" void OLProxy_SetUiMirror(int v);
extern "C" void  OLProxy_GetHudFit(float* scale, float* offX, float* offY);
extern "C" void  OLProxy_SetHudFit(float scale, float offX, float offY);
extern "C" int   OLProxy_SaveSettings();
extern "C" int   OLProxy_GetRecenterKey();
extern "C" int   OLProxy_GetRecenterPad();
extern "C" int   OLProxy_GetMenuKey();
extern "C" int   OLProxy_GetBindCapture();
extern "C" void  OLProxy_StartBindCapture(int which);
extern "C" const char* OLProxy_KeyName(int vk);
extern "C" const char* OLProxy_PadName(int mask);
void OLXR_Recenter();

// ---- the offscreen target ImGui renders into --------------------------------------------------
static const int kRTW = 1400, kRTH = 1050;   // 4:3; the window lives (and moves) inside this

static IDirect3DDevice9*  g_dev  = nullptr;
static IDirect3DTexture9*  g_rtTex  = nullptr;
static IDirect3DSurface9*  g_rtSurf = nullptr;
static bool g_inited  = false;
static bool g_visible = false;
static bool g_firstLayout = true;
static int  g_savedFrames = 0;    // "saved" confirmation lingers a few seconds
static int  g_savedOk = 0;

// ---- virtual cursor from the low-level mouse hook ---------------------------------------------
static volatile float g_curX = kRTW * 0.5f, g_curY = kRTH * 0.5f;
static volatile long  g_lBtn = 0, g_rBtn = 0, g_wheel = 0;
static HHOOK g_llHook = nullptr;
static HANDLE g_inputThread = nullptr;
static HINSTANCE g_self = nullptr;
static POINT g_center = { 0, 0 };

static LRESULT CALLBACK LLMouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && g_visible) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)l;
        const bool injected = (m->flags & LLMHF_INJECTED) != 0;
        switch (w) {
            case WM_MOUSEMOVE:
                if (!injected) {
                    const float s = 1.1f;
                    float nx = g_curX + (m->pt.x - g_center.x) * s;
                    float ny = g_curY + (m->pt.y - g_center.y) * s;
                    if (nx < 0) nx = 0; if (nx > kRTW) nx = kRTW;
                    if (ny < 0) ny = 0; if (ny > kRTH) ny = kRTH;
                    g_curX = nx; g_curY = ny;
                    SetCursorPos(g_center.x, g_center.y);
                }
                return 1;
            case WM_LBUTTONDOWN: InterlockedExchange(&g_lBtn, 1); return 1;
            case WM_LBUTTONUP:   InterlockedExchange(&g_lBtn, 0); return 1;
            case WM_RBUTTONDOWN: InterlockedExchange(&g_rBtn, 1); return 1;
            case WM_RBUTTONUP:   InterlockedExchange(&g_rBtn, 0); return 1;
            case WM_MOUSEWHEEL:  InterlockedExchangeAdd(&g_wheel, (short)HIWORD(m->mouseData)); return 1;
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 1;
            default: break;
        }
    }
    return CallNextHookEx(g_llHook, code, w, l);
}

static DWORD WINAPI InputThread(LPVOID) {
    g_center.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    g_center.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    g_llHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc, g_self, 0);
    OLProxyLog(g_llHook ? "[OLVR][MENU] mouse hook installed - the cursor works inside the menu"
                        : "[OLVR][MENU] LL mouse hook FAILED - gamepad/keyboard only");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

void OLMenu_StartInput(void* hinst) {
    if (g_inputThread) return;
    g_self = (HINSTANCE)hinst;
    g_inputThread = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
}

// ---- gamepad ----------------------------------------------------------------------------------
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE*);
static XInputGetState_t g_xinput = nullptr;
static void LoadXInput() {
    const wchar_t* d[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (auto n : d) { HMODULE m = GetModuleHandleW(n); if (!m) m = LoadLibraryW(n);
        if (m) { g_xinput = (XInputGetState_t)GetProcAddress(m, "XInputGetState"); if (g_xinput) return; } }
}

// ---- init / lifecycle -------------------------------------------------------------------------
int  OLMenu_Visible() { return g_visible ? 1 : 0; }

static bool EnsureRT() {
    if (g_rtTex) return true;
    if (FAILED(g_dev->CreateTexture(kRTW, kRTH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &g_rtTex, nullptr))) { g_rtTex = nullptr; return false; }
    if (FAILED(g_rtTex->GetSurfaceLevel(0, &g_rtSurf))) { g_rtTex->Release(); g_rtTex = nullptr; return false; }
    return true;
}

static bool EnsureInit(IDirect3DDevice9* dev) {
    if (g_inited) return true;
    g_dev = dev;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = true;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.35f);   // readable in a headset, but SMALLER than before
    io.FontGlobalScale = 1.35f;
    if (!ImGui_ImplDX9_Init(dev)) { OLProxyLog("[OLVR][MENU] ImGui_ImplDX9_Init FAILED"); ImGui::DestroyContext(); return false; }
    LoadXInput();
    if (!EnsureRT()) { OLProxyLog("[OLVR][MENU] offscreen RT create FAILED"); }
    g_inited = true;
    OLProxyLog("[OLVR][MENU] ImGui menu ready - move/resize the window, drag the sliders");
    return true;
}

void OLMenu_OnDeviceLost() {   // called from Hook_Reset BEFORE the game resets the device
    if (g_rtSurf) { g_rtSurf->Release(); g_rtSurf = nullptr; }
    if (g_rtTex)  { g_rtTex->Release();  g_rtTex  = nullptr; }
    if (g_inited) ImGui_ImplDX9_InvalidateDeviceObjects();
}
void OLMenu_OnDeviceReset() {   // called from Hook_Reset AFTER a successful reset
    if (g_inited) { ImGui_ImplDX9_CreateDeviceObjects(); EnsureRT(); }
}

void OLMenu_Toggle() {
    g_visible = !g_visible;
    if (g_visible) { InterlockedExchange(&g_lBtn, 0); InterlockedExchange(&g_rBtn, 0); ClipCursor(nullptr); }
    OLProxyLog(g_visible ? "[OLVR][MENU] open - mouse moves the cursor, drag the window/sliders (INSERT closes)"
                         : "[OLVR][MENU] closed");
}
// Kept so the proxy's key thread still links; ImGui does its own nav now.
void OLMenu_Nav(int) {}
void OLMenu_Adjust(int) {}

// ---- input feed -------------------------------------------------------------------------------
static void PumpInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(g_curX, g_curY);
    io.AddMouseButtonEvent(0, g_lBtn != 0);
    io.AddMouseButtonEvent(1, g_rBtn != 0);
    long wh = InterlockedExchange(&g_wheel, 0);
    if (wh) io.AddMouseWheelEvent(0.0f, (float)wh / WHEEL_DELTA);

    // keyboard nav (the camera is frozen, so these are safe to read directly)
    io.AddKeyEvent(ImGuiKey_UpArrow,    (GetAsyncKeyState(VK_UP)     & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_DownArrow,  (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_LeftArrow,  (GetAsyncKeyState(VK_LEFT)   & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_RightArrow, (GetAsyncKeyState(VK_RIGHT)  & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_Enter,      (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_Space,      (GetAsyncKeyState(VK_SPACE)  & 0x8000) != 0);

    if (g_xinput) {
        XINPUT_STATE st; ZeroMemory(&st, sizeof(st));
        if (g_xinput(0, &st) == ERROR_SUCCESS) {
            const WORD b = st.Gamepad.wButtons;
            io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (b & XINPUT_GAMEPAD_DPAD_UP) != 0);
            io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
            io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
            io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
            io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (b & XINPUT_GAMEPAD_A) != 0);
            io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & XINPUT_GAMEPAD_B) != 0);
        }
    }
}

// ---- the widget tree --------------------------------------------------------------------------
static void BuildUI() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_firstLayout) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(430, 560), ImGuiCond_FirstUseEver);
        g_firstLayout = false;
    }
    ImGui::Begin("Outlast VR", nullptr, ImGuiWindowFlags_NoCollapse);   // movable + resizable

    // WHAT IS HIDDEN AND WHY. Every setting below still exists, still loads and still saves;
    // it just is not shown, because a player opening this in a headset should see the handful
    // of things worth touching and nothing else. Hidden: positional tracking, head roll and the
    // camcorder light toggle (all correct at their defaults, all confusing to explain), the
    // UI-in-both-eyes mode (wrong on any setting but "on"), world scale (only affects
    // positional, which is off) and the cinema screen (its whole point is to be off).
    ImGui::TextDisabled("3D depth");
    // The stereo model picker is NOT shown. The Mass Effect model won the live comparison and is
    // the default; the old Outlast model survives only as an ini escape hatch ([Render]
    // StereoModel=0) because it is the history of this lane. It is not a choice worth putting in
    // front of a player: it costs field of view whenever focus distance moves, and its Swap
    // inverts the stereo outright.
    // Separation is shown against a real eye spacing, because "3.5 uu" means nothing in a
    // headset while "0.9x your eyes" is something you can picture. The scale is the MEASURED
    // 100 uu/m ([LEAN100] - eye height 168 uu, walk speed, and the tuned depth landing at the
    // measured real IPD all agree), which is why a separation of 3.30 reads as ~1x: the stereo
    // was tuned to true scale by eye, without the number being derived on purpose.
    { float v = OLUE3_GetHalfEye();
      float ipd = OLXR_GetRtEyeSep(); if (ipd < 0.03f || ipd > 0.12f) ipd = 0.064f;
      const float ratio = (v * 2.0f / 100.0f) / ipd;
      char lbl[64]; sprintf_s(lbl, "%.2f  (%.1fx your real eye spacing)", v, ratio);
      if (ImGui::SliderFloat("Depth", &v, 0.0f, 12.0f, lbl)) OLUE3_SetHalfEye(v);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "How far apart your two eyes see from. More depth also makes the world\n"
          "feel smaller. This is the main 3D control."); }
    { float v = OLXR_GetConvergence();
      if (ImGui::SliderFloat("Focus distance", &v, -3.0f, 3.0f, "%.2f")) OLXR_SetConvergence(v);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Sets how far away things sit before they look flat.\n"
          "If close objects look doubled or hard to look at, raise this a little."); }
    { bool b = OLUE3_GetEyeSwap() != 0;
      if (ImGui::Checkbox("Swap left and right eye", &b)) OLUE3_SetEyeSwap(b);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Only needed if the 3D looks inside out, with near things\n"
          "appearing far away."); }
    // Lean distance (world scale) stays out of the menu: it is derived from the depth slider now,
    // so there is nothing to tune. Positional is BACK (2026-08-17) as one checkbox, because the
    // thing that made it wrong - measuring from wherever the headset lay when the game started -
    // is fixed underneath ([HEADPOS] in ol_xr.cpp), not papered over with a slider.

    ImGui::Separator();
    ImGui::TextDisabled("Head tracking");
    { bool b = OLUE3_GetHeadTrack() != 0;
      if (ImGui::Checkbox("Look around with your head", &b)) OLUE3_SetHeadTrack(b); }
    // [LEAN100] Camera lean at the measured world scale (100 uu/m, from the CamProbe run).
    // Gain 1.0 keeps declared == rendered exactly - the setting that finally made it stable -
    // so the slider is a taste control around truth, not a sensitivity hack.
    { bool b = OLUE3_GetHeadPos() != 0;
      if (ImGui::Checkbox("Lean and duck with your body", &b)) OLUE3_SetHeadPos(b);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Moving your head moves the camera: lean round a corner, duck under a table.\n"
          "Recenter once you are sat comfortably, and again any time it feels off.");
      if (b) {
          ImGui::SameLine();
          if (OLXR_PosArmed()) ImGui::TextDisabled("ready");
          else ImGui::TextDisabled("waiting - recenter, or hold still a moment");
          float g = OLUE3_GetLeanGain();
          if (ImGui::SliderFloat("Lean amount", &g, 0.5f, 2.0f, "%.2f")) OLUE3_SetLeanGain(g);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip(
              "1.00 moves the camera exactly as far as you move, which is also the\n"
              "most stable setting. Change it only if you want less or more.");
      } }
    { float v = OLXR_GetSmoothing();
      if (ImGui::SliderFloat("Steadying", &v, 0.0f, 0.9f, v <= 0.001f ? "off" : "%.2f")) OLXR_SetSmoothing(v);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Settles small shakes while you hold still, and gets out of the way\n"
          "the moment you actually turn your head. Turn it down if looking\n"
          "around ever feels slow to respond."); }

    ImGui::Separator();
    // [HANDCAM] The camcorder IS the game's camera, so aiming the camera from the controller is
    // aiming the camcorder. Hold the right grip and you are holding it.
    ImGui::TextDisabled("Camcorder");
    { bool b = OLXR_GetHandCam() != 0;
      if (ImGui::Checkbox("Hold it in your hand", &b)) OLXR_SetHandCam(b);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Squeeze the right grip to raise the camcorder. While you hold it, the\n"
          "view looks through it, so pointing the controller aims the camera.");
      if (b) {
          float s = OLXR_GetHandCamStrength();
          if (ImGui::SliderFloat("How much it takes over", &s, 0.0f, 1.0f, "%.2f"))
              OLXR_SetHandCamStrength(s);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip(
              "1.00 aims entirely with your hand. Lower it to keep some of your\n"
              "head aim mixed in, which is steadier while walking.");
      } }

    ImGui::Separator();
    // RECENTER. The one thing a player needs mid-game without opening anything: sit differently,
    // drift off centre, put the world back where you are facing.
    ImGui::TextDisabled("Recenter the view");
    if (ImGui::Button("Recenter now", ImVec2(-1, 0))) OLXR_Recenter();
    { const int cap = OLProxy_GetBindCapture();
      char lbl[96];
      sprintf_s(lbl, "Keyboard: %s", OLProxy_KeyName(OLProxy_GetRecenterKey()));
      ImGui::Text("%s", lbl);
      ImGui::SameLine();
      if (cap == 1) ImGui::TextDisabled("press any key...");
      else if (ImGui::SmallButton("Change key")) OLProxy_StartBindCapture(1);

      sprintf_s(lbl, "Controller: double tap %s", OLProxy_PadName(OLProxy_GetRecenterPad()));
      ImGui::Text("%s", lbl);
      ImGui::SameLine();
      if (cap == 2) ImGui::TextDisabled("press any button...");
      else if (ImGui::SmallButton("Change button")) OLProxy_StartBindCapture(2);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Two quick taps, so it cannot happen by accident while you are running."); }

    ImGui::Separator();
    // The key that opens THIS menu. Worth rebinding for anyone on a keyboard without an INSERT
    // key, which is most compact and laptop boards.
    ImGui::TextDisabled("This menu");
    { const int cap = OLProxy_GetBindCapture();
      char lbl[96];
      sprintf_s(lbl, "Opens with: %s", OLProxy_KeyName(OLProxy_GetMenuKey()));
      ImGui::Text("%s", lbl);
      ImGui::SameLine();
      if (cap == 3) ImGui::TextDisabled("press any key...");
      else if (ImGui::SmallButton("Change##menukey")) OLProxy_StartBindCapture(3);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Change this if your keyboard has no INSERT key."); }

    // THE PICTURE SECTION IS GONE ON PURPOSE (2026-08-03). The brightness control blacked out
    // the headset in one small drag, and a setting that can stop you seeing the menu that would
    // undo it has no business in front of a player. It is not merely hidden here: the value can
    // no longer be reached from ANYWHERE. The menu row is gone, it is no longer read from the
    // ini, it is no longer written to the ini, and the reset hotkey is gone with it, so the
    // brightness is permanently the untouched picture the game rendered and the correction
    // shader is bypassed entirely. Cutscenes in full VR lived here too and is simply always on,
    // which is what it should have been.
    ImGui::Separator();
    // The camcorder readouts (battery, record dot, timecode) are drawn at the very edges of the
    // frame, which a headset lens cannot show. These pull them into the readable middle. Uniform
    // scale, so the display keeps its own proportions: stretching UI to fit is what makes it look
    // wrong. Only the mirrored UI viewport is touched, so nothing here reaches the 3D path.
    ImGui::TextDisabled("Camcorder display");
    { float s = 1.0f, ox = 0.0f, oy = 0.0f; OLProxy_GetHudFit(&s, &ox, &oy);
      bool ch = false;
      if (ImGui::SliderFloat("Size", &s, 0.40f, 1.00f, "%.2f")) ch = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(
          "Cannot see the battery meter or the record light? Lower this.\n"
          "It pulls everything in from the corners, where the lens cuts it off.");
      if (ImGui::SliderFloat("Move left or right", &ox, -0.40f, 0.40f, "%.2f")) ch = true;
      if (ImGui::SliderFloat("Move up or down",    &oy, -0.40f, 0.40f, "%.2f")) ch = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slide right to move it down.");
      if (ch) OLProxy_SetHudFit(s, ox, oy); }

    ImGui::Spacing();
    // One button, so nothing tuned while wearing the headset is ever lost or has to be
    // written down and typed back in afterwards.
    if (ImGui::Button("Save these settings", ImVec2(-1, 0))) {
        g_savedOk = OLProxy_SaveSettings();
        g_savedFrames = 180;   // the confirmation stays up for a few seconds
    }
    if (g_savedFrames > 0) {
        --g_savedFrames;
        ImGui::TextDisabled(g_savedOk ? "Saved. The game will start with these from now on."
                                      : "Could not save. The settings file is not writable.");
    }

    ImGui::Spacing();
    { char foot[128];
      sprintf_s(foot, "Press %s to close. The game is paused while this is open.",
                OLProxy_KeyName(OLProxy_GetMenuKey()));
      ImGui::TextDisabled("%s", foot); }
    ImGui::End();
}

// ---- draw the offscreen target into both eyes -------------------------------------------------
struct MenuVtx { float x, y, z, rhw; DWORD col; float u, v; };
static void DrawQuad(IDirect3DDevice9* dev, float x, float y, float w, float h) {
    MenuVtx v[4] = {
        { x,     y,     0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { x + w, y,     0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { x,     y + h, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { x + w, y + h, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVtx));
}

void OLMenu_Render(IDirect3DDevice9* dev, unsigned bbW, unsigned bbH) {
    if (!g_visible || !dev || bbW < 64 || bbH < 64) return;
    if (!EnsureInit(dev)) return;
    if (!g_rtTex && !EnsureRT()) return;
    ClipCursor(nullptr);

    // 1) render the ImGui frame into the offscreen target
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kRTW, (float)kRTH);
    io.DeltaTime = 1.0f / 60.0f;
    PumpInput();
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    BuildUI();
    ImGui::EndFrame();
    ImGui::Render();

    IDirect3DSurface9* oldRT = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    if (SUCCEEDED(dev->SetRenderTarget(0, g_rtSurf))) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);   // transparent surround
        if (SUCCEEDED(dev->BeginScene())) { ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); dev->EndScene(); }
    }
    if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }

    // 2) draw that target into the backbuffer, once per eye, blended (transparent surround shows game)
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTexture(0, g_rtTex);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const float rtAspect = (float)kRTW / (float)kRTH;
    const bool split = (OLUE3_StereoMode() == 1) && OLUE3_SplitActive() != 0;
    if (split) {
        const float halfW = (float)bbW * 0.5f;
        // Size the panel so it reads 4:3 ANGULARLY in the headset, and FITS inside the eye that
        // is actually shown. The submitted eye is g_eyeW x g_eyeH px spanning fovX x fovY - and
        // g_eyeW is the CONVERGENCE-CROPPED width (1728, not the 1920 half). The old formula sized
        // against the full half and then clamped, so the panel overflowed the crop, its sides got
        // cut, and the content looked squashed horizontally. A backbuffer quad of w x h px maps
        // 1:1 through the crop, so its angular aspect is
        //   (w/g_eyeW * tan(fovX/2)) / (h/bbH * tan(fovY/2));  solve that == rtAspect.
        int ew = 0, eh = 0; float fdx = 0.0f, fdy = 0.0f;
        OLXR_GetEyeInfo(&ew, &eh, &fdx, &fdy);                 // ew px, fov in DEGREES
        if (ew < 64) ew = (int)halfW;
        float fx = fdx * 0.01745329f, fy = fdy * 0.01745329f;
        if (fx < 0.5f || fx > 2.6f) fx = 1.5708f;
        if (fy < 0.5f || fy > 2.6f) fy = 1.65f;
        const float tfx = tanf(fx * 0.5f), tfy = tanf(fy * 0.5f);
        const float maxH = (float)bbH * 0.80f;                 // comfortable height ceiling
        const float maxW = (float)ew * 0.92f;                  // stay inside the shown crop
        float h = maxH;
        float w = rtAspect * (h / (float)bbH) * (tfy / tfx) * (float)ew;
        if (w > maxW) {                                        // too wide to be 4:3 at this height:
            w = maxW;                                          // fit to width, drop height to match
            h = w * (float)bbH * tfx / (rtAspect * tfy * (float)ew);
        }
        const float y = ((float)bbH - h) * 0.5f;
        DrawQuad(dev, (halfW - w) * 0.5f,         y, w, h);
        DrawQuad(dev, halfW + (halfW - w) * 0.5f, y, w, h);
    } else {
        const float h = (float)bbH * 0.78f;
        float w = h * rtAspect;
        if (w > (float)bbW * 0.6f) w = (float)bbW * 0.6f;
        DrawQuad(dev, ((float)bbW - w) * 0.5f, ((float)bbH - h) * 0.5f, w, h);
    }

    sb->Apply();
    sb->Release();
}
