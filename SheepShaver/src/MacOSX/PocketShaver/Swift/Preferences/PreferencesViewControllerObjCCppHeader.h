//
//  PreferencesViewControllerObjCCppHeader.h
//  SheepShaver_Xcode8
//
//  Created by Carl Björkman on 2025-08-24.
//

void objc_displayPreferencesStartup(void);
void objc_displayPreferencesDuringEmulationOnMain(void);

// Mac Catalyst: resize the emulation window to the guest resolution (windowed only; a no-op
// in full screen or off Catalyst). Called from the SDL video driver on guest mode changes.
void objc_resize_catalyst_window_for_guest(int guest_w, int guest_h);

#ifdef __cplusplus
extern "C" {
#endif
void objc_set_catalyst_fullscreen(bool enable);
bool catalyst_is_window_fullscreen(void);
#ifdef __cplusplus
}
#endif
// SDL-GPU: apply the catalystfullscreen pref after the compositor is up.
void objc_apply_catalyst_emulation_fullscreen(void);
