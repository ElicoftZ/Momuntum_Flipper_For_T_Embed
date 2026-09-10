#include <furi.h>
#include "u2f_hid.h"
#include "u2f.h"
#include "ctap2.h"
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <lib/toolbox/args.h>
#include <furi_hal_usb_hid_u2f.h>
#include <storage/storage.h>

#define TAG "U2fHid"

#define WORKER_TAG TAG "Worker"

#define U2F_HID_MAX_PAYLOAD_LEN ((HID_U2F_PACKET_LEN - 7) + 128 * (HID_U2F_PACKET_LEN - 5))

#define U2F_HID_TYPE_MASK 0x80 // Frame type mask
#define U2F_HID_TYPE_INIT 0x80 // Initial frame identifier
#define U2F_HID_TYPE_CONT 0x00 // Continuation frame identifier

#define U2F_HID_PING  (U2F_HID_TYPE_INIT | 0x01) // Echo data through local processor only
#define U2F_HID_MSG   (U2F_HID_TYPE_INIT | 0x03) // Send U2F message frame
#define U2F_HID_LOCK  (U2F_HID_TYPE_INIT | 0x04) // Send lock channel command
#define U2F_HID_INIT  (U2F_HID_TYPE_INIT | 0x06) // Channel initialization
#define U2F_HID_WINK  (U2F_HID_TYPE_INIT | 0x08) // Send device identification wink
#define U2F_HID_ERROR (U2F_HID_TYPE_INIT | 0x3f) // Error response

/* CTAP2 additions to U2FHID. */
#define U2F_HID_CBOR      (U2F_HID_TYPE_INIT | 0x10) // CTAP2 CBOR message
#define U2F_HID_CANCEL    (U2F_HID_TYPE_INIT | 0x11) // Abort the pending ceremony
#define U2F_HID_KEEPALIVE (U2F_HID_TYPE_INIT | 0x3b) // "still waiting", sent by us

#define U2F_HID_STATUS_PROCESSING 0x01
#define U2F_HID_STATUS_UPNEEDED   0x02

/* Capability bits in the INIT reply. Without CBOR (0x04) no host will ever
 * attempt CTAP2, which is exactly why this device was U2F-only until now. */
#define U2F_HID_CAPABILITY_WINK 0x01
#define U2F_HID_CAPABILITY_CBOR 0x04

/* CTAP2 has no host-side retry: if the authenticator goes quiet while waiting
 * for the button, the client gives up. So it must be kept informed. The spec
 * allows up to 100 ms between KEEPALIVEs. */
#define U2F_HID_KEEPALIVE_INTERVAL_MS 100U
#define U2F_HID_USER_PRESENCE_TIMEOUT_MS 30000U

#define U2F_HID_ERR_NONE          0x00 // No error
#define U2F_HID_ERR_INVALID_CMD   0x01 // Invalid command
#define U2F_HID_ERR_INVALID_PAR   0x02 // Invalid parameter
#define U2F_HID_ERR_INVALID_LEN   0x03 // Invalid message length
#define U2F_HID_ERR_INVALID_SEQ   0x04 // Invalid message sequencing
#define U2F_HID_ERR_MSG_TIMEOUT   0x05 // Message has timed out
#define U2F_HID_ERR_CHANNEL_BUSY  0x06 // Channel busy
#define U2F_HID_ERR_LOCK_REQUIRED 0x0a // Command requires channel lock
#define U2F_HID_ERR_SYNC_FAIL     0x0b // SYNC command failed
#define U2F_HID_ERR_OTHER         0x7f // Other unspecified error

#define U2F_HID_BROADCAST_CID 0xFFFFFFFF

typedef enum {
    WorkerEvtReserved = (1 << 0),
    WorkerEvtStop = (1 << 1),
    WorkerEvtConnect = (1 << 2),
    WorkerEvtDisconnect = (1 << 3),
    WorkerEvtRequest = (1 << 4),
    WorkerEvtUnlock = (1 << 5),
    WorkerEvtUserPresent = (1 << 6),
    WorkerEvtCancel = (1 << 7),
} WorkerEvtFlags;

