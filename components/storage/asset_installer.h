/**
 * @file asset_installer.h
 * @brief First-boot install of the SD content this port adds.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Install the bundled SD content if the card does not already have it.
 *
 * Does nothing and returns false when there is no `assets` partition (personal
 * / dual-boot builds) or when the card already carries a version.txt.
 *
 * @return true only when files were actually written.
 */
bool asset_installer_run(void);

#ifdef __cplusplus
}
#endif
