#include "../infrared_app_i.h"

#include <furi_hal_power.h>

#define IR_JAMMER_DUTY_CYCLE 0.33f
#define IR_JAMMER_TIMINGS_MAX 128U

typedef enum {
    IrJammerModeDense,
    IrJammerModeProtocol,
    IrJammerModeSweep,
    IrJammerModeRandom,
    IrJammerModeMixed,
    IrJammerModeCount,
} IrJammerMode;

typedef enum {
    IrJammerIntensityLow,
    IrJammerIntensityMedium,
    IrJammerIntensityHigh,
    IrJammerIntensityCount,
} IrJammerIntensity;

typedef enum {
    IrJammerRowOutput,
    IrJammerRowFrequency,
    IrJammerRowMode,
    IrJammerRowIntensity,
    IrJammerRowTimeout,
} IrJammerRow;

static const uint32_t ir_jammer_frequencies[] = {
    30000U,
    33000U,
    36000U,
    38000U,
    40000U,
    42000U,
    56000U,
};

static const char* const ir_jammer_frequency_text[] = {
    "Auto",
    "30 kHz",
    "33 kHz",
    "36 kHz",
    "38 kHz",
    "40 kHz",
    "42 kHz",
    "56 kHz",
};

static const char* const ir_jammer_mode_text[] = {
    "Dense",
    "Protocol",
    "Sweep",
    "Random",
    "Mixed",
};

static const char* const ir_jammer_intensity_text[] = {
    "Low",
    "Medium",
    "High",
};

static const char* const ir_jammer_timeout_text[] = {
    "15 sec",
    "30 sec",
    "60 sec",
    "Unlimited",
};

static const uint32_t ir_jammer_timeout_ms[] = {
    15000U,
    30000U,
    60000U,
    0U,
};

static uint32_t ir_jammer_random(InfraredAppState* state) {
    uint32_t value = state->jammer_rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    state->jammer_rng = value ? value : 0xA341316CU;
    return state->jammer_rng;
}

static size_t ir_jammer_timing_count(uint8_t intensity) {
    if(intensity == IrJammerIntensityLow) return 32U;
    if(intensity == IrJammerIntensityHigh) return IR_JAMMER_TIMINGS_MAX;
    return 64U;
}

static uint32_t ir_jammer_end_gap(uint8_t intensity) {
    if(intensity == IrJammerIntensityLow) return 8000U;
    if(intensity == IrJammerIntensityHigh) return 400U;
    return 2200U;
}

static void ir_jammer_build_dense(
    InfraredAppState* state,
    uint32_t* timings,
    size_t count) {
    for(size_t i = 0; i < count; i += 2U) {
        const uint32_t variation = ir_jammer_random(state) % 90U;
        timings[i] = 140U + variation;
        timings[i + 1U] = 110U + ((variation * 3U) % 130U);
    }
}

static void ir_jammer_build_protocol_noise(
    InfraredAppState* state,
    uint32_t* timings,
    size_t count) {
    timings[0] = 9000U;
    timings[1] = 4500U;
    for(size_t i = 2U; i < count; i += 2U) {
        timings[i] = 560U;
        timings[i + 1U] = (ir_jammer_random(state) & 1U) ? 1690U : 560U;
    }
}

static void ir_jammer_build_sweep(
    InfraredAppState* state,
    uint32_t* timings,
    size_t count) {
    const uint32_t phase = state->jammer_packet_count % 32U;
    const uint32_t base = (phase < 16U) ? (90U + phase * 45U) :
                                         (90U + (31U - phase) * 45U);
    for(size_t i = 0; i < count; i += 2U) {
        timings[i] = base + ((i * 17U) % 180U);
        timings[i + 1U] = 80U + ((base + i * 29U) % 900U);
    }
}

static void ir_jammer_build_random(
    InfraredAppState* state,
    uint32_t* timings,
    size_t count) {
    for(size_t i = 0; i < count; i += 2U) {
        timings[i] = 80U + (ir_jammer_random(state) % 720U);
        timings[i + 1U] = 80U + (ir_jammer_random(state) % 2400U);
    }
}

