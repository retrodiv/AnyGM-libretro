/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Software mixer for AUDO/SOND sounds and FMOD bank voices.
 * WAV PCM is referenced in place. Embedded and grouped OGG/MP3 data is decoded
 * on first playback; external OGG registration decodes immediately.
 */
#ifndef GML_AUDIO_H
#define GML_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include "gml_win.h"

typedef struct GmlAudio GmlAudio;

GmlAudio *gml_audio_create(GmlWin *win);
struct GmlFmodBanks *gml_audio_get_fmod(GmlAudio *a);   /* FMOD bank set, or NULL when unavailable */
void gml_audio_free(GmlAudio *a);

/* play sound resource `snd` (SOND index); loop!=0 repeats. Returns a voice handle. */
int  gml_audio_play(GmlAudio *a, int snd, int loop);
int  gml_audio_play_on(GmlAudio *a, int snd, int loop, int emitter);
int  gml_audio_warm_sound(GmlAudio *a, int snd);
/* Decode an external OGG blob and register it as a new sound.
 * The returned sound index works with gml_audio_play/stop/gain/pitch.
 * Handles are recycled after caster_free. */
int  gml_audio_add_ogg(GmlAudio *a, const uint8_t *ogg, int len);
void gml_audio_caster_free(GmlAudio *a, int handle);
void gml_audio_caster_free_all(GmlAudio *a);
void gml_audio_stop(GmlAudio *a, int snd);   /* stop every voice of this sound */
void gml_audio_stop_all(GmlAudio *a);
void gml_audio_pause_all(GmlAudio *a, int paused);  /* paused!=0 freezes all voices (keep position) */
void gml_audio_pause_sound(GmlAudio *a, int target, int paused);
int  gml_audio_is_playing(GmlAudio *a, int snd);
int  gml_audio_voice_paused(GmlAudio *a, int snd);   /* 1 if a matching voice exists and is paused */
void gml_audio_set_master_gain(GmlAudio *a, double gain);
double gml_audio_get_master_gain(GmlAudio *a);
void gml_audio_channel_num(GmlAudio *a, int channels);
int  gml_audio_get_channel_num(GmlAudio *a);
void gml_audio_sound_gain(GmlAudio *a, int target, double gain);
double gml_audio_sound_get_gain(GmlAudio *a, int target);
void gml_audio_sound_pitch(GmlAudio *a, int target, double pitch);
double gml_audio_sound_get_pitch(GmlAudio *a, int target);
double gml_audio_sound_length(GmlAudio *a, int sound);
void gml_audio_sound_set_track_position(GmlAudio *a, int target, double seconds);
double gml_audio_sound_get_track_position(GmlAudio *a, int target);
void gml_audio_sound_loop_start(GmlAudio *a, int target, double seconds);
void gml_audio_emitter_mix(GmlAudio *a, int emitter, double gain, double pan);
void gml_audio_voice_spatial(GmlAudio *a, int target, double gain, double pan);

/* mix `frames` stereo (interleaved L,R) int16 samples at 44100 Hz into out. */
void gml_audio_mix(GmlAudio *a, int16_t *out, int frames);

size_t gml_audio_state_size(GmlAudio *a);
int    gml_audio_state_save(GmlAudio *a, void *data, size_t len, size_t *written);
int    gml_audio_state_load(GmlAudio *a, const void *data, size_t len, size_t *used);

#endif
