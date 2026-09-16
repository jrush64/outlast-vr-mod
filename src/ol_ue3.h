#pragma once
// Outlast Stage 3: UE3 engine-side hook (CalcSceneView) + live offset discovery.
// Safe to call unconditionally: no-ops unless outlastvr.ini sets a CalcSceneViewRva.
void OLUE3_Init();
void OLUE3_ToggleSplit();
void OLUE3_ToggleAer();        // NUM2 - one-view-per-frame mode (A/B against the split)      // NUM4 - runtime on/off for the same-frame split
void OLUE3_AdjustEye(float d); // NUM1 / NUM3 - live IPD tuning (world units)
void OLUE3_ToggleBuildOrder(); // NUM6 - which eye is the PRIMARY view (diagnostic)
void OLUE3_SwapEyes();         // NUM5 - flip which side gets which eye
void OLUE3_ToggleOcclForce();  // NUM0 - bypass Outlast's per-object occlusion tracker (flicker A/B)
void OLUE3_ToggleHeadTrack();  // NUM. - head tracking on/off (A/B against gamepad-only)
void OLUE3_ToggleHeadPos();    // NUM/ - positional (lean/duck) on/off; OFF = camera stays put
void OLUE3_AdjustWorldScale(float mul);  // NUM+ / NUM- - uu per metre; scales head motion
