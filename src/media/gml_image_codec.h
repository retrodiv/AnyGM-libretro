/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_IMAGE_CODEC_H
#define GML_IMAGE_CODEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *data;
  size_t size;
} GmlMediaBuffer;

typedef enum {
  GML_DEFLATE_ZLIB = 0,
  GML_DEFLATE_RAW = 1
} GmlDeflateFraming;

/* Successful operations return a standard-heap buffer in out. The caller
 * releases it with gml_media_buffer_release or transfers both pointer and
 * ownership to another standard-heap owner. */
int gml_image_decode_rgba(const uint8_t *encoded, size_t encoded_size,
                          GmlMediaBuffer *out, int *width, int *height,
                          int *source_components);
int gml_image_encode_png(const uint8_t *pixels, int width, int height,
                         int components, int stride_bytes,
                         GmlMediaBuffer *out);

int gml_deflate_decode(const uint8_t *encoded, size_t encoded_size,
                       GmlDeflateFraming framing, GmlMediaBuffer *out);
int gml_deflate_decode_to_buffer(const uint8_t *encoded, size_t encoded_size,
                                 GmlDeflateFraming framing, uint8_t *decoded,
                                 size_t decoded_capacity, size_t *decoded_size);

void gml_media_buffer_release(GmlMediaBuffer *buffer);

#endif
