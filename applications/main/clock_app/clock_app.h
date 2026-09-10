#pragma once

#include <input/input.h>

#define TAG "Clock"

typedef enum {
    EventTypeTick,
    EventTypeKey,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* event_queue;
    uint64_t stopwatch_start_seconds;
    uint64_t stopwatch_elapsed_seconds;
    bool stopwatch_running;
    bool show_stopwatch;
} ClockState;
