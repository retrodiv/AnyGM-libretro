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
/* One synthetic instance presses and releases a key in one Step and owns
 * the matching Key Press event. The event records the Step count observed. */
int anygm_synthetic_bridged_key_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_draw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_background_color_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_clear_view_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_clear_view_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_multiview_framebuffer_content_create(
    AnygmSyntheticContent *fixture);
int anygm_synthetic_room_deactivation_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_game_change_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_game_restart_content_create(AnygmSyntheticContent *fixture);
/* One instance whose Alarm 1 counts a tick and re-arms itself on a sixty-frame period, long enough
 * to tell a countdown that resumed from its remainder apart from one that started the period over. */
int anygm_synthetic_alarm_content_create(AnygmSyntheticContent *fixture);
void anygm_synthetic_content_destroy(AnygmSyntheticContent *fixture);
int anygm_synthetic_content_read(const AnygmSyntheticContent *fixture,uint8_t **data,size_t *size);

#endif
