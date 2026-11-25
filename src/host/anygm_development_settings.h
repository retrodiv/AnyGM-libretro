/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_DEVELOPMENT_SETTINGS_H
#define ANYGM_DEVELOPMENT_SETTINGS_H

enum {
#define ANYGM_DEVELOPMENT_SETTING(name,purpose,retention) ANYGM_DEVELOPMENT_##name,
#include "anygm_development_settings.def"
#undef ANYGM_DEVELOPMENT_SETTING
  ANYGM_DEVELOPMENT_COUNT
};

#endif
