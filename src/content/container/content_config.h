/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_CONFIG_H
#define ANYGM_CONTENT_CONFIG_H

#include "content_transform.h"

#define ANYGM_CONFIG_MAX_SECTIONS 256u
#define ANYGM_CONFIG_OVERRIDE_BYTES 4096u

typedef struct AnygmContentConfig AnygmContentConfig;

/* Compile/validate every default and selected transform section.
 * No partial configuration is returned after failure. Unknown INI sections
 * remain uninterpreted; unknown selector prefixes are rejected. */
AnygmContentConfig *anygm_content_config_parse(const void *text,size_t size,
                                             char *error,size_t error_size);
void anygm_content_config_destroy(AnygmContentConfig *config);
/* Fast-path query without executing source preparation. */
int anygm_content_config_has_input(const AnygmContentConfig *config);
/* Recognition keys are bounded hints, never integrity checks. */
int anygm_content_config_candidate(const AnygmContentConfig *config,int zip,uint64_t size);
int anygm_content_config_input_candidate(const AnygmContentConfig *config,uint64_t size);
int anygm_content_config_key_has_input(const AnygmContentConfig *config,const char *key);
int anygm_content_config_matches(const AnygmContentConfig *config,const char *key);
int anygm_content_config_select(const AnygmContentConfig *config,const char *key,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size);
/* Apply defaults. Overrides are bounded text for the engine-owned directive parser. */
int anygm_content_config_apply(const AnygmContentConfig *config,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size);

#endif