struct U2fHid_packet {
    uint32_t cid;
    uint16_t len;
    uint8_t cmd;
    uint8_t payload[U2F_HID_MAX_PAYLOAD_LEN];
};

struct U2fHid {
    FuriThread* thread;
    FuriTimer* lock_timer;
    uint8_t seq_id_last;
    uint16_t req_buf_ptr;
    uint32_t req_len_left;
    uint32_t lock_cid;
    bool lock;
    U2fData* u2f_instance;
    Ctap2* ctap2;
    bool ceremony_active;
    struct U2fHid_packet packet;
    /* Separate from packet.payload on purpose: ctap2_request hands out
     * zero-copy pointers into the request while it builds the response, so the
     * two cannot share a buffer. */
    uint8_t ctap_resp[CTAP2_MAX_MSG_SIZE];
};

static void u2f_hid_event_callback(HidU2fEvent ev, void* context) {
    furi_assert(context);
    U2fHid* u2f_hid = context;

    if(ev == HidU2fDisconnected)
        furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtDisconnect);
    else if(ev == HidU2fConnected)
        furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtConnect);
    else if(ev == HidU2fRequest)
        furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtRequest);
}

/* Raised on the GUI thread when the user presses OK. */
static void u2f_hid_user_present_callback(void* context) {
    furi_assert(context);
    U2fHid* u2f_hid = context;
    furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtUserPresent);
}

static void u2f_hid_lock_timeout_callback(void* context) {
    furi_assert(context);
    U2fHid* u2f_hid = context;

    furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtUnlock);
}

static void u2f_hid_send_response(U2fHid* u2f_hid) {
    uint8_t packet_buf[HID_U2F_PACKET_LEN];
    uint16_t len_remain = u2f_hid->packet.len;
    uint8_t len_cur = 0;
    uint8_t seq_cnt = 0;
    uint16_t data_ptr = 0;

    memset(packet_buf, 0, HID_U2F_PACKET_LEN);
    memcpy(packet_buf, &(u2f_hid->packet.cid), sizeof(uint32_t)); //-V1086

    // Init packet
    packet_buf[4] = u2f_hid->packet.cmd;
    packet_buf[5] = u2f_hid->packet.len >> 8;
    packet_buf[6] = (u2f_hid->packet.len & 0xFF);
    len_cur = (len_remain < (HID_U2F_PACKET_LEN - 7)) ? (len_remain) : (HID_U2F_PACKET_LEN - 7);
    if(len_cur > 0) memcpy(&packet_buf[7], u2f_hid->packet.payload, len_cur);
    furi_hal_hid_u2f_send_response(packet_buf, HID_U2F_PACKET_LEN);
    data_ptr = len_cur;
    len_remain -= len_cur;

    // Continuation packets
    while(len_remain > 0) {
        memset(&packet_buf[4], 0, HID_U2F_PACKET_LEN - 4);
        packet_buf[4] = seq_cnt;
        len_cur = (len_remain < (HID_U2F_PACKET_LEN - 5)) ? (len_remain) :
                                                            (HID_U2F_PACKET_LEN - 5);
        memcpy(&packet_buf[5], &(u2f_hid->packet.payload[data_ptr]), len_cur);
        furi_hal_hid_u2f_send_response(packet_buf, HID_U2F_PACKET_LEN);
        seq_cnt++;
        len_remain -= len_cur;
        data_ptr += len_cur;
    }
}

/* Emit one init frame directly, without disturbing the in-flight request in
 * u2f_hid->packet. Needed because both users of this -- KEEPALIVE and the BUSY
 * reply to an interloping channel -- happen in the MIDDLE of a ceremony, when
 * packet still holds the request being served. */
static void
    u2f_hid_send_simple_frame(uint32_t cid, uint8_t cmd, const uint8_t* payload, uint8_t len) {
    uint8_t frame[HID_U2F_PACKET_LEN];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, &cid, sizeof(uint32_t)); //-V1086
    frame[4] = cmd;
    frame[5] = 0;
    frame[6] = len;
    if(len > 0) memcpy(&frame[7], payload, len);
    furi_hal_hid_u2f_send_response(frame, HID_U2F_PACKET_LEN);
}

