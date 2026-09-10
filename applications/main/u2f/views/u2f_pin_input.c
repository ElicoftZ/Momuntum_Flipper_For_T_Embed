#include "u2f_pin_input.h"
#include "../ctap2_pin.h"

#include <string.h>

#include <furi.h>
#include <gui/elements.h>
#include <input/input.h>

/* Keys 0..9 are digits, then Delete, then Save. The wheel walks all twelve in
 * one axis, which is the same linear-traversal fix this port applies to every
 * other grid UI on this board. */
#define PIN_KEY_DIGIT_COUNT 10
#define PIN_KEY_DELETE      10
#define PIN_KEY_SAVE        11
#define PIN_KEY_COUNT       12

#define DIGIT_CELL_W 12
#define DIGIT_ROW_X  4
#define DIGIT_ROW_Y  30
#define DIGIT_ROW_H  13

#define BUTTON_ROW_Y 46
#define BUTTON_ROW_H 15
#define BUTTON_DELETE_X 4
#define BUTTON_SAVE_X   66
#define BUTTON_W        58

struct U2fPinInput {
    View* view;
    U2fPinInputDoneCallback done_callback;
    U2fPinInputBackCallback back_callback;
    void* context;
};

typedef struct {
    char pin[CTAP2_PIN_LOCAL_MAX_LEN + 1];
    uint8_t len;
    uint8_t focus;
    const char* header;
    const char* error;
} U2fPinInputModel;

static void pin_input_draw_key(
    Canvas* canvas,
    uint8_t x,
    uint8_t y,
    uint8_t w,
    uint8_t h,
    const char* label,
    bool focused,
    bool enabled) {
    if(focused) {
        canvas_draw_box(canvas, x, y, w, h);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_frame(canvas, x, y, w, h);
    }

    /* A disabled Save is drawn as a dotted label rather than simply refusing
     * the press: the length rule has to be visible, or a short PIN looks like
     * a broken button. */
    if(!enabled && !focused) {
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_draw_str_aligned(
        canvas, (uint8_t)(x + w / 2), (uint8_t)(y + h / 2), AlignCenter, AlignCenter, label);
    canvas_set_color(canvas, ColorBlack);
}

static void u2f_pin_input_draw_callback(Canvas* canvas, void* _model) {
    U2fPinInputModel* model = _model;

    canvas_set_font(canvas, FontSecondary);

    if(model->header != NULL) {
        canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, model->header);
    }

    /* Masked entry, with an underscore marking where the next digit lands. */
    char masked[CTAP2_PIN_LOCAL_MAX_LEN + 2];
    size_t i = 0;
    for(; i < model->len; i++) masked[i] = '*';
    if(model->len < CTAP2_PIN_LOCAL_MAX_LEN) masked[i++] = '_';
    masked[i] = '\0';

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 13, AlignCenter, AlignTop, masked);
    canvas_set_font(canvas, FontSecondary);

    if(model->error != NULL) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignTop, model->error);
    }

    for(uint8_t key = 0; key < PIN_KEY_DIGIT_COUNT; key++) {
        char label[2] = {(char)('0' + key), '\0'};
        pin_input_draw_key(
            canvas,
            (uint8_t)(DIGIT_ROW_X + key * DIGIT_CELL_W),
            DIGIT_ROW_Y,
            DIGIT_CELL_W,
            DIGIT_ROW_H,
            label,
            model->focus == key,
            true);
    }

    pin_input_draw_key(
        canvas,
        BUTTON_DELETE_X,
        BUTTON_ROW_Y,
        BUTTON_W,
        BUTTON_ROW_H,
        "Delete",
        model->focus == PIN_KEY_DELETE,
        model->len > 0);

    pin_input_draw_key(
        canvas,
        BUTTON_SAVE_X,
        BUTTON_ROW_Y,
        BUTTON_W,
        BUTTON_ROW_H,
        (model->len >= CTAP2_PIN_MIN_LEN) ? "Save" : "Save (4+)",
        model->focus == PIN_KEY_SAVE,
        model->len >= CTAP2_PIN_MIN_LEN);
}

static bool u2f_pin_input_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    U2fPinInput* instance = context;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyBack) {
        /* Only a short press, so the side key's long-press stays free. */
        if(event->type != InputTypeShort) return false;
        if(instance->back_callback != NULL) instance->back_callback(instance->context);
        return true;
    }

    bool commit = false;
    char committed[CTAP2_PIN_LOCAL_MAX_LEN + 1] = {0};

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        with_view_model(
            instance->view,
            U2fPinInputModel * model,
            {
                /* Wrap at both ends so the wheel never dead-ends. */
                if(event->key == InputKeyUp) {
                    model->focus =
                        (uint8_t)((model->focus + PIN_KEY_COUNT - 1) % PIN_KEY_COUNT);
                } else {
                    model->focus = (uint8_t)((model->focus + 1) % PIN_KEY_COUNT);
                }
            },
            true);
        return true;

    } else if(event->key == InputKeyOk) {
        if(event->type != InputTypeShort) return false;

        with_view_model(
            instance->view,
            U2fPinInputModel * model,
            {
                model->error = NULL;
                if(model->focus < PIN_KEY_DIGIT_COUNT) {
                    if(model->len < CTAP2_PIN_LOCAL_MAX_LEN) {
                        model->pin[model->len++] = (char)('0' + model->focus);
                        model->pin[model->len] = '\0';
                    }
                } else if(model->focus == PIN_KEY_DELETE) {
                    if(model->len > 0) model->pin[--model->len] = '\0';
                } else if(model->len >= CTAP2_PIN_MIN_LEN) {
                    memcpy(committed, model->pin, sizeof(committed));
                    commit = true;
                }
            },
            true);

        /* Fire the callback OUTSIDE with_view_model: the done handler switches
         * scenes, and doing that while holding the model lock deadlocks the
         * draw thread -- the same trap the ViewPort apps in this tree hit. */
        if(commit && instance->done_callback != NULL) {
            instance->done_callback(committed, instance->context);
        }
        memset(committed, 0, sizeof(committed));
        return true;
    }

    return false;
}

U2fPinInput* u2f_pin_input_alloc(void) {
    U2fPinInput* instance = malloc(sizeof(U2fPinInput));

    instance->view = view_alloc();
    instance->done_callback = NULL;
    instance->back_callback = NULL;
    instance->context = NULL;

    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(U2fPinInputModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, u2f_pin_input_draw_callback);
    view_set_input_callback(instance->view, u2f_pin_input_input_callback);

    return instance;
}

void u2f_pin_input_free(U2fPinInput* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* u2f_pin_input_get_view(U2fPinInput* instance) {
    furi_assert(instance);
    return instance->view;
}

void u2f_pin_input_set_callbacks(
    U2fPinInput* instance,
    U2fPinInputDoneCallback done_callback,
    U2fPinInputBackCallback back_callback,
    void* context) {
    furi_assert(instance);
    instance->done_callback = done_callback;
    instance->back_callback = back_callback;
    instance->context = context;
}

void u2f_pin_input_reset(U2fPinInput* instance, const char* header) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        U2fPinInputModel * model,
        {
            memset(model->pin, 0, sizeof(model->pin));
            model->len = 0;
            model->focus = 0;
            model->header = header;
            model->error = NULL;
        },
        true);
}

void u2f_pin_input_set_error(U2fPinInput* instance, const char* error) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        U2fPinInputModel * model,
        {
            memset(model->pin, 0, sizeof(model->pin));
            model->len = 0;
            model->focus = 0;
            model->error = error;
        },
        true);
}
