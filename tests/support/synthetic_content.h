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
/* The same fixture at an explicit authored room raster for presentation contracts. */
int anygm_synthetic_sized_content_create(AnygmSyntheticContent *fixture,unsigned width,unsigned height);
/* Two authored tilemap layers followed by an empty room, without external assets. */
int anygm_synthetic_tilemap_content_create(AnygmSyntheticContent *fixture);
/* The initial room owns a persistent controller and a boot-only global;
 * the second room records whether that controller's Create ran. */
int anygm_synthetic_cold_room_content_create(AnygmSyntheticContent *fixture);
/* The neutral engine fixture with one object-held array of lists and one global list. Its Step
 * deliberately rewrites both so a persistent content override can prove it runs after Step. */
int anygm_synthetic_list_override_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_anchor_script_content_create(AnygmSyntheticContent *fixture);
/* A modal draw fills the frame and preserves a green question marker; its script draws red. */
int anygm_synthetic_after_draw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_after_draw_anchor_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_simulated_key_content_create(AnygmSyntheticContent *fixture);
/* One synthetic instance presses and releases a key in one Step and owns
 * the matching Key Press event. The event records the Step count observed. */
int anygm_synthetic_bridged_key_content_create(AnygmSyntheticContent *fixture);
/* One synthetic instance pairs key press/release for five Steps and counts
 * the following Begin Steps that observe the key held. */
int anygm_synthetic_bridged_hold_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_draw_content_create(AnygmSyntheticContent *fixture);
/* One assigned view surface, a translated camera and a GUI-owned transparent clear. */
int anygm_synthetic_view_surface_target_content_create(AnygmSyntheticContent *fixture);
/* Two neutral sprite rooms: a held right key moves the visible object, then enters a second room. */
int anygm_synthetic_gpu_scene_route_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_gpu_dense_scene_route_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_shifted_port_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_window_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_window_gui_size_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_indirect_gui_upscale_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_resident_indirect_gui_upscale_content_create(AnygmSyntheticContent *fixture);
/* The same owned fixture in a full or narrow room, for forced-raster admission. */
int anygm_synthetic_forced_owned_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_translated_world_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_forced_narrow_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_forced_narrow_overdraw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_indirect_gui_reduce_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_indirect_gui_alpha_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_direct_gui_sprite_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_resident_direct_gui_content_create(AnygmSyntheticContent *fixture);
/* Direct SCREEN keeps automatic application composition before the first GUI stretch. */
int anygm_synthetic_automatic_direct_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_automatic_alpha_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_automatic_viewless_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_automatic_viewless_alpha_gui_content_create(AnygmSyntheticContent *fixture);
/* An empty indirect GUI still needs the automatic application image before its final scale. */
int anygm_synthetic_automatic_empty_indirect_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_surface_canvas_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_background_color_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_moving_sound_content_create(AnygmSyntheticContent *fixture);
/* Request fifty display Hz during Create while retaining thirty logical FPS and active audio. */
int anygm_synthetic_display_cadence_content_create(AnygmSyntheticContent *fixture);
/* The same moving image/audio route at a large authored raster for frontend capacity growth. */
int anygm_synthetic_large_moving_sound_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_gpu_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_offset_gpu_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_fractional_gpu_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_fractional_scale_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_step_target_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_nested_target_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_assigned_surface_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_multiview_canvas_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_window_raster_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_crt_native_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_crt_scaled_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_host_box_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_ended_frame_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_cursor_frame_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_window_cursor_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_redraw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_viewless_draw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_viewless_resize_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_viewless_ortho_resize_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_reduced_view_resize_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_owned_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_manual_application_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_manual_application_scaled_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_late_manual_application_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_draw_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_identity_present_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_filtered_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_fractional_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_modern_content_create(AnygmSyntheticContent *fixture);
/* Later Classic native nearest: generation, view, retention and alpha axes;
 * the last four variants retain filtered or non-native presentation guards. */
int anygm_synthetic_classic_native_borrowed_content_create(AnygmSyntheticContent *fixture,unsigned variant);
int anygm_synthetic_classic_single_view_offset_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_geometry_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_borrowed_resize_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_read_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_cpu_gui_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_filter_transition_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_partial_alpha_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_zero_alpha_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_rectangle_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_alpha_rectangle_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_binary_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_source_alpha_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_zero_alpha_part_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_single_view_software3d_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_retained_target_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_late_retained_target_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_step_composition_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_step_base_writer_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_late_step_composition_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_native_step_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_late_native_step_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_scaled_step_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_filtered_step_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_scaled_filtered_step_content_create(AnygmSyntheticContent *fixture);
/* Draw on the logical root before retaining, releasing and recreating a numbered target. */
int anygm_synthetic_classic_root_handoff_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_scaled_root_handoff_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_filtered_root_handoff_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_scaled_filtered_root_handoff_content_create(AnygmSyntheticContent *fixture);
/* Independent nearest base/vertical samples and a physical doubled SCREEN. */
int anygm_synthetic_classic_sample_root_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_sample_resize_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_sample_filtered_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_step_clear_content_create(AnygmSyntheticContent *fixture);
int anygm_synthetic_classic_transition_content_create(AnygmSyntheticContent *fixture);
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