static void u2f_hid_send_keepalive(U2fHid* u2f_hid, uint8_t status) {
    u2f_hid_send_simple_frame(u2f_hid->packet.cid, U2F_HID_KEEPALIVE, &status, 1);
}

/* Drain frames that arrive while a ceremony is blocking the worker.
 *
 * Only two things can legitimately show up: a CANCEL for the channel being
 * served, and traffic from some other channel. Everything else is dropped --
 * a client is not allowed to pipeline a second request onto a busy channel,
 * and buffering one would mean answering it with the wrong ceremony's consent.
 *
 * Returns true if the active channel asked to cancel. */
static bool u2f_hid_ceremony_drain(U2fHid* u2f_hid) {
    uint8_t frame[HID_U2F_PACKET_LEN];
    bool cancelled = false;

    while(true) {
        uint32_t len = furi_hal_hid_u2f_get_request(frame);
        if(len == 0) break;
        if(len < 7) continue;
        /* Continuation frames belong to a request we are not assembling. */
        if((frame[4] & U2F_HID_TYPE_MASK) != U2F_HID_TYPE_INIT) continue;

        uint32_t cid = 0;
        memcpy(&cid, frame, 4);

        if(cid == u2f_hid->packet.cid) {
            if(frame[4] == U2F_HID_CANCEL) cancelled = true;
        } else {
            uint8_t busy = U2F_HID_ERR_CHANNEL_BUSY;
            u2f_hid_send_simple_frame(cid, U2F_HID_ERROR, &busy, 1);
        }
    }

    return cancelled;
}

/* Block until the user presses OK, the host cancels, or we give up.
 *
 * This is the structural difference between CTAP1 and CTAP2 on this device.
 * The U2F path answers "user missing" immediately and lets the host re-ask
 * every couple of hundred milliseconds; CTAP2 expects the authenticator to
 * hold the request open and keep saying so. */
static Ctap2PresenceResult u2f_hid_wait_for_user(void* context, U2fNotifyEvent prompt) {
    U2fHid* u2f_hid = context;

    /* A press left over from a previous ceremony must not satisfy this one. */
    u2f_clear_user_present(u2f_hid->u2f_instance);
    furi_thread_flags_clear(WorkerEvtUserPresent | WorkerEvtCancel);

    u2f_hid->ceremony_active = true;
    u2f_notify(u2f_hid->u2f_instance, prompt);

    Ctap2PresenceResult result = Ctap2PresenceTimeout;
    uint32_t waited_ms = 0;

    while(waited_ms < U2F_HID_USER_PRESENCE_TIMEOUT_MS) {
        uint32_t flags = furi_thread_flags_wait(
            WorkerEvtUserPresent | WorkerEvtCancel | WorkerEvtStop | WorkerEvtRequest |
                WorkerEvtDisconnect,
            FuriFlagWaitAny,
            U2F_HID_KEEPALIVE_INTERVAL_MS);

        if(!(flags & FuriFlagError)) {
            if(flags & WorkerEvtStop) {
                /* Put it back so the worker loop still sees it and exits. */
                furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtStop);
                result = Ctap2PresenceCancelled;
                break;
            }
            if(flags & WorkerEvtDisconnect) {
                furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtDisconnect);
                result = Ctap2PresenceCancelled;
                break;
            }
            if(flags & WorkerEvtCancel) {
                result = Ctap2PresenceCancelled;
                break;
            }
            if(flags & WorkerEvtUserPresent) {
                result = Ctap2PresenceGranted;
                break;
            }
            if((flags & WorkerEvtRequest) && u2f_hid_ceremony_drain(u2f_hid)) {
                result = Ctap2PresenceCancelled;
                break;
            }
        }

        u2f_hid_send_keepalive(u2f_hid, U2F_HID_STATUS_UPNEEDED);

        /* Re-raise the prompt as well as the wire keepalive.
         *
         * u2f_scene_main arms a 500 ms timer on every prompt and drops the
         * view back to "Connected!" when it expires. Under CTAP1 that is
         * invisible, because the host re-asks every couple of hundred
         * milliseconds and each retry restarts the timer. CTAP2 asks exactly
         * once and then waits up to 30 s, so without this the "Press OK"
         * prompt would vanish half a second in and the user would be staring
         * at an idle screen while the ceremony silently timed out. */
        u2f_notify(u2f_hid->u2f_instance, prompt);

        waited_ms += U2F_HID_KEEPALIVE_INTERVAL_MS;
    }

    u2f_hid->ceremony_active = false;
    u2f_clear_user_present(u2f_hid->u2f_instance);
    return result;
}

