// protocols/protocol_items.h
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/subghz/types.h>

#include "kia_generic.h"
#include "scher_khan.h"
#include "kia_v0.h"
#include "kia_v1.h"
#include "kia_v2.h"
#include "kia_v3_v4.h"
#include "kia_v5.h"
#include "kia_v6.h"
#include "kia_v7.h"
#include "ford_v0.h"
#include "ford_v1.h"
#include "ford_v2.h"
#include "ford_v3.h"
#include "fiat_v0.h"
#include "fiat_v1.h"
#include "fiat_v2.h"
#include "mazda_v0.h"
#include "mitsubishi_v0.h"
#include "porsche_touareg.h"
#include "subaru.h"
#include "suzuki.h"
#include "vag.h"
#include "star_line.h"
#include "psa.h"
#include "chrysler_v0.h"
#include "honda_static.h"
#include "honda_v1.h"
#include "honda_v2.h"
#include "renault_v0.h"
#include "aut64.h"

extern const SubGhzProtocolRegistry protopirate_protocol_registry;

// Restored from before the registry consolidation (commit cddaca59) --
// the emulate scene and TX protocol plugins still look up protocols through
// this canonical-name/TX-key catalog. See protocol_items.c for the table
// itself (kept close to upstream's, since this port's registry entries and
// upstream's catalog entries already line up 1:1 for every protocol here).
typedef enum {
    ProtoPirateProtocolRegistryRouteAMDefault = 0,
    ProtoPirateProtocolRegistryRouteAMVag,
    ProtoPirateProtocolRegistryRouteFMDefault,
    ProtoPirateProtocolRegistryRouteFMF4,
    ProtoPirateProtocolRegistryRouteFMHonda1,
} ProtoPirateProtocolRegistryRoute;

typedef enum {
    ProtoPirateProtocolCatalogRouteAMDefault = 0,
    ProtoPirateProtocolCatalogRouteAMVag,
    ProtoPirateProtocolCatalogRouteFMDefault,
    ProtoPirateProtocolCatalogRouteFMF4,
    ProtoPirateProtocolCatalogRouteFMHonda1,
    ProtoPirateProtocolCatalogRouteByModulation,
} ProtoPirateProtocolCatalogRoutePolicy;

typedef struct {
    const char* canonical_name;
    ProtoPirateProtocolCatalogRoutePolicy route_policy;
    const char* tx_key;
} ProtoPirateProtocolCatalogEntry;

const ProtoPirateProtocolCatalogEntry*
    protopirate_protocol_catalog_find(const char* protocol_name);

const char* protopirate_protocol_catalog_canonical_name(const char* protocol_name);

bool protopirate_protocol_catalog_can_tx(const char* protocol_name);

bool protopirate_protocol_catalog_offers_bruteforce(const char* protocol_name);

const char* protopirate_protocol_catalog_tx_key(const char* protocol_name);

const char*
    protopirate_protocol_catalog_display_name(const char* protocol_name, uint32_t protocol_type);

ProtoPirateProtocolRegistryRoute protopirate_protocol_catalog_get_route(
    const char* preset_name,
    uint32_t frequency,
    const uint8_t* preset_data,
    size_t preset_data_size,
    const char* protocol_name);

ProtoPirateProtocolRegistryRoute protopirate_get_protocol_registry_route(
    const char* preset_name,
    uint32_t frequency,
    const uint8_t* preset_data,
    size_t preset_data_size,
    const char* protocol_name);

const char* protopirate_get_protocol_registry_route_name(ProtoPirateProtocolRegistryRoute route);

// Timing information for protocol analysis
typedef struct {
    const char* name;
    uint32_t te_short;
    uint32_t te_long;
    uint32_t te_delta;
    uint32_t min_count_bit;
} ProtoPirateProtocolTiming;

// Get timing info for a protocol by name (returns NULL if not found)
const ProtoPirateProtocolTiming* protopirate_get_protocol_timing(const char* protocol_name);

// Get timing info by index (for iteration)
const ProtoPirateProtocolTiming* protopirate_get_protocol_timing_by_index(size_t index);

// Get number of protocols with timing info
size_t protopirate_get_protocol_timing_count(void);
