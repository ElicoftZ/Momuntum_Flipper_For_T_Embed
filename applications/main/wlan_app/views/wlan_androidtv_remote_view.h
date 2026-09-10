#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdbool.h>

// The remote-control screen: a TV-remote button grid (D-pad cross, power/home/
// back, media, volume) navigated by the encoder. Rotating moves the cursor
// (Up/Down; hold+rotate = Left/Right), OK sends the selected key. Back is left
// unhandled so the scene manager pops the scene.

typedef void (*WlanAtvKeyCallback)(void* context, int keycode);

View* wlan_androidtv_remote_view_alloc(void);
void wlan_androidtv_remote_view_free(View* view);

/** Invoked (GUI thread) with the RemoteKeyCode when the user presses OK on a
 *  button. */
void wlan_androidtv_remote_view_set_key_callback(
    View* view,
    WlanAtvKeyCallback callback,
    void* context);

/** Header label (e.g. the TV's "vendor model") and the live connection dot. */
void wlan_androidtv_remote_view_set_status(View* view, const char* title, bool connected);