static void u2f_hid_send_error(U2fHid* u2f_hid, uint8_t error) {
    u2f_hid->packet.len = 1;
    u2f_hid->packet.cmd = U2F_HID_ERROR;
    u2f_hid->packet.payload[0] = error;
    u2f_hid_send_response(u2f_hid);
}

static bool u2f_hid_parse_request(U2fHid* u2f_hid) {
    FURI_LOG_D(
        WORKER_TAG,
        "Req cid=%lX cmd=%x len=%u",
        u2f_hid->packet.cid,
        u2f_hid->packet.cmd,
        u2f_hid->packet.len);

    if(u2f_hid->packet.cmd == U2F_HID_PING) { // PING - echo request back
        u2f_hid_send_response(u2f_hid);

    } else if(u2f_hid->packet.cmd == U2F_HID_MSG) { // MSG - U2F message
        if((u2f_hid->lock == true) && (u2f_hid->packet.cid != u2f_hid->lock_cid)) return false;
        uint16_t resp_len =
            u2f_msg_parse(u2f_hid->u2f_instance, u2f_hid->packet.payload, u2f_hid->packet.len);
        if(resp_len > 0) {
            u2f_hid->packet.len = resp_len;
            u2f_hid_send_response(u2f_hid);
        } else
            return false;

    } else if(u2f_hid->packet.cmd == U2F_HID_LOCK) { // LOCK - lock all channels except current
        if(u2f_hid->packet.len != 1) return false;
        uint8_t lock_timeout = u2f_hid->packet.payload[0];
        if(lock_timeout == 0) { // Lock off
            u2f_hid->lock = false;
            u2f_hid->lock_cid = 0;
        } else { // Lock on
            u2f_hid->lock = true;
            u2f_hid->lock_cid = u2f_hid->packet.cid;
            furi_timer_start(u2f_hid->lock_timer, lock_timeout * 1000);
        }

    } else if(u2f_hid->packet.cmd == U2F_HID_CBOR) { // CBOR - CTAP2 message
        if((u2f_hid->lock == true) && (u2f_hid->packet.cid != u2f_hid->lock_cid)) return false;
        if(u2f_hid->packet.len < 1) return false;

        if(u2f_hid->packet.len > CTAP2_MAX_MSG_SIZE) {
            /* Answer in CTAP2's own error vocabulary rather than a U2FHID
             * error: the client is mid-CBOR-exchange and expects a status
             * byte back, not a transport-level complaint. */
            u2f_hid->packet.payload[0] = CTAP2_ERR_REQUEST_TOO_LARGE;
            u2f_hid->packet.len = 1;
            u2f_hid_send_response(u2f_hid);
            return true;
        }

        size_t resp_len = ctap2_request(
            u2f_hid->ctap2,
            u2f_hid->packet.payload,
            u2f_hid->packet.len,
            u2f_hid->ctap_resp,
            sizeof(u2f_hid->ctap_resp));

        memcpy(u2f_hid->packet.payload, u2f_hid->ctap_resp, resp_len);
        u2f_hid->packet.len = (uint16_t)resp_len;
        u2f_hid_send_response(u2f_hid);

    } else if(u2f_hid->packet.cmd == U2F_HID_CANCEL) { // CANCEL - abort pending ceremony
        /* A ceremony in progress is cancelled inside u2f_hid_wait_for_user,
         * which drains frames itself. Reaching here means nothing was pending,
         * and the spec says a stray CANCEL is simply not answered. */

    } else if(u2f_hid->packet.cmd == U2F_HID_INIT) { // INIT - channel initialization request
        if((u2f_hid->packet.len != 8) || (u2f_hid->packet.cid != U2F_HID_BROADCAST_CID) ||
           (u2f_hid->lock == true))
            return false;
        u2f_hid->packet.len = 17;
        uint32_t random_cid = furi_hal_random_get();
        memcpy(&(u2f_hid->packet.payload[8]), &random_cid, sizeof(uint32_t)); //-V1086
        u2f_hid->packet.payload[12] = 2; // Protocol version
        u2f_hid->packet.payload[13] = 1; // Device version major
        u2f_hid->packet.payload[14] = 0; // Device version minor
        u2f_hid->packet.payload[15] = 1; // Device build version
        u2f_hid->packet.payload[16] =
            U2F_HID_CAPABILITY_WINK | U2F_HID_CAPABILITY_CBOR; // Capabilities
        u2f_hid_send_response(u2f_hid);

    } else if(u2f_hid->packet.cmd == U2F_HID_WINK) { // WINK - notify user
        if(u2f_hid->packet.len != 0) return false;
        u2f_wink(u2f_hid->u2f_instance);
        u2f_hid->packet.len = 0; //-V1048
        u2f_hid_send_response(u2f_hid);
    } else
        return false;
    return true;
}

