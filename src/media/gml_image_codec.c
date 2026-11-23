/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_image_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* The bundled stb implementations are third-party code, so their diagnostics are isolated here
 * rather than fixed in place. Each compiler needs its own spelling of the same request. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
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
/* The bundled writer warns against strict-aliasing optimization. GCC can
 * optimize its byte loops with alias analysis disabled; keep Clang's
 * established profile. The encoded-byte controls cover both profiles. */
#if defined(__clang__)
#pragma clang diagnostic pop
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang optimize off
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC push_options
#pragma GCC optimize ("O3,no-strict-aliasing")
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang optimize on
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#pragma GCC diagnostic pop
#endif

/* NSIS 2 uses ordinary raw Deflate except that a stored block carries LEN without NLEN. Keep the
 * variation at this shared inflate seam instead of teaching a container parser compression. */
static int gml_nsis_uncompressed_block(stbi__zbuf *stream){
  if(stream->num_bits&7) stbi__zreceive(stream,stream->num_bits&7);
  if(stream->num_bits<0) return 0;
  stbi_uc low,high;
#define GML_NSIS_ALIGNED_BYTE(target) do { \
    if(stream->num_bits>=8){ \
      (target)=(stbi_uc)(stream->code_buffer&255u); \
      stream->code_buffer>>=8; stream->num_bits-=8; \
    }else (target)=stbi__zget8(stream); \
  } while(0)
  GML_NSIS_ALIGNED_BYTE(low);
  GML_NSIS_ALIGNED_BYTE(high);
  int length=(int)low+((int)high<<8);
  if(stream->zout+length>stream->zout_end) return 0;
  while(length>0 && stream->num_bits>=8){
    *stream->zout++=(char)(stream->code_buffer&255u);
    stream->code_buffer>>=8;
    stream->num_bits-=8;
    length--;
  }
  if(stream->zbuffer+length>stream->zbuffer_end) return 0;
  memcpy(stream->zout,stream->zbuffer,(size_t)length);
  stream->zbuffer+=length;
  stream->zout+=length;
#undef GML_NSIS_ALIGNED_BYTE
  return 1;
}

static int gml_nsis_parse_deflate(stbi__zbuf *stream){
  int final;
  stream->num_bits=0;
  stream->code_buffer=0;
  stream->hit_zeof_once=0;
  do{
    final=stbi__zreceive(stream,1);
    int type=stbi__zreceive(stream,2);
    if(type==0){
      if(!gml_nsis_uncompressed_block(stream)) return 0;
    }else if(type==3) return 0;
    else{
      if(type==1){
        if(!stbi__zbuild_huffman(&stream->z_length,stbi__zdefault_length,STBI__ZNSYMS) ||
           !stbi__zbuild_huffman(&stream->z_distance,stbi__zdefault_distance,32)) return 0;
      }else if(!stbi__compute_huffman_codes(stream)) return 0;
      if(!stbi__parse_huffman_block(stream)) return 0;
    }
  }while(!final);
  return 1;
}

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

/* Packaged atlas pages are large 8-bit RGBA PNGs. Decoding them through the generic loader costs a
 * second full-size allocation and an extra pass, so this narrow form inflates straight into one
 * buffer and reconstructs its rows in place. It accepts only streams the generic loader accepts
 * with the same result and returns zero for everything else, which then takes the generic path. */
static uint32_t png_be32(const uint8_t *p){
  return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|(uint32_t)p[3];
}

/* The same Deflate block decoding as stbi__parse_huffman_block, with the same tables, for a
 * fixed output buffer with eight bytes of writable slack. Bits come from a 64-bit reservoir
 * refilled a word at a time, and matches are copied a word at a time. Implicit zero bits past the
 * input may be read ahead but never consumed, so this refuses every stream the generic decoder
 * refuses. */
typedef struct {
  const uint8_t *in,*end;
  uint64_t bits;
  unsigned count,implicit;
} PngBits;

static void png_refill(PngBits *b){
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
  if(b->end-b->in>=8){
    uint64_t word;
    memcpy(&word,b->in,8);
    b->bits|=word<<b->count;
    b->in+=(63u-b->count)>>3;
    b->count|=56u;
    return;
  }
#endif
  while(b->count<=56u && b->in<b->end){
    b->bits|=(uint64_t)*b->in++<<b->count;
    b->count+=8u;
  }
  if(b->count<48u){ b->implicit+=56u-b->count; b->count=56u; }
}

static int png_symbol(PngBits *b,const stbi__zhuffman *z){
  int entry=z->fast[b->bits&STBI__ZFAST_MASK];
  if(entry){
    b->bits>>=entry>>9;b->count-=(unsigned)(entry>>9);
    return entry&511;
  }
  int k=stbi__bit_reverse((int)(b->bits&0xffffu),16),size;
  for(size=STBI__ZFAST_BITS+1;;++size) if(k<z->maxcode[size]) break;
  if(size>=16) return -1;
  int index=(k>>(16-size))-z->firstcode[size]+z->firstsymbol[size];
  if(index>=STBI__ZNSYMS || z->size[index]!=size) return -1;
  b->bits>>=size;b->count-=(unsigned)size;
  return z->value[index];
}

