#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DetectorAll,
    DetectorFlipper,
    DetectorFlock,
    DetectorAxon,
    DetectorSkimmer,
    DetectorMeta,
    DetectorSmartTag,
    DetectorAirTag,
    DetectorAirPods,
    DetectorMicrosoft,
    DetectorPixelBuds,
    DetectorTile,
    DetectorBeats,
    DetectorAppleHost,
    DetectorXiaomi,
    DetectorGalaxyBuds,
    DetectorGarmin,
    DetectorFitbit,
    DetectorGoPro,
    DetectorKindCount,
} DetectorKind;

#define DETECTOR_BIT(kind) (1u << (kind))
typedef struct {
    uint32_t kinds;
    uint32_t signatures;
    char name[32];
} DetectorMatch;

extern const char* const detector_labels[DetectorKindCount];
bool ble_detector_parse(const uint8_t* data, size_t length, DetectorMatch* match);