static InfraredWorkerGetSignalResponse
    ir_jammer_get_signal_callback(void* context, InfraredWorker* worker) {
    InfraredApp* infrared = context;
    InfraredAppState* state = &infrared->app_state;
    if(!state->jammer_active) {
        return InfraredWorkerGetSignalResponseStop;
    }

    uint32_t timings[IR_JAMMER_TIMINGS_MAX];
    const size_t count = ir_jammer_timing_count(state->jammer_intensity);
    IrJammerMode mode = (IrJammerMode)state->jammer_mode;
    if(mode == IrJammerModeMixed) {
        mode = (IrJammerMode)(state->jammer_packet_count % IrJammerModeMixed);
    }

    switch(mode) {
    case IrJammerModeDense:
        ir_jammer_build_dense(state, timings, count);
        break;
    case IrJammerModeProtocol:
        ir_jammer_build_protocol_noise(state, timings, count);
        break;
    case IrJammerModeSweep:
        ir_jammer_build_sweep(state, timings, count);
        break;
    case IrJammerModeRandom:
        ir_jammer_build_random(state, timings, count);
        break;
    default:
        ir_jammer_build_dense(state, timings, count);
        break;
    }

    // Always end with a space so consecutive patterns have a clean boundary.
    timings[count - 1U] = ir_jammer_end_gap(state->jammer_intensity);

    uint32_t frequency;
    if(state->jammer_frequency_index == 0U) {
        const size_t index =
            state->jammer_packet_count % COUNT_OF(ir_jammer_frequencies);
        frequency = ir_jammer_frequencies[index];
    } else {
        frequency = ir_jammer_frequencies[state->jammer_frequency_index - 1U];
    }

    infrared_worker_set_raw_signal(
        worker, timings, count, frequency, IR_JAMMER_DUTY_CYCLE);
    state->jammer_packet_count++;
    return InfraredWorkerGetSignalResponseNew;
}

static void ir_jammer_set_output_text(InfraredApp* infrared, const char* text) {
    VariableItem* output =
        variable_item_list_get(infrared->var_item_list, IrJammerRowOutput);
    variable_item_set_current_value_text(output, text);
}

static void ir_jammer_start(InfraredApp* infrared) {
    InfraredAppState* state = &infrared->app_state;
    if(state->jammer_active || state->is_transmitting) {
        return;
    }

    state->jammer_active = true;
    state->jammer_started_at = furi_get_tick();
    state->jammer_packet_count = 0U;
    state->jammer_rng = state->jammer_started_at ^ 0x9E3779B9U;

    infrared_worker_tx_set_get_signal_callback(
        infrared->worker, ir_jammer_get_signal_callback, infrared);
    infrared_worker_tx_start(infrared->worker);

    infrared_play_notification_message(
        infrared, InfraredNotificationMessageBlinkStartSend);
    furi_hal_power_insomnia_enter();
    state->is_transmitting = true;
    ir_jammer_set_output_text(infrared, "RUNNING");
}

static void ir_jammer_stop(InfraredApp* infrared, const char* status) {
    InfraredAppState* state = &infrared->app_state;
    state->jammer_active = false;
    if(state->is_transmitting) {
        infrared_tx_stop(infrared);
    }
    ir_jammer_set_output_text(infrared, status);
}

static void ir_jammer_frequency_changed(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    infrared->app_state.jammer_frequency_index = index;
    variable_item_set_current_value_text(item, ir_jammer_frequency_text[index]);
}

static void ir_jammer_mode_changed(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    infrared->app_state.jammer_mode = index;
    variable_item_set_current_value_text(item, ir_jammer_mode_text[index]);
}

static void ir_jammer_intensity_changed(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    infrared->app_state.jammer_intensity = index;
    variable_item_set_current_value_text(item, ir_jammer_intensity_text[index]);
}

static void ir_jammer_timeout_changed(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    infrared->app_state.jammer_timeout_index = index;
    variable_item_set_current_value_text(item, ir_jammer_timeout_text[index]);
}

