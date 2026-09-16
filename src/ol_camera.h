#pragma once
// Outlast render-side camera census (D3D9 vertex-shader constants). Read-only unless the
// F11 ownership nudge is armed.
void OLCam_InstallHook(void* device, void* (*hookSlot)(void**, int, void*));
void OLCam_ToggleNudge();
void OLCam_OnFrame();
