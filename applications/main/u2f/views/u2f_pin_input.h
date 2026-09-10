#pragma once

/* Wheel-driven numeric PIN entry.
 *
 * This board has Up, Down, OK and Back and nothing else -- the existing
 * number_input and byte_input modules both move their cursor with Left/Right,
 * so neither is reachable here. Rather than invent a second gesture vocabulary
 * (long-press to confirm, and so on), this is text_input collapsed to one
 * dimension: a single strip of keys the wheel walks, with real Delete and Save
 * keys on it. Every action is a short press.
 */

#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct U2fPinInput U2fPinInput;

/** Called when Save is pressed with a long-enough PIN. */
typedef void (*U2fPinInputDoneCallback)(const char* pin, void* context);

/** Called on Back. */
typedef void (*U2fPinInputBackCallback)(void* context);

U2fPinInput* u2f_pin_input_alloc(void);
void u2f_pin_input_free(U2fPinInput* instance);
View* u2f_pin_input_get_view(U2fPinInput* instance);

void u2f_pin_input_set_callbacks(
    U2fPinInput* instance,
    U2fPinInputDoneCallback done_callback,
    U2fPinInputBackCallback back_callback,
    void* context);

/** Clear the entry and set the prompt. Call on every scene entry -- a PIN left
 * in the model from a previous step must never carry into the next one. */
void u2f_pin_input_reset(U2fPinInput* instance, const char* header);

/** Show a one-line complaint above the keys ("PIN did not match", "Wrong
 * PIN"), without clearing the prompt. */
void u2f_pin_input_set_error(U2fPinInput* instance, const char* error);

#ifdef __cplusplus
}
#endif