static unsigned png_take(PngBits *b,int count){
  unsigned value=(unsigned)(b->bits&((1u<<count)-1u));
  b->bits>>=count;b->count-=(unsigned)count;
  return value;
}

static int png_huffman_block(stbi__zbuf *a){
  char *zout=a->zout;
  PngBits b={a->zbuffer,a->zbuffer_end,a->code_buffer,(unsigned)a->num_bits,0};
  if(a->hit_zeof_once || a->num_bits<0) return 0;
  for(;;){
    /* One refill covers a whole length and distance pair: at most 48 bits. */
    if(b.count<48u) png_refill(&b);
    if(b.implicit>b.count) return 0;
    int z=png_symbol(&b,&a->z_length);
    if(z<256){
      if(z<0 || zout>=a->zout_end) return 0;
      *zout++=(char)z;
      continue;
    }
    if(z==256) break;
    if(z>=286) return 0;
    z-=257;
    int len=stbi__zlength_base[z];
    if(stbi__zlength_extra[z]) len+=(int)png_take(&b,stbi__zlength_extra[z]);
    z=png_symbol(&b,&a->z_distance);
    if(z<0 || z>=30) return 0;
    int dist=stbi__zdist_base[z];
    if(stbi__zdist_extra[z]) dist+=(int)png_take(&b,stbi__zdist_extra[z]);
    if(zout-a->zout_start<dist || len>a->zout_end-zout) return 0;
    const char *from=zout-dist;
    if(dist==1) memset(zout,*from,(size_t)len);
    else if(dist>=8){
      /* Each word is read only after the bytes it covers were written. The final word may
       * write up to seven bytes past the match, which the slack absorbs and later output
       * replaces. */
      for(int done=0;done<len;done+=8){
        uint64_t word;
        memcpy(&word,from+done,8);
        memcpy(zout+done,&word,8);
      }
    } else {
      /* A short distance repeats a period of 2-7 bytes. Byte copies would each wait on the
       * previous store; words cut from the repeated period do not. */
      unsigned char period[16];
      int phase=0,step=8%dist;
      for(int i=0;i<16;i++) period[i]=(unsigned char)from[i%dist];
      for(int done=0;done<len;done+=8){
        memcpy(zout+done,period+phase,8);
        phase+=step;
        if(phase>=dist) phase-=dist;
      }
    }
    zout+=len;
  }
  /* Return the unconsumed real bits to the generic reader for the next block header. */
  if(b.implicit>b.count) return 0;
  unsigned real=b.count-b.implicit;
  a->zout=zout;
  a->zbuffer=(stbi_uc*)(b.in-(real>>3));
  a->num_bits=(int)(real&7u);
  a->code_buffer=(stbi__uint32)(b.bits&((1u<<(real&7u))-1u));
  return 1;
}

static int png_inflate(uint8_t *out,size_t size,const uint8_t *in,size_t in_size){
  stbi__zbuf a;
  int final;
  a.zbuffer=(stbi_uc*)in;a.zbuffer_end=(stbi_uc*)in+in_size;
  a.zout_start=a.zout=(char*)out;a.zout_end=(char*)out+size;
  a.z_expandable=0;
  if(!stbi__parse_zlib_header(&a)) return 0;
  a.num_bits=0;a.code_buffer=0;a.hit_zeof_once=0;
  do{
    final=stbi__zreceive(&a,1);
    int type=stbi__zreceive(&a,2);
    if(type==0){ if(!stbi__parse_uncompressed_block(&a)) return 0; }
    else if(type==3) return 0;
    else{
      if(type==1){
        if(!stbi__zbuild_huffman(&a.z_length,stbi__zdefault_length,STBI__ZNSYMS) ||
           !stbi__zbuild_huffman(&a.z_distance,stbi__zdefault_distance,32)) return 0;
      } else if(!stbi__compute_huffman_codes(&a)) return 0;
      if(!png_huffman_block(&a)) return 0;
    }
  }while(!final);
  return a.zout==a.zout_end;
}

