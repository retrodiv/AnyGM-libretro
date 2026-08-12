/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_tracker.h — portable ProTracker MOD and FastTracker II XM playback.
 *
 * This decoder supports the jbfmod.dll extension interface without requiring a native library.
 * The engine renders a module ONCE to interleaved stereo S16 and records a row-accurate timeline,
 * so playback rides the ordinary sound mixer, savestates capture it like any other dynamic sound,
 * and every position or spectrum question is answered from data rather than from a live synth.
 * All sequencing and mixing is integer/fixed-point so two platforms render identical bytes.
 */
#ifndef GML_TRACKER_H
#define GML_TRACKER_H
#include <stdint.h>
#include <stddef.h>

typedef struct GmlTrackerModule GmlTrackerModule;

/* One row-start on the rendered timeline. */
typedef struct {
  uint32_t frame;      /* first output frame of this row */
  uint16_t order;      /* position in the order list */
  uint16_t pattern;    /* pattern played at that position */
  uint16_t row;
  uint8_t  speed;      /* ticks per row when the row started */
  uint8_t  bpm;
} GmlTrackerRowMark;

/* Parse a MOD or XM image. Returns NULL and writes a reason when the bytes are neither. */
GmlTrackerModule *gml_tracker_load(const uint8_t *data, size_t size, char *err, size_t errcap);
void gml_tracker_free(GmlTrackerModule *m);

/* Format facts, answerable straight after load. */
const char *gml_tracker_name(const GmlTrackerModule *m);
const char *gml_tracker_type(const GmlTrackerModule *m);       /* "MOD" or "XM" */
int gml_tracker_num_channels(const GmlTrackerModule *m);
int gml_tracker_num_orders(const GmlTrackerModule *m);
int gml_tracker_num_patterns(const GmlTrackerModule *m);
int gml_tracker_num_instruments(const GmlTrackerModule *m);
int gml_tracker_num_samples(const GmlTrackerModule *m);
int gml_tracker_pattern_rows(const GmlTrackerModule *m, int pattern);

/* Render the module from order 0 until its own sequence repeats or ends, at `rate` Hz.
 * On success *pcm holds malloc'd interleaved stereo S16 (caller frees), *frames its length,
 * *loop_frame the frame the song loops back to (or 0), and the row timeline is retained on the
 * module for the position getters. `pan_separation` is 0..128 as jbfmod's SetPanSeperation takes.
 * Returns 1 on success. Rendering is capped at twenty minutes so a pathological order loop cannot
 * exhaust memory; the cap is reported through the timeline simply ending there. */
int gml_tracker_render(GmlTrackerModule *m, int rate, int pan_separation,
                       int16_t **pcm, uint32_t *frames, uint32_t *loop_frame);

/* The retained timeline of the last render. */
const GmlTrackerRowMark *gml_tracker_timeline(const GmlTrackerModule *m, uint32_t *count);

/* One instrument trigger on the rendered timeline, for jbfmod's GetInstrumentPlayed family. */
typedef struct { uint32_t frame; uint16_t instrument; } GmlTrackerNoteEvent;
const GmlTrackerNoteEvent *gml_tracker_events(const GmlTrackerModule *m, uint32_t *count);

/* Integer Goertzel magnitude of one of `bands` spectrum bands, over the 1024 mono-folded frames
 * ending at `at_frame` of an interleaved stereo S16 render. Returns 0..65536 (16.16 of 0..1).
 * Pure integer arithmetic, so a spectrum-driven visual replays identically everywhere. */
uint32_t gml_tracker_spectrum(const int16_t *stereo, uint32_t frames,
                              uint32_t at_frame, int band, int bands);

#endif
