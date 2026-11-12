/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_CONFIG_H
#define ANYGM_CONTENT_CONFIG_H

#include "content_transform.h"

#define ANYGM_CONFIG_MAX_SECTIONS 256u
#define ANYGM_CONFIG_OVERRIDE_BYTES 4096u

typedef struct AnygmContentConfig AnygmContentConfig;

/* Compile/validate every default transform section.
 * No partial configuration is returned after failure. Unknown INI sections
 * remain uninterpreted; removed sha256 selectors are always rejected. */
AnygmContentConfig *anygm_content_config_parse(const void *text,size_t size,
                                             char *error,size_t error_size);
void anygm_content_config_destroy(AnygmContentConfig *config);
/* Fast-path query without executing source preparation. */
int anygm_content_config_has_input(const AnygmContentConfig *config);
/* Apply defaults. Overrides are bounded text for the engine-owned directive parser. */
int anygm_content_config_apply(const AnygmContentConfig *config,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size);

#endif
