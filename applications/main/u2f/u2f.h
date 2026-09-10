#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>

typedef enum {
    U2fNotifyRegister,
    U2fNotifyAuth,
    U2fNotifyAuthSuccess,
    U2fNotifyWink,
    U2fNotifyConnect,
    U2fNotifyDisconnect,
    U2fNotifyError,
} U2fNotifyEvent;

typedef struct U2fData U2fData;

typedef void (*U2fEvtCallback)(U2fNotifyEvent evt, void* context);

/* Raised from u2f_confirm_user_present(), i.e. from the GUI thread when the
 * user presses OK. CTAP1 only needed the flag u2f_confirm_user_present() sets,
 * because the host retries a "user missing" reply until it takes. CTAP2 has no
 * such retry -- the transport worker has to be woken so it can stop sending
 * KEEPALIVEs and answer. */
typedef void (*U2fPresenceCallback)(void* context);

U2fData* u2f_alloc(void);

bool u2f_init(U2fData* instance);

void u2f_free(U2fData* instance);

void u2f_set_event_callback(U2fData* instance, U2fEvtCallback callback, void* context);

void u2f_confirm_user_present(U2fData* instance);

void u2f_set_presence_callback(U2fData* instance, U2fPresenceCallback callback, void* context);

/* Accessors for the CTAP2 layer, which derives its credentials from the same
 * device key and shares one signature counter with U2F -- a FIDO2 authenticator
 * has a single counter, not one per protocol version. */
const uint8_t* u2f_get_device_key(U2fData* instance);
const uint8_t* u2f_get_cert_key(U2fData* instance);
bool u2f_is_user_present(U2fData* instance);
void u2f_clear_user_present(U2fData* instance);
struct mbedtls_ecp_group* u2f_get_group(U2fData* instance);
uint32_t u2f_get_counter(U2fData* instance);

/** Increment, persist and return the new counter value. */
uint32_t u2f_bump_counter(U2fData* instance);

/** Replace the device key with a fresh random one and zero the counter.
 *
 * This is how authenticatorReset destroys credentials: every credential this
 * device issues is WRAPPED under the device key rather than stored, so there is
 * nothing to delete -- changing the key is what makes them unopenable. Applies
 * to U2F registrations too, which share the same key. */
bool u2f_regenerate_device_key(U2fData* instance);

/** Raise one of the U2fNotifyEvent callbacks from the CTAP2 layer. */
void u2f_notify(U2fData* instance, U2fNotifyEvent evt);

uint16_t u2f_msg_parse(U2fData* instance, uint8_t* buf, uint16_t len);

void u2f_wink(U2fData* instance);

void u2f_set_state(U2fData* instance, uint8_t state);

#ifdef __cplusplus
}
#endif
