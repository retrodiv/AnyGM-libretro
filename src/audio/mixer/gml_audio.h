/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_audio.h — software mixer for AUDO/SOND PCM and decoded OGG sounds. */
#ifndef GML_AUDIO_H
#define GML_AUDIO_H
#include <stdint.h>
#include "gml_win.h"

typedef struct GmlAudio GmlAudio;

GmlAudio *gml_audio_create(GmlWin *win);
void gml_audio_free(GmlAudio *a);

/* play sound resource `snd` (SOND index); loop!=0 repeats. */
void gml_audio_play(GmlAudio *a, int snd, int loop);
void gml_audio_stop(GmlAudio *a, int snd);   /* stop every voice of this sound */
void gml_audio_stop_all(GmlAudio *a);
void gml_audio_pause_all(GmlAudio *a, int paused);  /* paused!=0 freezes all voices (keep position) */
int  gml_audio_is_playing(GmlAudio *a, int snd);

/* mix `frames` stereo (interleaved L,R) int16 samples at 44100 Hz into out. */
void gml_audio_mix(GmlAudio *a, int16_t *out, int frames);

#endif
