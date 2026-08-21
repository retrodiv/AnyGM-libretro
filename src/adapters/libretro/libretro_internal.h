/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_LIBRETRO_INTERNAL_H
#define ANYGM_LIBRETRO_INTERNAL_H

#include "anygm.h"
#include "libretro.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct LibretroAdapter {
  retro_environment_t environment;
  retro_video_refresh_t video;
  retro_audio_sample_t audio_sample;
  retro_audio_sample_batch_t audio_batch;
  retro_input_poll_t input_poll;
  retro_input_state_t input_state;
  retro_log_printf_t log;
  struct retro_rumble_interface rumble;
  const struct retro_vfs_interface *vfs;
  AnygmEngine *engine;
  AnygmFrameOutput frame;
  AnygmAvInfo av;
  AnygmConfig config;
  uint8_t keyboard_events[ANYGM_MAX_KEYS];
  bool keyboard_events_accepted;
  uint8_t override_used[ANYGM_MAX_RUNTIME_OVERRIDES];
  uint32_t port_device[ANYGM_MAX_GAMEPADS];
  uint32_t gamepad_ports;
  unsigned pointer_seen;
  bool loaded;
  bool rumble_available;
  bool frame_completed;
  bool reset_pending_frame;
  bool reset_ring_rejection_reported;
  bool startup_ring_compact;
  size_t fixed_state_capacity;
  size_t startup_resume_capacity;
  bool state_capacity_growth_reported;
  bool variable_state_supported;
  char save_directory[1024];
  char cache_directory[1024];
  char language[16];
  char region[16];
  char language_tag[32];
  /* the locale spelled the way a POSIX environment variable spells it; see
   * host_development_setting */
  char locale_variable[40];
  uint64_t seed_counter;
  /* memoised development settings; see host_development_setting */
  struct { uint32_t hash; const char *value; char name[52]; } setting_cache[128];
  int setting_cache_count;
} LibretroAdapter;

extern LibretroAdapter g_libretro;

void libretro_log(enum retro_log_level level,const char *format,...);
void libretro_host_services_init(AnygmHostServices *services);
void libretro_vfs_request(void);
void libretro_vfs_services_init(AnygmHostServices *services);
void libretro_options_register(void);
void libretro_options_apply(bool all_fields);
const char *libretro_options_value(const char *key);
void libretro_options_publish_rooms(void);
void libretro_options_release(void);
#if ANYGM_HARDWARE_RENDER
/* Read the hybrid-GPU option and negotiate a frontend graphics context. Called once before content
 * is loaded, because changing the choice mid-session would leave half the frame on one path. */
void libretro_hw_render_request(void);
void libretro_hw_render_release(void);
bool libretro_hw_render_requested(void);
#else
static inline void libretro_hw_render_request(void){}
static inline void libretro_hw_render_release(void){}
static inline bool libretro_hw_render_requested(void){ return false; }
#endif
void libretro_input_register(void);
void libretro_input_snapshot(AnygmInputFrame *input,uint32_t width,uint32_t height);
void libretro_update_av(void);

#endif
