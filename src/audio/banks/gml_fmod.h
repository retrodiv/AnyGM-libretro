/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_fmod.h — FMOD FSB5 audio decode (generic FMOD container support; see gml_fmod.c). */
#ifndef GML_FMOD_H
#define GML_FMOD_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
  int channels, rate, num_samples;
  uint32_t setup_crc;          /* Vorbis setup packet CRC32 (looked up in gml_fmod_codebooks.h) */
  const uint8_t *data;         /* raw uint16-size-prefixed Vorbis packets */
  size_t data_len;
} GmlFmodSample;

/* Locate subsound `index` inside an FSB5 chunk; fills *out. Returns 1 on success. */
int gml_fmod_fsb5_sample(const uint8_t *fsb5, size_t fsb5_len, int index, GmlFmodSample *out);

/* Decode FSB5 Vorbis subsound `index` to interleaved S16. Returns frames (per-channel sample count);
 * *out_pcm is malloc'd (caller frees). 0 on failure (unknown codebook / decode error). */
int gml_fmod_decode(const uint8_t *fsb5, size_t fsb5_len, int index, int *channels, int *rate, int16_t **out_pcm);

/* Bank sets resolve event paths through GUIDs to subsounds. Metadata and sample
 * headers remain in memory; compressed audio is read from disk on demand. */
typedef struct GmlFmodBanks GmlFmodBanks;   /* opaque */

/* Probe the supported bank directories for the string table, then attempt the
 * supported named banks. Returns NULL when no usable string table is available. */
GmlFmodBanks *gml_fmod_banks_load(const char *dir);
void gml_fmod_banks_free(GmlFmodBanks *b);

/* Resolve an event path ("event:/<bank>/<event>") to a playable subsound.
 * Returns 1 and fills the out params on success; 0 if the path/GUID/sample is unknown (→ stay silent,
 * never guess a wrong sound). *bank_index identifies which loaded bank holds the FSB5. */
int gml_fmod_banks_resolve(GmlFmodBanks *b, const char *path,
                           int *bank_index, int *subsound, int *loop,
                           uint32_t *loop_start, uint32_t *loop_end);

/* Decode a resolved (bank_index, subsound) to interleaved S16 (malloc'd; caller frees). Returns frames
 * (per-channel), or 0 on failure. Pulls the compressed subsound from disk on demand. */
int gml_fmod_banks_decode(GmlFmodBanks *b, int bank_index, int subsound,
                          int *channels, int *rate, int16_t **out_pcm);

/* Count of resolvable event paths (diagnostics). */
int gml_fmod_banks_num_events(GmlFmodBanks *b);

/* ---- Runtime playback: event-instance voices + software mixer ----
 * Handles are opaque ints (>0 valid, 0 = none/failure). `one_shot` voices auto-release when they
 * finish; created instances persist until released. The mixer resamples each voice to out_rate and
 * ADDS it into the S16 stereo buffer (so it composes with the engine's own AUDO mixer). */
int  gml_fmod_start(GmlFmodBanks *b, const char *path, int play_now, int one_shot);
void gml_fmod_play(GmlFmodBanks *b, int handle);
void gml_fmod_stop(GmlFmodBanks *b, int handle);
void gml_fmod_release(GmlFmodBanks *b, int handle);
void gml_fmod_set_paused(GmlFmodBanks *b, int handle, int paused);
void gml_fmod_set_paused_all(GmlFmodBanks *b, int paused);
int  gml_fmod_is_playing(GmlFmodBanks *b, int handle);
int  gml_fmod_get_paused(GmlFmodBanks *b, int handle);
double gml_fmod_get_length(GmlFmodBanks *b, const char *path);   /* seconds, 0 if unknown */
double gml_fmod_get_timeline_pos(GmlFmodBanks *b, int handle);   /* current position, ms */
void gml_fmod_set_timeline_pos(GmlFmodBanks *b, int handle, double ms);
void gml_fmod_set_param(GmlFmodBanks *b, int handle, const char *name, double value);
double gml_fmod_get_param(GmlFmodBanks *b, int handle, const char *name);
void gml_fmod_set_global_param(GmlFmodBanks *b, const char *name, double value);
double gml_fmod_get_global_param(GmlFmodBanks *b, const char *name);
void gml_fmod_set_listener(GmlFmodBanks *b, double x, double y);            /* 3D listener position */
void gml_fmod_set_3d(GmlFmodBanks *b, int handle, double x, double y);      /* 3D emitter position (pan) */
void gml_fmod_stop_all(GmlFmodBanks *b);
void gml_fmod_mix(GmlFmodBanks *b, int16_t *out, int frames, int out_rate);

#endif
