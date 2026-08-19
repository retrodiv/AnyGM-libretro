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
/* One synthetic instance pairs key press/release for five Steps and counts
 * the following Begin Steps that observe the key held. */
int anygm_synthetic_bridged_hold_content_create(AnygmSyntheticContent *fixture);
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
/* Two rooms whose authored view ports differ - 128x96 then 192x144 - over one view of 64x48, and
 * one instance whose Create sets a scale of 3 and whose Step advances a room when a global asks.
 * Between them these are the two shapes a scoped override has to be able to give back: a value the
 * content wrote once and never revisits, and a value entering a room rewrites from that room's own
 * record. Advancing on a global rather than on input keeps the room change under the test's
 * control without running any input path. */
int anygm_synthetic_scoped_override_content_create(AnygmSyntheticContent *fixture);
/* Paired synthetic classic fixtures use the same 64x48 view and 128x96 port. One leaves a
 * matching surface bound after the step phase; the other uses ordinary room presentation. */
int anygm_synthetic_classic_compositor_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_plain_content_create(AnygmSyntheticContent *fixture);
/* One instance whose Alarm 0 spins on sleep() until a global is set, and a Step event that counts
 * the frames it runs on. A wait like this cannot end inside the frame it starts on, so the counter
 * says what the runtime did with the event: held it and continued it later, or ran the whole loop
 * inside one frame and gave up on it. */
int anygm_synthetic_blocking_wait_content_create(AnygmSyntheticContent *fixture);
void anygm_synthetic_content_destroy(AnygmSyntheticContent *fixture);
int anygm_synthetic_content_read(const AnygmSyntheticContent *fixture,uint8_t **data,size_t *size);

#endif