static void ir_jammer_enter_callback(void* context, uint32_t index) {
    InfraredApp* infrared = context;
    if(index != IrJammerRowOutput) {
        return;
    }

    if(infrared->app_state.jammer_active) {
        ir_jammer_stop(infrared, "Stopped");
    } else {
        ir_jammer_start(infrared);
    }
}

static void ir_jammer_add_setting(
    InfraredApp* infrared,
    const char* label,
    const char* const* values,
    uint8_t values_count,
    uint8_t selected,
    VariableItemChangeCallback callback) {
    VariableItem* item = variable_item_list_add(
        infrared->var_item_list, label, values_count, callback, infrared);
    variable_item_set_current_value_index(item, selected);
    variable_item_set_current_value_text(item, values[selected]);
}

void infrared_scene_jammer_on_enter(void* context) {
    InfraredApp* infrared = context;
    InfraredAppState* state = &infrared->app_state;

    state->jammer_active = false;
    state->is_transmitting = false;
    state->jammer_frequency_index = 0U; // Auto covers common 30-56 kHz receivers.
    state->jammer_mode = IrJammerModeMixed;
    state->jammer_intensity = IrJammerIntensityMedium;
    state->jammer_timeout_index = 1U;
    state->jammer_packet_count = 0U;
    state->jammer_rng = furi_get_tick() ^ 0xA341316CU;

    variable_item_list_set_header(
        infrared->var_item_list, "Native IR Jammer - own devices only");
    VariableItem* output = variable_item_list_add(
        infrared->var_item_list, "Output (OK)", 1U, NULL, infrared);
    variable_item_set_current_value_text(output, "Stopped");

    ir_jammer_add_setting(
        infrared,
        "Carrier",
        ir_jammer_frequency_text,
        COUNT_OF(ir_jammer_frequency_text),
        state->jammer_frequency_index,
        ir_jammer_frequency_changed);
    ir_jammer_add_setting(
        infrared,
        "Pattern",
        ir_jammer_mode_text,
        COUNT_OF(ir_jammer_mode_text),
        state->jammer_mode,
        ir_jammer_mode_changed);
    ir_jammer_add_setting(
        infrared,
        "Intensity",
        ir_jammer_intensity_text,
        COUNT_OF(ir_jammer_intensity_text),
        state->jammer_intensity,
        ir_jammer_intensity_changed);
    ir_jammer_add_setting(
        infrared,
        "Safety stop",
        ir_jammer_timeout_text,
        COUNT_OF(ir_jammer_timeout_text),
        state->jammer_timeout_index,
        ir_jammer_timeout_changed);

    variable_item_list_set_enter_callback(
        infrared->var_item_list, ir_jammer_enter_callback, infrared);
    view_dispatcher_switch_to_view(
        infrared->view_dispatcher, InfraredViewVariableList);
}

bool infrared_scene_jammer_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    InfraredAppState* state = &infrared->app_state;

    if(event.type == SceneManagerEventTypeTick && state->jammer_active) {
        const uint32_t elapsed_ticks = furi_get_tick() - state->jammer_started_at;
        const uint32_t timeout_ms = ir_jammer_timeout_ms[state->jammer_timeout_index];
        if(timeout_ms == 0U) {
            ir_jammer_set_output_text(infrared, "RUN NO TIMEOUT");
            return true;
        }

        const uint32_t timeout_ticks = furi_ms_to_ticks(timeout_ms);
        if(elapsed_ticks >= timeout_ticks) {
            ir_jammer_stop(infrared, "Safety stopped");
        } else {
            char status[24];
            const uint32_t ticks_left = timeout_ticks - elapsed_ticks;
            const uint32_t seconds_left =
                (ticks_left + furi_kernel_get_tick_frequency() - 1U) /
                furi_kernel_get_tick_frequency();
            snprintf(status, sizeof(status), "RUN %lus", (unsigned long)seconds_left);
            ir_jammer_set_output_text(infrared, status);
        }
        return true;
    }

    return false;
}

void infrared_scene_jammer_on_exit(void* context) {
    InfraredApp* infrared = context;
    ir_jammer_stop(infrared, "Stopped");
    infrared_worker_tx_set_get_signal_callback(infrared->worker, NULL, NULL);
    variable_item_list_reset(infrared->var_item_list);
}
