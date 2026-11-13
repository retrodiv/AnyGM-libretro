/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_VIDEO_PLAYER_H
#define GML_VIDEO_PLAYER_H

#include "gml_video_codec.h"
#include "gml_video_constants.h"

struct GmlAudio;
struct GmlRender;
typedef struct GmlVideoPlayer GmlVideoPlayer;
/* A value projection, not a native-memory wire layout. Pixels and queued PCM remain with their
 * existing owners; the decoder windows are derived from the immutable source and output counts. */
typedef struct GmlVideoPlayerState {
  uint32_t provider[5],picture_reads,sound_reads,flags,notifications;
  int32_t surface,queue,voice;
  uint64_t audio_cursor;
  double position,volume,image_end;
} GmlVideoPlayerState;
enum { GML_VIDEO_NOTIFY_START=1, GML_VIDEO_NOTIFY_END=2 };

/* Input bytes remain immutable and borrowed until close. The player owns its ordinary surface,
 * queue and voice; it borrows the engine's existing renderer and mixer. */
GmlVideoPlayer *gml_video_player_open(const uint8_t *bytes,size_t size,
                                     struct GmlRender *render,struct GmlAudio *audio);
void gml_video_player_close(GmlVideoPlayer *player);
void gml_video_player_tick(GmlVideoPlayer *player,double milliseconds);
void gml_video_player_pause(GmlVideoPlayer *player,int paused);
void gml_video_player_loop(GmlVideoPlayer *player,int enabled);
void gml_video_player_volume(GmlVideoPlayer *player,double volume);
int gml_video_player_status(const GmlVideoPlayer *player);
double gml_video_player_position(const GmlVideoPlayer *player);
double gml_video_player_duration(const GmlVideoPlayer *player);
/* Returns an ordinary RGBA surface, -1 on error/not ready, or -2 after a non-looping end. */
int gml_video_player_surface(GmlVideoPlayer *player);
unsigned gml_video_player_notifications(GmlVideoPlayer *player);
void gml_video_player_rebind(GmlVideoPlayer *player,struct GmlRender *render,struct GmlAudio *audio);
int gml_video_player_state_get(const GmlVideoPlayer *player,GmlVideoPlayerState *state);
GmlVideoPlayer *gml_video_player_state_restore(const uint8_t *bytes,size_t size,
                        struct GmlRender *render,struct GmlAudio *audio,
                        const GmlVideoPlayerState *state);
/* Called after both renderer and mixer sections have restored their resource identities. */
int gml_video_player_state_adopt(GmlVideoPlayer *player);
/* Metadata-only allowance for the movie surface and initial PCM window, before either exists. */
size_t gml_video_player_state_capacity_hint(const GmlVideoPlayer *player);

#endif
