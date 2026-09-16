#pragma once
// Outlast FlatXR: D3D9 -> (CPU readback) -> D3D11 -> OpenXR.
struct IDirect3DDevice9;

void OLXR_OnPresent(IDirect3DDevice9* dev);   // capture backbuffer + submit to the headset
void OLXR_Shutdown();
void OLXR_Recenter();                          // zero the head-tracking baseline
void OLXR_ToggleProjection();                  // quad <-> projection layer

// Put the picture back to untouched. Reachable from a hotkey because a brightness setting can
// leave you unable to READ the control that would undo it.
extern "C" void OLXR_ResetGamma();

extern "C" void OLProxyLog(const char* line);  // implemented in d3d9_proxy.cpp

// Head pose relative to the recenter baseline. Written by ol_xr.cpp, read by the (future)
// camera bake in d3d9_proxy.cpp. Nothing consumes these yet - Stage 2 is flat.
extern volatile float g_olYawRad;
extern volatile float g_olPitchRad;
extern volatile float g_olRollRad;
extern volatile float g_olHeadX;               // meters, +right
extern volatile float g_olHeadY;               // meters, +up
extern volatile float g_olHeadZ;               // meters, +back
extern volatile long  g_olPoseValid;

// Declared FOV for the projection layer. 0 = use the fallback default until the camera lane
// gives the game's REAL rendered FOV. Never invent a number here once that exists -
// declared FOV must match what was rendered.
extern volatile float g_olDeclFovXDeg;
extern volatile float g_olDeclFovYDeg;
extern volatile long  g_olUseProjection;       // 1 = projection layer, 0 = head-locked quad

// ---- [XRINPUT] motion controls ----------------------------------------------------------------
// The VR controllers, read by the submit worker and published once per frame. d3d9_proxy.cpp
// turns this into the XInput state the game polls (step 1 proved it reads a fabricated pad).
// Sticks are -1..1, triggers/grips 0..1. Hand poses are in the recenter frame, metres, and are
// only filled when the runtime says the pose is tracked.
struct OLXrPose { float qx, qy, qz, qw; float px, py, pz; };
struct OLXrPad {
    float    moveX, moveY;     // left stick  (+X right, +Y forward)
    float    lookX, lookY;     // right stick
    float    trigL, trigR;
    float    gripL, gripR;
    unsigned buttons;          // OLXR_BTN_*
    OLXrPose handPose[2];      // [0] left, [1] right
    unsigned handValid;        // bit 0 = left, bit 1 = right
};
enum {
    OLXR_BTN_A = 1u << 0, OLXR_BTN_B = 1u << 1, OLXR_BTN_X = 1u << 2, OLXR_BTN_Y = 1u << 3,
    OLXR_BTN_LSTICK = 1u << 4, OLXR_BTN_RSTICK = 1u << 5, OLXR_BTN_MENU = 1u << 6,
};
extern "C" int OLXR_MotionControlsReady();
extern "C" int OLXR_GetPad(OLXrPad* out);      // 1 = out filled with a coherent one-frame snapshot
