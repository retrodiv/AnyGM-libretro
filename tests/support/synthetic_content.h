/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_TEST_SYNTHETIC_CONTENT_H
#define ANYGM_TEST_SYNTHETIC_CONTENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct AnygmSyntheticContent {
  char directory[96];
  char path[192];
} AnygmSyntheticContent;

int anygm_synthetic_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_simulated_key_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_draw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_background_color_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_clear_view_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_clear_view_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_multiview_framebuffer_content_create(
    AnygmSyntheticContent *fixture);
int anygm_synthetic_game_change_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_game_restart_content_create(AnygmSyntheticContent *fixture);
void anygm_synthetic_content_destroy(AnygmSyntheticContent *fixture);
int anygm_synthetic_content_read(const AnygmSyntheticContent *fixture,uint8_t **data,size_t *size);

#endif
