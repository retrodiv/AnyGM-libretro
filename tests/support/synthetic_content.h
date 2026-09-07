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
/* The neutral engine fixture with one object-held array of lists and one global list. Its Step
 * deliberately rewrites both so a persistent content override can prove it runs after Step. */
int anygm_synthetic_list_override_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_anchor_script_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_simulated_key_content_create(AnygmSyntheticContent *fixture);
/* One synthetic instance presses and releases a key in one Step and owns
 * the matching Key Press event. The event records the Step count observed. */
int anygm_synthetic_bridged_key_content_create(AnygmSyntheticContent *fixture);
/* One synthetic instance pairs key press/release for five Steps and counts
 * the following Begin Steps that observe the key held. */
int anygm_synthetic_bridged_hold_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_draw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_shifted_port_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_window_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_window_gui_size_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_surface_canvas_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_background_color_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_clear_view_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_clear_view_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_multiview_framebuffer_content_create(
    AnygmSyntheticContent *fixture);
int anygm_synthetic_undefined_placement_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_room_deactivation_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_game_change_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_game_restart_content_create(AnygmSyntheticContent *fixture);
/* One instance whose Alarm 1 counts a tick and re-arms itself on a sixty-frame period, long enough
 * to tell a countdown that resumed from its remainder apart from one that started the period over. */
int anygm_synthetic_alarm_content_create(AnygmSyntheticContent *fixture);
/* A spawner whose Alarm 1 creates a second object, a placed instance destroyed on room entry so a
 * pool slot above the spawner's is free when that happens, and the created object's own Alarm 0
 * armed for the very next frame. Each event records the frame it ran on, so the pair of numbers
 * says whether the alarm phase reached an instance that did not exist when the phase began. */
int anygm_synthetic_alarm_phase_content_create(AnygmSyntheticContent *fixture);
/* Two instances answering different mouse subtypes on the same press: the first placed one owns
 * the global-press subtype and the second owns global-held, which comes first by subtype and
 * second by instance. Each records the order it ran in, so the pair says which of the two the
 * dispatch follows. */
int anygm_synthetic_mouse_order_content_create(AnygmSyntheticContent *fixture);
/* One script named as an extension's init script, setting a library handle to a non-zero sentinel,
 * and one placed instance whose Create records what that handle held. The pair says whether the
 * init ran, and whether it ran before the first room. */
int anygm_synthetic_extension_init_content_create(AnygmSyntheticContent *fixture);
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
