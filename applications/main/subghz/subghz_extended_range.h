#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Momentum's extended-range marker. The port previously only understood OFW's
 * "dangerous_settings" file; this is the name Momentum writes, so a card moved
 * between the two keeps the setting.
 *
 * Momentum's file also carries ignore_default_tx_region. This port has no
 * region locking to bypass, so that key is deliberately not honoured. */
#define SUBGHZ_EXTENDED_RANGE_PATH EXT_PATH("subghz/assets/extend_range.txt")
#define SUBGHZ_EXTENDED_RANGE_KEY  "use_ext_range_at_own_risk"

/** Read the persisted extended-range setting. False if unset or unreadable. */
bool subghz_extended_range_load(void);

/** Persist the setting and apply it to the radio immediately. */
bool subghz_extended_range_save(bool enabled);

#ifdef __cplusplus
}
#endif
