#pragma once
// In-headset settings menu for the Outlast VR probe (INSERT toggles it).
//
// Everything tunable lives here rather than in outlastvr.ini: editing an ini means taking the
// headset off, which makes tuning a thing you do blind and once instead of live and properly.
struct IDirect3DDevice9;

void OLMenu_Render(IDirect3DDevice9* dev, unsigned bbW, unsigned bbH);  // call before XR capture
void OLMenu_OnDeviceLost();                                            // BEFORE the game's Reset
void OLMenu_OnDeviceReset();                                           // AFTER a successful Reset
void OLMenu_StartInput(void* hinst);   // installs the low-level mouse hook (once, at startup)
void OLMenu_Toggle();
void OLMenu_Nav(int dy);        // -1 up, +1 down
void OLMenu_Adjust(int dir);    // -1 left, +1 right (or activate, for action rows)
int  OLMenu_Visible();