static uint8_t *decode_png_rgba8(const uint8_t *encoded,size_t size,int *out_width,int *out_height){
  static const uint8_t signature[8]={0x89,'P','N','G','\r','\n',0x1a,'\n'};
  uint32_t width,height;
  size_t offset=33,compressed=0,chunks=0;
  const uint8_t *single=NULL;
  if(size<45 || memcmp(encoded,signature,8) || png_be32(encoded+8)!=13 ||
     memcmp(encoded+12,"IHDR",4)) return NULL;
  width=png_be32(encoded+16);height=png_be32(encoded+20);
  /* Bit depth 8, colour type 6, deflate, adaptive filtering, no interlace. */
  if(!width || !height || encoded[24]!=8 || encoded[25]!=6 || encoded[26] || encoded[27] ||
     encoded[28] || (uint64_t)width*height*4u>=(1u<<30)) return NULL;
  for(;;){
    uint32_t length,type;
    if(size-offset<12) return NULL;
    length=png_be32(encoded+offset);
    if(length>(1u<<30) || length>size-offset-12) return NULL;
    type=png_be32(encoded+offset+4);
    if(type==0x49444154u){                                    /* IDAT */
      if(!chunks++) single=encoded+offset+8;
      compressed+=length;
    } else if(type==0x49454e44u) break;                        /* IEND */
    /* Any other critical chunk, and transparency, keep the generic loader's own decision. */
    else if(!(type&(1u<<29)) || type==0x74524e53u) return NULL;
    offset+=12u+length;
  }
  if(!chunks || compressed>INT_MAX) return NULL;
  size_t row=(size_t)width*4u,raw_size=(row+1u)*height;
  if(raw_size>INT_MAX) return NULL;
  uint8_t *joined=NULL;
  const uint8_t *stream=single;
  if(chunks>1){
    joined=(uint8_t*)malloc(compressed?compressed:1u);
    if(!joined) return NULL;
    size_t at=0;
    for(size_t chunk=33;;){
      uint32_t length=png_be32(encoded+chunk),type=png_be32(encoded+chunk+4);
      if(type==0x49454e44u) break;
      if(type==0x49444154u){memcpy(joined+at,encoded+chunk+8,length);at+=length;}
      chunk+=12u+length;
    }
    stream=joined;
  }
  uint8_t *raw=(uint8_t*)malloc(raw_size+8u);
  int inflated=raw && png_inflate(raw,raw_size,stream,compressed);
  free(joined);
  if(!inflated){free(raw);return NULL;}
  /* Row y's filter byte and data start y+1 bytes after its packed position, so a forward pass
   * reads each input byte before the packed output can overwrite it. */
  for(uint32_t y=0;y<height;y++){
    const uint8_t *in=raw+(size_t)y*(row+1u);
    uint8_t filter=in[0];
    uint8_t *out=raw+(size_t)y*row;
    const uint8_t *prior=y?out-row:NULL;
    in++;
    if(filter>4){free(raw);return NULL;}
    /* The first row predicts from zero: up becomes none, and Paeth reduces to left. */
    if(!prior) filter=filter==2?0:filter==4?1:filter;
    if(filter==0){ memmove(out,in,row); continue; }
    /* The first pixel predicts from zero on its left. */
    for(size_t i=0;i<4;i++){
      int up=prior?prior[i]:0;
      out[i]=(uint8_t)(in[i]+(filter==1?0:filter==3?up>>1:up));
    }
    switch(filter){
    case 1: for(size_t i=4;i<row;i++) out[i]=(uint8_t)(in[i]+out[i-4]); break;
    case 2: for(size_t i=4;i<row;i++) out[i]=(uint8_t)(in[i]+prior[i]); break;
    case 3:
      if(prior) for(size_t i=4;i<row;i++) out[i]=(uint8_t)(in[i]+((out[i-4]+prior[i])>>1));
      else for(size_t i=4;i<row;i++) out[i]=(uint8_t)(in[i]+(out[i-4]>>1));
      break;
    default:
      for(size_t i=4;i<row;i++) out[i]=(uint8_t)(in[i]+stbi__paeth(out[i-4],prior[i],prior[i-4]));
      break;
    }
  }
  *out_width=(int)width;*out_height=(int)height;
  return raw;
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
  uint8_t *direct=decode_png_rgba8(encoded,encoded_size,&decoded_width,&decoded_height);
  if(direct){
    out->data=direct;
    out->size=(size_t)decoded_width*(size_t)decoded_height*4u;
    if(width) *width=decoded_width;
    if(height) *height=decoded_height;
    if(source_components) *source_components=4;
    return 1;
  }
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

int gml_deflate_decode_nsis_to_buffer(const uint8_t *encoded,size_t encoded_size,
                                      uint8_t *decoded,size_t decoded_capacity,
                                      size_t *decoded_size){
  if(decoded_size) *decoded_size=0;
  if(!encoded || !decoded || !decoded_size || !encoded_size ||
     encoded_size>(size_t)INT_MAX || decoded_capacity>(size_t)INT_MAX) return 0;
  stbi__zbuf stream;
  memset(&stream,0,sizeof stream);
  stream.zbuffer=(stbi_uc *)encoded;
  stream.zbuffer_end=(stbi_uc *)encoded+encoded_size;
  stream.zout_start=(char *)decoded;
  stream.zout=(char *)decoded;
  stream.zout_end=(char *)decoded+decoded_capacity;
  stream.z_expandable=0;
  if(!gml_nsis_parse_deflate(&stream)) return 0;
  *decoded_size=(size_t)(stream.zout-(char *)decoded);
  return 1;
}
