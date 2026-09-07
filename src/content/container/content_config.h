/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_CONFIG_H
#define ANYGM_CONTENT_CONFIG_H

#include "content_transform.h"

#define ANYGM_CONFIG_MAX_SECTIONS 256u
#define ANYGM_CONFIG_OVERRIDE_BYTES 4096u

typedef struct AnygmContentConfig AnygmContentConfig;

/* Compile/validate every transform section, including unmatched selectors.
 * No partial configuration is returned after failure. Unknown INI sections
 * remain uninterpreted; malformed sha256 selectors are always rejected. */
AnygmContentConfig *anygm_content_config_parse(const void *text,size_t size,
                                             char *error,size_t error_size);
void anygm_content_config_destroy(AnygmContentConfig *config);
/* Fast-path query; includes unmatched source selectors without executing them. */
int anygm_content_config_has_input(const AnygmContentConfig *config);
/* NULL digest selects defaults; otherwise select precisely these original bytes.
 * Overrides are returned as bounded text for the engine-owned directive parser. */
int anygm_content_config_apply(const AnygmContentConfig *config,const uint8_t *digest,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size);

#endif
