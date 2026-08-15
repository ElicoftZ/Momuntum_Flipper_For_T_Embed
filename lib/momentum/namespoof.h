#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NAMESPOOF_HEADER  "Flipper Name File"
#define NAMESPOOF_VERSION 1
#define NAMESPOOF_PATH    EXT_PATH("dolphin/name.settings")

/** Apply the custom device name from NAMESPOOF_PATH, or restore the real one
 * if the file is missing or malformed. Safe to call repeatedly. */
void namespoof_init(void);

#ifdef __cplusplus
}
#endif
