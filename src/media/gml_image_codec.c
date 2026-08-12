/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_image_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBI_NO_STDIO
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC push_options
/* This bundled stb_image_write revision miscompiles its PNG compressor at
 * GCC -O2. Keep only the third-party implementation at its verified level. */
#pragma GCC optimize ("O1")
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC pop_options
#pragma GCC diagnostic pop
#endif

static void gml_media_buffer_reset(GmlMediaBuffer *buffer){
  if(buffer){
    buffer->data=NULL;
    buffer->size=0;
  }
}

void gml_media_buffer_release(GmlMediaBuffer *buffer){
  if(!buffer) return;
  free(buffer->data);
  gml_media_buffer_reset(buffer);
}

int gml_image_decode_rgba(const uint8_t *encoded, size_t encoded_size,
                          GmlMediaBuffer *out, int *width, int *height,
                          int *source_components){
  if(!out) return 0;
  gml_media_buffer_reset(out);
  if(width) *width=0;
  if(height) *height=0;
  if(source_components) *source_components=0;
  if(!encoded || encoded_size==0 || encoded_size>(size_t)INT_MAX) return 0;

  int decoded_width=0,decoded_height=0,components=0;
  uint8_t *pixels=stbi_load_from_memory(encoded,(int)encoded_size,
                                         &decoded_width,&decoded_height,
                                         &components,4);
  if(!pixels || decoded_width<=0 || decoded_height<=0 ||
     (size_t)decoded_width>SIZE_MAX/(size_t)decoded_height/4u){
    stbi_image_free(pixels);
    return 0;
  }
  out->data=pixels;
  out->size=(size_t)decoded_width*(size_t)decoded_height*4u;
  if(width) *width=decoded_width;
  if(height) *height=decoded_height;
  if(source_components) *source_components=components;
  return 1;
}

int gml_image_decode_rgba_frames(const uint8_t *encoded, size_t encoded_size,
                                 GmlMediaBuffer *out, int *width, int *height,
                                 int *frames, int *source_components){
  if(!out) return 0;
  gml_media_buffer_reset(out);
  if(width) *width=0;
  if(height) *height=0;
  if(frames) *frames=0;
  if(source_components) *source_components=0;
  if(!encoded || encoded_size==0 || encoded_size>(size_t)INT_MAX) return 0;

  int decoded_width=0,decoded_height=0,decoded_frames=1,components=0;
  uint8_t *pixels=NULL;
  int *delays=NULL;
  int gif=encoded_size>=6 &&
    (!memcmp(encoded,"GIF87a",6) || !memcmp(encoded,"GIF89a",6));
  if(gif)
    pixels=stbi_load_gif_from_memory(encoded,(int)encoded_size,&delays,
                                     &decoded_width,&decoded_height,
                                     &decoded_frames,&components,4);
  else
    pixels=stbi_load_from_memory(encoded,(int)encoded_size,
                                 &decoded_width,&decoded_height,&components,4);
  free(delays);
  if(!pixels || decoded_width<=0 || decoded_height<=0 || decoded_frames<=0 ||
     (size_t)decoded_width>SIZE_MAX/(size_t)decoded_height/4u ||
     (size_t)decoded_width*(size_t)decoded_height*4u>
       SIZE_MAX/(size_t)decoded_frames){
    stbi_image_free(pixels);
    return 0;
  }
  out->data=pixels;
  out->size=(size_t)decoded_width*(size_t)decoded_height*
            (size_t)decoded_frames*4u;
  if(width) *width=decoded_width;
  if(height) *height=decoded_height;
  if(frames) *frames=decoded_frames;
  if(source_components) *source_components=components;
  return 1;
}

int gml_image_encode_png(const uint8_t *pixels, int width, int height,
                         int components, int stride_bytes,
                         GmlMediaBuffer *out){
  if(!out) return 0;
  gml_media_buffer_reset(out);
  if(!pixels || width<=0 || height<=0 || components<1 || components>4 ||
     width>INT_MAX/components || stride_bytes<width*components) return 0;

  /* One fixed encoder profile makes output independent of the caller and
   * preserves the package writer's established bounded compression cost. */
  stbi_write_png_compression_level=1;
  int encoded_size=0;
  uint8_t *encoded=stbi_write_png_to_mem(pixels,stride_bytes,width,height,
                                         components,&encoded_size);
  if(!encoded || encoded_size<=0){
    free(encoded);
    return 0;
  }
  out->data=encoded;
  out->size=(size_t)encoded_size;
  return 1;
}

int gml_deflate_encode_zlib(const uint8_t *decoded, size_t decoded_size,
                            GmlMediaBuffer *out){
  static const uint8_t empty_zlib[]={
    0x78,0x01,0x01,0x00,0x00,0xff,0xff,0x00,0x00,0x00,0x01
  };
  if(!out) return 0;
  gml_media_buffer_reset(out);
  if((!decoded && decoded_size) || decoded_size>(size_t)INT_MAX) return 0;
  if(!decoded_size){
    out->data=(uint8_t*)malloc(sizeof(empty_zlib));
    if(!out->data) return 0;
    memcpy(out->data,empty_zlib,sizeof(empty_zlib));
    out->size=sizeof(empty_zlib);
    return 1;
  }
  int encoded_size=0;
  uint8_t *encoded=stbi_zlib_compress((uint8_t*)decoded,(int)decoded_size,
                                      &encoded_size,8);
  if(!encoded || encoded_size<=0){
    free(encoded);
    return 0;
  }
  out->data=encoded;
  out->size=(size_t)encoded_size;
  return 1;
}

int gml_deflate_decode(const uint8_t *encoded, size_t encoded_size,
                       GmlDeflateFraming framing, GmlMediaBuffer *out){
  if(!out) return 0;
  gml_media_buffer_reset(out);
  if(!encoded || encoded_size==0 || encoded_size>(size_t)INT_MAX ||
     (framing!=GML_DEFLATE_ZLIB && framing!=GML_DEFLATE_RAW)) return 0;

  int decoded_size=0;
  char *decoded=framing==GML_DEFLATE_RAW
    ? stbi_zlib_decode_noheader_malloc((const char*)encoded,(int)encoded_size,
                                       &decoded_size)
    : stbi_zlib_decode_malloc((const char*)encoded,(int)encoded_size,
                              &decoded_size);
  if(!decoded || decoded_size<0){
    STBI_FREE(decoded);
    return 0;
  }
  out->data=(uint8_t*)decoded;
  out->size=(size_t)decoded_size;
  return 1;
}

int gml_deflate_decode_to_buffer(const uint8_t *encoded, size_t encoded_size,
                                 GmlDeflateFraming framing, uint8_t *decoded,
                                 size_t decoded_capacity, size_t *decoded_size){
  if(decoded_size) *decoded_size=0;
  if(!encoded || !decoded || !decoded_size || encoded_size==0 ||
     encoded_size>(size_t)INT_MAX || decoded_capacity>(size_t)INT_MAX ||
     (framing!=GML_DEFLATE_ZLIB && framing!=GML_DEFLATE_RAW)) return 0;
  int result=framing==GML_DEFLATE_RAW
    ? stbi_zlib_decode_noheader_buffer((char*)decoded,(int)decoded_capacity,
                                       (const char*)encoded,(int)encoded_size)
    : stbi_zlib_decode_buffer((char*)decoded,(int)decoded_capacity,
                              (const char*)encoded,(int)encoded_size);
  if(result<0) return 0;
  *decoded_size=(size_t)result;
  return 1;
}
