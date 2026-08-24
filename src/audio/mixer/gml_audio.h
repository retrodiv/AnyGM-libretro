/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_audio.h — software mixer for GameMaker AUDO/SOND sounds. WAV data is mixed in place;
 * OGG Vorbis and MP3 assets are decoded lazily on first playback. */
#ifndef GML_AUDIO_H
#define GML_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include "gml_win.h"

typedef struct GmlAudio GmlAudio;

/* Resolve an audio-group sidecar using either the legacy numeric filename or the
 * custom relative path stored by newer package formats. */
int gml_audio_group_file_path(const GmlWin *win, int group, char *out, size_t out_cap);

GmlAudio *gml_audio_create(GmlWin *win);
void gml_audio_rebind_content(GmlAudio *audio,GmlWin *win);
struct GmlFmodBanks *gml_audio_get_fmod(GmlAudio *a);   /* FMOD bank set, or NULL when unused */
void gml_audio_free(GmlAudio *a);

/* play sound resource `snd` (SOND index); loop!=0 repeats. Returns a voice handle. */
int  gml_audio_play(GmlAudio *a, int snd, int loop);
int  gml_audio_play_on(GmlAudio *a, int snd, int loop, int emitter);
int  gml_audio_warm_sound(GmlAudio *a, int snd);
/* Register a bounded loose audio blob as a dynamic sound. The generic path accepts PCM WAV,
 * OGG Vorbis, and MP3; the OGG-only entry point retains eager caster validation. Returned handles
 * are sound indices usable with the regular playback controls and are recycled after free. */
int  gml_audio_add_ogg(GmlAudio *a, const uint8_t *ogg, int len);
int  gml_audio_add_encoded(GmlAudio *a, const uint8_t *encoded, int len);
int  gml_audio_add_pcm16(GmlAudio *a,const int16_t *pcm,uint32_t frames,
                         int channels,int sample_rate,
                         const uint8_t *identity,size_t identity_size);
/* Rehydrate a dynamic handle from its exact bounded source bytes during canonical state restore.
 * The replacement is accepted only when the bytes match the state-owned SHA-256 identity. */
int  gml_audio_restore_encoded(GmlAudio *a,int handle,const uint8_t *encoded,int len,
                               const uint8_t expected_sha256[32]);
/* Replace an existing sound asset with caller-supplied encoded bytes. */
int  gml_audio_replace_encoded(GmlAudio *a,int snd,const uint8_t *encoded,int len);
int  gml_audio_restore_pcm16(GmlAudio *a,int handle,const int16_t *pcm,uint32_t frames,
                             int channels,int sample_rate,
                             const uint8_t *identity,size_t identity_size,
                             const uint8_t expected_sha256[32]);
int  gml_audio_sound_content_hash(GmlAudio *a,int handle,uint8_t digest[32]);
void gml_audio_caster_free(GmlAudio *a, int handle);
void gml_audio_caster_free_all(GmlAudio *a);
void gml_audio_stop(GmlAudio *a, int snd);   /* stop every voice of this sound */
void gml_audio_stop_all(GmlAudio *a);
void gml_audio_pause_all(GmlAudio *a, int paused);  /* paused!=0 freezes all voices (keep position) */
void gml_audio_pause_sound(GmlAudio *a, int target, int paused);
int  gml_audio_is_playing(GmlAudio *a, int snd);
int  gml_audio_exists(GmlAudio *a, int target);       /* sound asset or live voice handle */
int  gml_audio_voice_paused(GmlAudio *a, int snd);   /* 1 if a matching voice exists and is paused */
int  gml_audio_voice_sound(GmlAudio *a, int voice);  /* sound index behind a live voice, or -1 */
double gml_audio_voice_pan(GmlAudio *a,int voice);
void gml_audio_set_master_gain(GmlAudio *a, double gain);
double gml_audio_get_master_gain(GmlAudio *a);
void gml_audio_group_gain(GmlAudio *a, int group, double gain, int milliseconds);
double gml_audio_group_get_gain(GmlAudio *a, int group);
void gml_audio_group_stop_all(GmlAudio *a, int group);
void gml_audio_channel_num(GmlAudio *a, int channels);
int  gml_audio_get_channel_num(GmlAudio *a);
void gml_audio_sound_gain(GmlAudio *a, int target, double gain);
void gml_audio_sound_gain_fade(GmlAudio *a, int target, double gain, int milliseconds);
double gml_audio_sound_get_gain(GmlAudio *a, int target);
void gml_audio_sound_pitch(GmlAudio *a, int target, double pitch);
double gml_audio_sound_get_pitch(GmlAudio *a, int target);
void gml_audio_sound_set_default_loop(GmlAudio *a, int sound, int loop);
int  gml_audio_sound_get_default_loop(GmlAudio *a, int sound);
void gml_audio_sound_set_external_type(GmlAudio *a, int sound, uint32_t type);
uint32_t gml_audio_sound_get_external_type(GmlAudio *a, int sound);
void gml_audio_sound_set_external_group(GmlAudio *a,int sound,int group);
double gml_audio_sound_length(GmlAudio *a, int sound);
/* Read-only view of a sound's decoded PCM, for analysis such as jbfmod's spectrum. Returns NULL
 * when the sound holds no decoded samples. */
const int16_t *gml_audio_sound_pcm16(GmlAudio *a,int sound,uint32_t *frames,int *channels);
int gml_audio_sound_format(GmlAudio *a,int sound,int *channels,
                           int *sample_rate,int *bytes_per_second);
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
