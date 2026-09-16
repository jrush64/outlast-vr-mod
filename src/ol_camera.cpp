// ol_camera.cpp - Outlast camera census: which vertex-shader constant registers carry the
// view-projection matrix, and can they be driven?
// =============================================================================
// This is the D3D9-side half of Stage 3 discovery, and it needs no static analysis at all -
// which is why it runs in the same probe build as the engine-side hook. Between them, one
// test run answers both questions:
//   engine side (ol_ue3.cpp)  : where are the viewport-rect + FSceneView fields?
//   render side (this file)   : which constant registers are the camera, and can it be moved?
//
// UE3/D3D9 uploads the view-projection as 4 consecutive float4 constants via
// SetVertexShaderConstantF. A perspective VP in UE3's row-vector convention computes
// clip = worldPos * VP, so column 3 (m[3],m[7],m[11]) is the view-forward axis and is
// UNIT LENGTH. That's the signature this scan looks for.
//
// NUM7 arms the ownership nudge: scale column 0 of the top-ranked VP register, which
// squeezes horizontal FOV. If the world visibly stretches EVERY frame, camera ownership is confirmed.
// Off by default. This does not ship - it is a proof, not a feature.
// =============================================================================

#include <Windows.h>
#include <d3d9.h>
#include <cstdio>
#include <math.h>
#include "ol_camera.h"
#include "ol_xr.h"

typedef HRESULT (STDMETHODCALLTYPE *SetVSConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
static SetVSConstF_t o_SetVSConstF = nullptr;

static const int kMaxReg = 256;
static long  g_regHits[kMaxReg] = { 0 };   // 4x4 uploads seen at this start register
static long  g_regVP[kMaxReg]   = { 0 };   // ...that looked like a view-projection
static float g_lastVP[16]       = { 0 };
static int   g_primaryReg       = -1;      // best VP candidate so far
static volatile long g_nudge    = 0;       // F11
static long  g_frames           = 0;
static long  g_calls            = 0;

// A VP matrix in UE3's row-vector convention: clip.w = dot(world.xyz, col3) + m[15],
// so (m[3],m[7],m[11]) is the normalized view direction.
static bool LooksLikeVP(const float* m) {
    float wx = m[3], wy = m[7], wz = m[11];
    float l = sqrtf(wx*wx + wy*wy + wz*wz);
    if (l < 0.95f || l > 1.05f) return false;
    // reject identity / degenerate
    if (m[0] == 0.0f && m[5] == 0.0f) return false;
    // a projection scale of a sane magnitude on x and y
    float sx = sqrtf(m[0]*m[0] + m[4]*m[4] + m[8]*m[8]);
    float sy = sqrtf(m[1]*m[1] + m[5]*m[5] + m[9]*m[9]);
    return sx > 0.05f && sx < 50.0f && sy > 0.05f && sy < 50.0f;
}

static HRESULT STDMETHODCALLTYPE Hook_SetVSConstF(IDirect3DDevice9* This, UINT start, const float* data, UINT count) {
    ++g_calls;
    if (data && count >= 4 && start < (UINT)(kMaxReg - 3)) {
        if (LooksLikeVP(data)) {
            ++g_regVP[start];
            memcpy(g_lastVP, data, sizeof(g_lastVP));
            if (g_primaryReg < 0 || g_regVP[start] > g_regVP[g_primaryReg]) g_primaryReg = (int)start;
        }
        ++g_regHits[start];

        // ---- ownership nudge (NUM7): squeeze horizontal FOV on the primary VP register ----
        // OSCILLATING, not static. A one-shot change can be confused with a game-side reaction
        // to the keypress (that's what F11-as-fullscreen did). A continuous ~1.5s breathing
        // zoom can only be this hook, and it proves these pixels are reached EVERY frame, not just once.
        if (g_nudge && (int)start == g_primaryReg && LooksLikeVP(data)) {
            float mod[64];
            UINT n = count > 16 ? 16 : count;          // only the 4 rows this handles
            memcpy(mod, data, n * 4 * sizeof(float));
            float phase = (float)(GetTickCount() % 1500) / 1500.0f;
            float k = 0.55f + 0.45f * (0.5f - 0.5f * cosf(phase * 6.2831853f));  // 0.55 <-> 1.0
            mod[0] *= k; mod[4] *= k; mod[8] *= k; mod[12] *= k;
            HRESULT hr = o_SetVSConstF(This, start, mod, n);
            if (count > n) o_SetVSConstF(This, start + n, data + n * 4, count - n);
            return hr;
        }
    }
    return o_SetVSConstF(This, start, data, count);
}

void OLCam_InstallHook(void* devVtblSlot94Owner, void* (*hookSlot)(void**, int, void*)) {
    void** vt = *(void***)devVtblSlot94Owner;
    o_SetVSConstF = (SetVSConstF_t)hookSlot(vt, 94, (void*)&Hook_SetVSConstF);
    OLProxyLog("[OLVR][CAM] SetVertexShaderConstantF hook installed (census on, nudge off - NUM7)");
}

void OLCam_ToggleNudge() {
    g_nudge = g_nudge ? 0 : 1;
    char line[160];
    sprintf_s(line, "[OLVR][CAM] ownership nudge %s (reg c%d)", g_nudge ? "ON - world should pulse/breathe horizontally" : "off", g_primaryReg);
    OLProxyLog(line);
}

// Called once per Present. Logs a census every ~5 seconds of gameplay.
void OLCam_OnFrame() {
    if ((++g_frames % 300) != 0) return;
    char line[512];
    sprintf_s(line, "[OLVR][CAM] census frames=%ld setVSConstF calls=%ld primaryVPreg=c%d", g_frames, g_calls, g_primaryReg);
    OLProxyLog(line);
    int shown = 0;
    for (int r = 0; r < kMaxReg && shown < 8; ++r) {
        if (g_regVP[r] > 0) {
            sprintf_s(line, "[OLVR][CAM]   c%-3d  VP-shaped=%ld  total4x4=%ld", r, g_regVP[r], g_regHits[r]);
            OLProxyLog(line);
            ++shown;
        }
    }
    if (g_primaryReg >= 0) {
        const float* m = g_lastVP;
        sprintf_s(line, "[OLVR][CAM]   last VP fwd=(%.3f,%.3f,%.3f) trans=(%.1f,%.1f,%.1f,%.1f)",
                  m[3], m[7], m[11], m[12], m[13], m[14], m[15]);
        OLProxyLog(line);
    } else {
        OLProxyLog("[OLVR][CAM]   no VP-shaped 4x4 upload seen yet");
    }
}