static int32_t u2f_hid_worker(void* context) {
    U2fHid* u2f_hid = context;
    uint8_t packet_buf[HID_U2F_PACKET_LEN];

    FURI_LOG_D(WORKER_TAG, "Init");

    FuriHalUsbInterface* usb_mode_prev = furi_hal_usb_get_config();
    if(!furi_hal_usb_set_config(&usb_hid_u2f, NULL)) {
        /* Reachable, so it must not panic: the USB composite is one-shot per boot,
         * so if USB storage or the qFlipper bridge brought USB up first there is no
         * FIDO interface until a reboot. */
        FURI_LOG_E(WORKER_TAG, "No U2F USB interface available, reboot needed");
        return 0;
    }

    u2f_hid->lock_timer =
        furi_timer_alloc(u2f_hid_lock_timeout_callback, FuriTimerTypeOnce, u2f_hid);

    u2f_hid->ctap2 = ctap2_alloc(u2f_hid->u2f_instance);
    ctap2_set_presence_callback(u2f_hid->ctap2, u2f_hid_wait_for_user, u2f_hid);
    u2f_set_presence_callback(u2f_hid->u2f_instance, u2f_hid_user_present_callback, u2f_hid);

    furi_hal_hid_u2f_set_callback(u2f_hid_event_callback, u2f_hid);

    while(1) {
        uint32_t flags = furi_thread_flags_wait(
            WorkerEvtStop | WorkerEvtConnect | WorkerEvtDisconnect | WorkerEvtRequest |
                WorkerEvtUserPresent | WorkerEvtCancel,
            FuriFlagWaitAny,
            FuriWaitForever);
        furi_check(!(flags & FuriFlagError));
        if(flags & WorkerEvtStop) break;
        if(flags & WorkerEvtConnect) {
            u2f_set_state(u2f_hid->u2f_instance, 1);
            FURI_LOG_D(WORKER_TAG, "Connect");
        }
        if(flags & WorkerEvtDisconnect) {
            u2f_set_state(u2f_hid->u2f_instance, 0);
            FURI_LOG_D(WORKER_TAG, "Disconnect");
        }
        if(flags & WorkerEvtRequest) {
            /* Continuation frames land back-to-back, faster than a thread flag
             * can be raised more than once, so drain the queue rather than
             * taking a single frame per event. */
            while(true) {
                uint32_t len_cur = furi_hal_hid_u2f_get_request(packet_buf);
                if(len_cur == 0) {
                    break;
                }
                do {
                    if((packet_buf[4] & U2F_HID_TYPE_MASK) == U2F_HID_TYPE_INIT) {
                        if(len_cur < 7) {
                            u2f_hid->req_len_left = 0;
                            break; // Wrong chunk len
                        }
                        // Init packet
                        u2f_hid->packet.len = (packet_buf[5] << 8) | (packet_buf[6]);
                        if(u2f_hid->packet.len > U2F_HID_MAX_PAYLOAD_LEN) {
                            u2f_hid->req_len_left = 0;
                            break; // Wrong packet len
                        }
                        if(u2f_hid->packet.len > (len_cur - 7)) {
                            u2f_hid->req_len_left = u2f_hid->packet.len - (len_cur - 7);
                            len_cur = len_cur - 7;
                        } else {
                            u2f_hid->req_len_left = 0;
                            len_cur = u2f_hid->packet.len;
                        }
                        memcpy(&(u2f_hid->packet.cid), packet_buf, 4);
                        u2f_hid->packet.cmd = packet_buf[4];
                        u2f_hid->seq_id_last = 0;
                        u2f_hid->req_buf_ptr = len_cur;
                        if(len_cur > 0) memcpy(u2f_hid->packet.payload, &packet_buf[7], len_cur);
                    } else {
                        if(len_cur < 5) {
                            u2f_hid->req_len_left = 0;
                            break; // Wrong chunk len
                        }
                        // Continuation packet
                        if(u2f_hid->req_len_left > 0) {
                            uint32_t cid_temp = 0;
                            memcpy(&cid_temp, packet_buf, 4);
                            uint8_t seq_temp = packet_buf[4];
                            if((cid_temp == u2f_hid->packet.cid) &&
                               (seq_temp == u2f_hid->seq_id_last)) {
                                if(u2f_hid->req_len_left > (len_cur - 5)) {
                                    len_cur = len_cur - 5;
                                    u2f_hid->req_len_left -= len_cur;
                                } else {
                                    len_cur = u2f_hid->req_len_left;
                                    u2f_hid->req_len_left = 0;
                                }
                                memcpy(
                                    &(u2f_hid->packet.payload[u2f_hid->req_buf_ptr]),
                                    &packet_buf[5],
                                    len_cur);
                                u2f_hid->req_buf_ptr += len_cur;
                                u2f_hid->seq_id_last++;
                            }
                        }
                    }
                    if(u2f_hid->req_len_left == 0) {
                        if(u2f_hid_parse_request(u2f_hid) == false) {
                            u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_CMD);
                        }
                    }
                } while(0);
            }
        }
        if(flags & WorkerEvtUnlock) {
            u2f_hid->lock = false;
            u2f_hid->lock_cid = 0;
        }
    }
    furi_timer_stop(u2f_hid->lock_timer);
    furi_timer_free(u2f_hid->lock_timer);

    /* Drop the callback into this thread before the thread goes away. */
    u2f_set_presence_callback(u2f_hid->u2f_instance, NULL, NULL);
    ctap2_free(u2f_hid->ctap2);
    u2f_hid->ctap2 = NULL;

    furi_hal_hid_u2f_set_callback(NULL, NULL);
    furi_hal_usb_set_config(usb_mode_prev, NULL);
    FURI_LOG_D(WORKER_TAG, "End");

    return 0;
}

U2fHid* u2f_hid_start(U2fData* u2f_inst) {
    /* calloc: the struct grew fields (ctap2, ceremony_active) that are read
     * before anything assigns them. */
    U2fHid* u2f_hid = calloc(1, sizeof(U2fHid));

    u2f_hid->u2f_instance = u2f_inst;

    /* 2048 was enough for CTAP1, whose deepest call was one ECDSA sign over a
     * pre-computed hash. CTAP2 adds CBOR assembly and, in Phase 2, ECDH key
     * agreement -- mbedtls' bignum temporaries alone will not fit in 2 KB. */
    u2f_hid->thread = furi_thread_alloc_ex("U2fHidWorker", 8192, u2f_hid_worker, u2f_hid);
    furi_thread_start(u2f_hid->thread);
    return u2f_hid;
}

void u2f_hid_stop(U2fHid* u2f_hid) {
    furi_assert(u2f_hid);
    furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtStop);
    furi_thread_join(u2f_hid->thread);
    furi_thread_free(u2f_hid->thread);
    free(u2f_hid);
}
