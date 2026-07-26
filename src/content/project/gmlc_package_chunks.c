/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_package_internal.h"

#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_win.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reserve(Buf *b, size_t n){
  if(b->len+n<=b->cap) return 1;
  size_t nc=b->cap?b->cap*2:4096;
  while(b->len+n>nc) nc*=2;
  uint8_t *nd=(uint8_t*)realloc(b->data,nc);
  if(!nd) return 0;
  b->data=nd; b->cap=nc;
  return 1;
}

int wbytes(Buf *b, const void *p, size_t n){
  if(n==0) return 1;
  if(!reserve(b,n)) return 0;
  memcpy(b->data+b->len,p,n);
  b->len+=n;
  return 1;
}

int wu8(Buf *b, uint8_t v){ return wbytes(b,&v,1); }
int wu16(Buf *b, uint16_t v){ uint8_t x[2]={(uint8_t)v,(uint8_t)(v>>8)}; return wbytes(b,x,2); }
int wu32(Buf *b, uint32_t v){ uint8_t x[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; return wbytes(b,x,4); }
static int wu64(Buf *b, uint64_t v){ return wu32(b,(uint32_t)v) && wu32(b,(uint32_t)(v>>32)); }
int wi32(Buf *b, int32_t v){ return wu32(b,(uint32_t)v); }
int wf32(Buf *b, float f){ uint32_t u; memcpy(&u,&f,4); return wu32(b,u); }
int zfill(Buf *b, size_t n){ if(!reserve(b,n)) return 0; memset(b->data+b->len,0,n); b->len+=n; return 1; }
void patch32(Buf *b, size_t pos, uint32_t v){ b->data[pos]=(uint8_t)v; b->data[pos+1]=(uint8_t)(v>>8); b->data[pos+2]=(uint8_t)(v>>16); b->data[pos+3]=(uint8_t)(v>>24); }
static void patch64(Buf *b, size_t pos, uint64_t v){ patch32(b,pos,(uint32_t)v); patch32(b,pos+4,(uint32_t)(v>>32)); }

static uint64_t string_hash(const char *s){
  uint64_t hash=1469598103934665603ull;
  for(;*s;s++){
    hash^=(uint8_t)*s;
    hash*=1099511628211ull;
  }
  return hash;
}

static int strtab_rehash(StrTab *table, int capacity){
  int *slots=(int*)malloc((size_t)capacity*sizeof(*slots));
  if(!slots) return 0;
  for(int i=0;i<capacity;i++) slots[i]=-1;
  for(int i=0;i<table->n;i++){
    size_t at=(size_t)string_hash(table->items[i])&((size_t)capacity-1u);
    while(slots[at]>=0) at=(at+1u)&((size_t)capacity-1u);
    slots[at]=i;
  }
  free(table->hash_slots);
  table->hash_slots=slots;
  table->hash_cap=capacity;
  return 1;
}

int intern(Pkg *p, const char *s){
  if(!s) s="";
  if(!p->strs.hash_cap){
    if(!strtab_rehash(&p->strs,256)) return -1;
  } else if((int64_t)(p->strs.n+1)*10>=(int64_t)p->strs.hash_cap*7){
    if(p->strs.hash_cap>INT_MAX/2 || !strtab_rehash(&p->strs,p->strs.hash_cap*2)) return -1;
  }
  size_t slot=(size_t)string_hash(s)&((size_t)p->strs.hash_cap-1u);
  while(p->strs.hash_slots[slot]>=0){
    int index=p->strs.hash_slots[slot];
    if(!strcmp(p->strs.items[index],s)) return index;
    slot=(slot+1u)&((size_t)p->strs.hash_cap-1u);
  }
  if(p->strs.n>=p->strs.cap){
    int nc=p->strs.cap?p->strs.cap*2:128;
    char **ni=(char**)malloc((size_t)nc*sizeof(*ni));
    uint32_t *no=(uint32_t*)malloc((size_t)nc*sizeof(*no));
    if(!ni || !no){ free(ni); free(no); return -1; }
    if(p->strs.n){
      memcpy(ni,p->strs.items,(size_t)p->strs.n*sizeof(*ni));
      memcpy(no,p->strs.char_off,(size_t)p->strs.n*sizeof(*no));
    }
    free(p->strs.items);
    free(p->strs.char_off);
    p->strs.items=ni; p->strs.char_off=no; p->strs.cap=nc;
  }
  int index=p->strs.n;
  p->strs.items[index]=gmlc_strdup(s);
  p->strs.char_off[index]=0;
  if(!p->strs.items[index]) return -1;
  p->strs.hash_slots[slot]=index;
  p->strs.n++;
  return index;
}

static int add_str_patch(Pkg *p, uint32_t pos, int sid){
  if(p->n_patches>=p->cap_patches){
    int nc=p->cap_patches?p->cap_patches*2:128;
    StrPatch *np=(StrPatch*)realloc(p->patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->patches=np; p->cap_patches=nc;
  }
  p->patches[p->n_patches].pos=pos;
  p->patches[p->n_patches].sid=sid;
  p->n_patches++;
  return 1;
}

int add_frame_patch(Pkg *p, uint32_t pos, int frame){
  if(p->n_frame_patches>=p->cap_frame_patches){
    int nc=p->cap_frame_patches?p->cap_frame_patches*2:128;
    FramePatch *np=(FramePatch*)realloc(p->frame_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->frame_patches=np; p->cap_frame_patches=nc;
  }
  p->frame_patches[p->n_frame_patches].pos=pos;
  p->frame_patches[p->n_frame_patches].frame=frame;
  p->n_frame_patches++;
  return 1;
}

static int add_font_patch(Pkg *p, uint32_t pos, int font){
  if(p->n_font_patches>=p->cap_font_patches){
    int nc=p->cap_font_patches?p->cap_font_patches*2:16;
    FontPatch *np=(FontPatch*)realloc(p->font_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->font_patches=np; p->cap_font_patches=nc;
  }
  p->font_patches[p->n_font_patches].pos=pos;
  p->font_patches[p->n_font_patches].font=font;
  p->n_font_patches++;
  return 1;
}

int wstrptr(Pkg *p, int sid){
  uint32_t pos=(uint32_t)p->b.len;
  if(!wu32(&p->b,0)) return 0;
  return add_str_patch(p,pos,sid);
}

size_t chunk_begin(Pkg *p, const char name[4]){
  wbytes(&p->b,name,4);
  size_t szpos=p->b.len;
  wu32(&p->b,0);
  return szpos;
}

void chunk_end(Pkg *p, size_t szpos){
  patch32(&p->b,szpos,(uint32_t)(p->b.len-(szpos+4)));
}

int empty_list_chunk(Pkg *p, const char name[4]){
  size_t s=chunk_begin(p,name);
  if(!wu32(&p->b,0)) return 0;
  chunk_end(p,s);
  return 1;
}

int read_blob(const GmlcProject *project, const char *path,
                     uint8_t **out, size_t *out_len){
  const GmlcMemoryFile *memory=gmlc_project_find_memory_file(project,path);
  if(memory){
    if(memory->kind==GMLC_MEMORY_TEXT || !memory->size) return 0;
    uint8_t *copy=(uint8_t*)malloc(memory->size);
    if(!copy) return 0;
    memcpy(copy,memory->data,memory->size);
    *out=copy; *out_len=memory->size;
    return 1;
  }
  return project&&path&&anygm_vfs_read_all(project->host,path,out,out_len,(size_t)UINT32_MAX);
}

int write_sond(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SOND");
  uint32_t n=(uint32_t)p->n_sounds;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSound *snd=&p->sounds[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid_name=intern(pkg,snd->name);
    int sid_file=intern(pkg,snd->name);
    wstrptr(pkg,sid_name);
    wu32(&pkg->b,0x65);
    wu32(&pkg->b,0);
    wstrptr(pkg,sid_file);
    wu32(&pkg->b,0);
    wf32(&pkg->b,snd->volume);
    wf32(&pkg->b,snd->pitch==0.0f?1.0f:snd->pitch);
    wi32(&pkg->b,0);
    wi32(&pkg->b,(int32_t)i);
  }
  chunk_end(pkg,s);
  return 1;
}

int write_scpt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SCPT");
  uint32_t n=(uint32_t)p->n_scripts;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  int code_base=room_code_count(p);
  for(uint32_t i=0;i<n;i++){
    const GmlcScript *sc=&p->scripts[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,sc->name);
    wstrptr(pkg,sid);
    wi32(&pkg->b,code_base+(int)i);
  }
  chunk_end(pkg,s);
  return 1;
}

/* GLSL preambles provide matrix, lighting, fog and alpha-test interfaces. */
static const char gms2_glsles_vertex_prefix[] =
  "#define LOWPREC lowp\n"
  "#define MATRIX_VIEW 0\n"
  "#define MATRIX_PROJECTION 1\n"
  "#define MATRIX_WORLD 2\n"
  "#define MATRIX_WORLD_VIEW 3\n"
  "#define MATRIX_WORLD_VIEW_PROJECTION 4\n"
  "#define MATRICES_MAX 5\n"
  "#define MAX_VS_LIGHTS 8\n"
  "\n"
  "uniform mat4 gm_Matrices[MATRICES_MAX];\n"
  "uniform bool gm_LightingEnabled;\n"
  "uniform bool gm_VS_FogEnabled;\n"
  "uniform float gm_FogStart;\n"
  "uniform float gm_RcpFogRange;\n"
  "uniform vec4 gm_AmbientColour;\n"
  "uniform vec4 gm_Lights_Direction[MAX_VS_LIGHTS];\n"
  "uniform vec4 gm_Lights_PosRange[MAX_VS_LIGHTS];\n"
  "uniform vec4 gm_Lights_Colour[MAX_VS_LIGHTS];\n"
  "\n"
  "float CalcFogFactor(vec4 pos)\n"
  "{\n"
  "  if (!gm_VS_FogEnabled) return 0.0;\n"
  "  vec4 viewpos = gm_Matrices[MATRIX_WORLD_VIEW] * pos;\n"
  "  return (viewpos.z - gm_FogStart) * gm_RcpFogRange;\n"
  "}\n"
  "\n"
  "vec4 DoDirLight(vec3 normal, vec4 direction, vec4 tint)\n"
  "{\n"
  "  return max(0.0, dot(normal, direction.xyz)) * tint;\n"
  "}\n"
  "\n"
  "vec4 DoPointLight(vec3 position, vec3 normal, vec4 posrange, vec4 tint)\n"
  "{\n"
  "  vec3 offset = position - posrange.xyz;\n"
  "  float dist = length(offset);\n"
  "  float atten = dist > posrange.w ? 0.0 : posrange.w / dist;\n"
  "  return max(0.0, dot(normal, offset / dist)) * atten * tint;\n"
  "}\n"
  "\n"
  "vec4 DoLighting(vec4 base, vec4 pos, vec3 srcnormal)\n"
  "{\n"
  "  if (!gm_LightingEnabled) return base;\n"
  "  vec3 normal = -normalize((gm_Matrices[MATRIX_WORLD_VIEW] * vec4(srcnormal, 0.0)).xyz);\n"
  "  vec3 position = (gm_Matrices[MATRIX_WORLD] * pos).xyz;\n"
  "  vec4 lit = vec4(0.0);\n"
  "  for (int i = 0; i < MAX_VS_LIGHTS; i++)\n"
  "    lit += DoDirLight(normal, gm_Lights_Direction[i], gm_Lights_Colour[i]);\n"
  "  for (int i = 0; i < MAX_VS_LIGHTS; i++)\n"
  "    lit += DoPointLight(position, normal, gm_Lights_PosRange[i], gm_Lights_Colour[i]);\n"
  "  return min(vec4(1.0), lit * base + gm_AmbientColour);\n"
  "}\n"
  "\n"
  "#define _YY_GLSLES_ 1\n";

static const char gms2_glsl_vertex_prefix[] =
  "#version 120\n"
  "#define LOWPREC\n"
  "#define MATRIX_VIEW 0\n"
  "#define MATRIX_PROJECTION 1\n"
  "#define MATRIX_WORLD 2\n"
  "#define MATRIX_WORLD_VIEW 3\n"
  "#define MATRIX_WORLD_VIEW_PROJECTION 4\n"
  "#define MATRICES_MAX 5\n"
  "#define MAX_VS_LIGHTS 8\n"
  "\n"
  "uniform mat4 gm_Matrices[MATRICES_MAX];\n"
  "uniform bool gm_LightingEnabled;\n"
  "uniform bool gm_VS_FogEnabled;\n"
  "uniform float gm_FogStart;\n"
  "uniform float gm_RcpFogRange;\n"
  "uniform vec4 gm_AmbientColour;\n"
  "uniform vec4 gm_Lights_Direction[MAX_VS_LIGHTS];\n"
  "uniform vec4 gm_Lights_PosRange[MAX_VS_LIGHTS];\n"
  "uniform vec4 gm_Lights_Colour[MAX_VS_LIGHTS];\n"
  "\n"
  "float CalcFogFactor(vec4 pos)\n"
  "{\n"
  "  if (!gm_VS_FogEnabled) return 0.0;\n"
  "  vec4 viewpos = gm_Matrices[MATRIX_WORLD_VIEW] * pos;\n"
  "  return (viewpos.z - gm_FogStart) * gm_RcpFogRange;\n"
  "}\n"
  "\n"
  "vec4 DoDirLight(vec3 normal, vec4 direction, vec4 tint)\n"
  "{\n"
  "  return max(0.0, dot(normal, direction.xyz)) * tint;\n"
  "}\n"
  "\n"
  "vec4 DoPointLight(vec3 position, vec3 normal, vec4 posrange, vec4 tint)\n"
  "{\n"
  "  vec3 offset = position - posrange.xyz;\n"
  "  float dist = length(offset);\n"
  "  float atten = dist > posrange.w ? 0.0 : posrange.w / dist;\n"
  "  return max(0.0, dot(normal, offset / dist)) * atten * tint;\n"
  "}\n"
  "\n"
  "vec4 DoLighting(vec4 base, vec4 pos, vec3 srcnormal)\n"
  "{\n"
  "  if (!gm_LightingEnabled) return base;\n"
  "  vec3 normal = -normalize((gm_Matrices[MATRIX_WORLD_VIEW] * vec4(srcnormal, 0.0)).xyz);\n"
  "  vec3 position = (gm_Matrices[MATRIX_WORLD] * pos).xyz;\n"
  "  vec4 lit = vec4(0.0);\n"
  "  for (int i = 0; i < MAX_VS_LIGHTS; i++)\n"
  "    lit += DoDirLight(normal, gm_Lights_Direction[i], gm_Lights_Colour[i]);\n"
  "  for (int i = 0; i < MAX_VS_LIGHTS; i++)\n"
  "    lit += DoPointLight(position, normal, gm_Lights_PosRange[i], gm_Lights_Colour[i]);\n"
  "  return min(vec4(1.0), lit * base + gm_AmbientColour);\n"
  "}\n"
  "\n"
  "#define _YY_GLSL_ 1\n";

static const char gms2_glsles_fragment_prefix[] =
  "precision mediump float;\n"
  "#define LOWPREC lowp\n"
  "uniform sampler2D gm_BaseTexture;\n"
  "uniform bool gm_PS_FogEnabled;\n"
  "uniform vec4 gm_FogColour;\n"
  "uniform bool gm_AlphaTestEnabled;\n"
  "uniform float gm_AlphaRefValue;\n"
  "\n"
  "void DoAlphaTest(vec4 colour)\n"
  "{\n"
  "  if (gm_AlphaTestEnabled && colour.a <= gm_AlphaRefValue) discard;\n"
  "}\n"
  "\n"
  "void DoFog(inout vec4 colour, float amount)\n"
  "{\n"
  "  if (gm_PS_FogEnabled) colour = mix(colour, gm_FogColour, clamp(amount, 0.0, 1.0));\n"
  "}\n"
  "\n"
  "#define _YY_GLSLES_ 1\n";

static const char gms2_glsl_fragment_prefix[] =
  "#version 120\n"
  "#define LOWPREC\n"
  "uniform sampler2D gm_BaseTexture;\n"
  "uniform bool gm_PS_FogEnabled;\n"
  "uniform vec4 gm_FogColour;\n"
  "uniform bool gm_AlphaTestEnabled;\n"
  "uniform float gm_AlphaRefValue;\n"
  "\n"
  "void DoAlphaTest(vec4 colour)\n"
  "{\n"
  "  if (gm_AlphaTestEnabled && colour.a <= gm_AlphaRefValue) discard;\n"
  "}\n"
  "\n"
  "void DoFog(inout vec4 colour, float amount)\n"
  "{\n"
  "  if (gm_PS_FogEnabled) colour = mix(colour, gm_FogColour, clamp(amount, 0.0, 1.0));\n"
  "}\n"
  "\n"
  "#define _YY_GLSL_ 1\n";

static char *join2(const char *a, const char *b){
  size_t na=strlen(a?a:""), nb=strlen(b?b:"");
  char *out=(char*)malloc(na+nb+1);
  if(!out) return NULL;
  memcpy(out,a?a:"",na);
  memcpy(out+na,b?b:"",nb);
  out[na+nb]=0;
  return out;
}

static char *join3(const char *a, const char *b, const char *c){
  char *ab=join2(a,b);
  if(!ab) return NULL;
  char *out=join2(ab,c);
  free(ab);
  return out;
}

static char *with_crlf_first_newlines(const char *s, int count){
  if(!s) return gmlc_strdup("");
  size_t extra=0;
  int seen=0;
  for(size_t i=0;s[i];i++){
    if(s[i]=='\n'){
      if(seen<count && (i==0 || s[i-1]!='\r')) extra++;
      seen++;
    }
  }
  size_t n=strlen(s);
  char *out=(char*)malloc(n+extra+1);
  if(!out) return NULL;
  size_t j=0;
  seen=0;
  for(size_t i=0;i<n;i++){
    if(s[i]=='\n'){
      if(seen<count && (i==0 || s[i-1]!='\r')) out[j++]='\r';
      seen++;
    }
    out[j++]=s[i];
  }
  out[j]=0;
  return out;
}

/* HLSL entries provide fixed vertex layouts and limited fragment variants. */
static const char gms2_hlsl_vertex_shared[] =
  "#define MATRIX_VIEW 0\n"
  "#define MATRIX_PROJECTION 1\n"
  "#define MATRIX_WORLD 2\n"
  "#define MATRIX_WORLD_VIEW 3\n"
  "#define MATRIX_WORLD_VIEW_PROJECTION 4\n"
  "#define MATRICES_MAX 5\n"
  "#define MAX_VS_LIGHTS 8\n"
  "\n"
  "float4x4 gm_Matrices[MATRICES_MAX] : register(c0);\n"
  "bool gm_LightingEnabled;\n"
  "bool gm_VS_FogEnabled;\n"
  "float gm_FogStart;\n"
  "float gm_RcpFogRange;\n"
  "float4 gm_AmbientColour;\n"
  "float3 gm_Lights_Direction[MAX_VS_LIGHTS];\n"
  "float4 gm_Lights_PosRange[MAX_VS_LIGHTS];\n"
  "float4 gm_Lights_Colour[MAX_VS_LIGHTS];\n"
  "uniform float4 dx_ViewAdjust : register(c1);\n"
  "\n"
  "struct VS_INPUT\n"
  "{\n"
  "  float4 colour : COLOR0;\n"
  "  float3 position : POSITION;\n"
  "  float2 texcoord : TEXCOORD0;\n"
  "};\n";

static const char gms2_hlsl_vertex_color_tail[] =
  "\n"
  "struct VS_OUTPUT\n"
  "{\n"
  "  float4 position : POSITION;\n"
  "  float4 colour : TEXCOORD0;\n"
  "  float2 texcoord : TEXCOORD1;\n"
  "};\n"
  "\n"
  "VS_OUTPUT main(VS_INPUT input)\n"
  "{\n"
  "  VS_OUTPUT output;\n"
  "  output.position = mul(transpose(gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION]), float4(input.position, 1.0));\n"
  "  output.colour = input.colour;\n"
  "  output.texcoord = input.texcoord;\n"
  "  return output;\n"
  "}\n";

static const char gms2_hlsl_vertex_texcoord_tail[] =
  "\n"
  "struct VS_OUTPUT\n"
  "{\n"
  "  float4 position : POSITION;\n"
  "  float2 texcoord : TEXCOORD0;\n"
  "};\n"
  "\n"
  "VS_OUTPUT main(VS_INPUT input)\n"
  "{\n"
  "  VS_OUTPUT output;\n"
  "  output.position = mul(transpose(gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION]), float4(input.position, 1.0));\n"
  "  output.texcoord = input.texcoord;\n"
  "  return output;\n"
  "}\n";

static const char gms2_hlsl_fragment_color_prefix[] =
  "sampler2D gm_BaseTexture : register(s0);\n"
  "bool gm_PS_FogEnabled;\n"
  "float4 gm_FogColour;\n"
  "bool gm_AlphaTestEnabled;\n"
  "float gm_AlphaRefValue;\n"
  "\n"
  "struct PS_INPUT\n"
  "{\n"
  "  float4 colour : TEXCOORD0;\n"
  "  float2 texcoord : TEXCOORD1;\n"
  "};\n"
  "\n"
  "struct PS_OUTPUT\n"
  "{\n"
  "  float4 colour : COLOR0;\n"
  "};\n"
  "\n"
  "PS_OUTPUT main(PS_INPUT input)\n"
  "{\n"
  "  float4 shaded = input.colour * tex2D(gm_BaseTexture, input.texcoord);\n";

static const char gms2_hlsl_fragment_color_suffix[] =
  "  PS_OUTPUT output;\n"
  "  output.colour = shaded;\n"
  "  return output;\n"
  "}\n";

static const char gms2_hlsl_fragment_gray[] =
  "sampler2D gm_BaseTexture : register(s0);\n"
  "\n"
  "struct PS_INPUT\n"
  "{\n"
  "  float2 texcoord : TEXCOORD0;\n"
  "};\n"
  "\n"
  "struct PS_OUTPUT\n"
  "{\n"
  "  float4 colour : COLOR0;\n"
  "};\n"
  "\n"
  "PS_OUTPUT main(PS_INPUT input)\n"
  "{\n"
  "  float4 texel = tex2D(gm_BaseTexture, input.texcoord);\n"
  "  float level = (texel.r + texel.g + texel.b) / 3.0;\n"
  "  PS_OUTPUT output;\n"
  "  output.colour = float4(level, level, level, 1.0);\n"
  "  return output;\n"
  "}\n";

static const char *hlsl_zero_swizzle(const char *fragment_source){
  if(!fragment_source) return NULL;
  if(strstr(fragment_source,"gl_FragColor.br")) return "(gl_Color[0].zx = float2(0.0, 0.0));";
  if(strstr(fragment_source,"gl_FragColor.bg")) return "(gl_Color[0].zy = float2(0.0, 0.0));";
  if(strstr(fragment_source,"gl_FragColor.gr")) return "(gl_Color[0].yx = float2(0.0, 0.0));";
  return NULL;
}

static int shader_looks_grayscale(const char *fragment_source){
  return fragment_source &&
         strstr(fragment_source,"original_color") &&
         strstr(fragment_source,"average") &&
         strstr(fragment_source,"gl_FragColor = new_color");
}

int write_shdr(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SHDR");
  uint32_t n=(uint32_t)p->n_shaders;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  int attr_pos=-1, attr_col=-1, attr_tex=-1;
  for(uint32_t i=0;i<n;i++){
    const GmlcShader *sh=&p->shaders[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    char *v_gles=join2(gms2_glsles_vertex_prefix,sh->vertex_source?sh->vertex_source:"");
    char *f_gles=join2(gms2_glsles_fragment_prefix,sh->fragment_source?sh->fragment_source:"");
    char *v_gl=join2(gms2_glsl_vertex_prefix,sh->vertex_source?sh->vertex_source:"");
    char *f_gl=join2(gms2_glsl_fragment_prefix,sh->fragment_source?sh->fragment_source:"");
    int hlsl_gray=shader_looks_grayscale(sh->fragment_source);
    int hlsl_uses_color=!hlsl_gray && sh->fragment_source && strstr(sh->fragment_source,"v_vColour");
    char *v_hlsl_lf=join2(gms2_hlsl_vertex_shared,hlsl_uses_color?gms2_hlsl_vertex_color_tail:gms2_hlsl_vertex_texcoord_tail);
    char *f_hlsl_lf=NULL;
    const char *swiz=hlsl_zero_swizzle(sh->fragment_source);
    if(hlsl_gray){
      f_hlsl_lf=join2("",gms2_hlsl_fragment_gray);
    } else if(swiz){
      f_hlsl_lf=join3(gms2_hlsl_fragment_color_prefix,swiz,gms2_hlsl_fragment_color_suffix);
    } else {
      f_hlsl_lf=join2("",sh->fragment_source?sh->fragment_source:"");
    }
    char *v_hlsl=with_crlf_first_newlines(v_hlsl_lf,20);
    char *f_hlsl=with_crlf_first_newlines(f_hlsl_lf,8);
    free(v_hlsl_lf); free(f_hlsl_lf);
    if(!v_gles || !f_gles || !v_gl || !f_gl || !v_hlsl || !f_hlsl){
      free(v_gles); free(f_gles); free(v_gl); free(f_gl); free(v_hlsl); free(f_hlsl);
      return 0;
    }
    int sid=intern(pkg,sh->name?sh->name:"");
    int vgles_sid=intern(pkg,v_gles);
    int fgles_sid=intern(pkg,f_gles);
    int vgl_sid=intern(pkg,v_gl);
    int fgl_sid=intern(pkg,f_gl);
    int vhlsl_sid=intern(pkg,v_hlsl);
    int fhlsl_sid=intern(pkg,f_hlsl);
    free(v_gles); free(f_gles); free(v_gl); free(f_gl); free(v_hlsl); free(f_hlsl);
    if(sid<0 || vgles_sid<0 || fgles_sid<0 || vgl_sid<0 || fgl_sid<0 || vhlsl_sid<0 || fhlsl_sid<0) return 0;
    if(attr_pos<0){
      attr_pos=intern(pkg,"in_Position");
      attr_col=intern(pkg,"in_Colour");
      attr_tex=intern(pkg,"in_TextureCoord");
      if(attr_pos<0 || attr_col<0 || attr_tex<0) return 0;
    }
    wstrptr(pkg,sid);
    wu32(&pkg->b,0x80000001u);
    wstrptr(pkg,vgles_sid);
    wstrptr(pkg,fgles_sid);
    wstrptr(pkg,vgl_sid);
    wstrptr(pkg,fgl_sid);
    wstrptr(pkg,vhlsl_sid);
    wstrptr(pkg,fhlsl_sid);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,3);
    wstrptr(pkg,attr_pos);
    wstrptr(pkg,attr_col);
    wstrptr(pkg,attr_tex);
    wi32(&pkg->b,2);
    for(int j=0;j<6;j++){
      wu32(&pkg->b,0);
      wu32(&pkg->b,0);
    }
  }
  chunk_end(pkg,s);
  return 1;
}

int write_tmln(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"TMLN");
  wu32(&pkg->b,(uint32_t)p->n_timelines);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)p->n_timelines*4);
  for(int i=0;i<p->n_timelines;i++){
    const GmlcTimeline *timeline=&p->timelines[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,timeline->name?timeline->name:"");
    wstrptr(pkg,sid);
    wu32(&pkg->b,0x434C4D54u); /* TMLC: controlled compiler timeline record */
    wu32(&pkg->b,(uint32_t)timeline->n_moments);
    for(int m=0;m<timeline->n_moments;m++){
      wi32(&pkg->b,timeline->moments[m].step);
      wi32(&pkg->b,timeline_moment_code_index(p,i,m));
    }
  }
  chunk_end(pkg,s);
  return 1;
}

int write_font(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"FONT");
  uint32_t n=(uint32_t)p->n_fonts;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcFont *font=&p->fonts[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,font->name?font->name:"");
    wstrptr(pkg,sid);
    wu32(&pkg->b,0);
    wi32(&pkg->b,font->em_size>0?font->em_size:12);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    uint32_t tex_pos=(uint32_t)pkg->b.len;
    wu32(&pkg->b,0);
    if(!add_font_patch(pkg,tex_pos,(int)i)) return 0;
    wf32(&pkg->b,1.0f);
    wf32(&pkg->b,1.0f);
    wu32(&pkg->b,(uint32_t)font->n_glyphs);
    size_t gtable=pkg->b.len;
    zfill(&pkg->b,(size_t)font->n_glyphs*4);
    for(int g=0;g<font->n_glyphs;g++){
      const GmlcFontGlyph *gl=&font->glyphs[g];
      patch32(&pkg->b,gtable+(size_t)g*4,(uint32_t)pkg->b.len);
      wu16(&pkg->b,(uint16_t)gl->ch);
      wu16(&pkg->b,(uint16_t)gl->x);
      wu16(&pkg->b,(uint16_t)gl->y);
      wu16(&pkg->b,(uint16_t)gl->w);
      wu16(&pkg->b,(uint16_t)gl->h);
      wu16(&pkg->b,(uint16_t)gl->shift);
      wu16(&pkg->b,(uint16_t)gl->offset);
      wu16(&pkg->b,0); /* empty GMS2 glyph-kerning list */
    }
  }
  for(uint16_t i=0;i<0x80;i++) wu16(&pkg->b,i);
  for(uint16_t i=0;i<0x80;i++) wu16(&pkg->b,0x3f);
  chunk_end(pkg,s);
  return 1;
}

int write_audo(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"AUDO");
  uint32_t n=(uint32_t)p->n_sounds;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSound *snd=&p->sounds[i];
    uint8_t *blob=NULL; size_t blen=0;
    if(!snd->data_path){
      patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
      wu32(&pkg->b,0);
      if(i+1<n) while(pkg->b.len % 4) wu8(&pkg->b,0);
      continue;
    }
    if(!read_blob(p,snd->data_path,&blob,&blen)){
      snprintf(err,errcap,"%s: sound blob read failed",snd->data_path?snd->data_path:"<missing>");
      return 0;
    }
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,(uint32_t)blen);
    wbytes(&pkg->b,blob,blen);
    if(i+1<n) while(pkg->b.len % 4) wu8(&pkg->b,0);
    free(blob);
  }
  chunk_end(pkg,s);
  return 1;
}

int fixed_zero_chunk(Pkg *p, const char name[4], size_t n){
  size_t s=chunk_begin(p,name);
  if(!zfill(&p->b,n)) return 0;
  chunk_end(p,s);
  return 1;
}

int write_classic_marker(Pkg *pkg, const GmlcProject *project){
  if(!project->classic_version) return 1;
  if(project->classic_game_information_size>UINT32_MAX) return 0;
  size_t s=chunk_begin(pkg,"CLSC");
  wu32(&pkg->b,(uint32_t)project->classic_version);
  wi32(&pkg->b,project->classic_scaling);
  wu32(&pkg->b,(uint32_t)(project->classic_interpolate?1:0));
  wu32(&pkg->b,project->classic_outside_color);
  wu32(&pkg->b,(uint32_t)(project->classic_swap_creation_events?1:0));
  wu32(&pkg->b,(uint32_t)project->classic_game_information_size);
  if(project->classic_game_information_size &&
     !wbytes(&pkg->b,project->classic_game_information,
             project->classic_game_information_size)) return 0;
  /* Appended after the variable-sized information body so older CLSC readers retain their exact
   * field offsets and simply ignore this layout word. */
  wu32(&pkg->b,(uint32_t)(project->classic_executable_layout?1:0));
  chunk_end(pkg,s);
  return 1;
}

int write_agrp(Pkg *p){
  int sid=intern(p,"Default");
  if(sid<0) return 0;
  size_t s=chunk_begin(p,"AGRP");
  wu32(&p->b,1);
  size_t table=p->b.len;
  wu32(&p->b,0);
  patch32(&p->b,table,(uint32_t)p->b.len);
  wstrptr(p,sid);
  chunk_end(p,s);
  return 1;
}

int write_optn(Pkg *p){
  int sleep_sid=intern(p,"@@SleepMargin");
  int sleep_value_sid=intern(p,"10");
  int draw_sid=intern(p,"@@DrawColour");
  int draw_value_sid=intern(p,"4294967295");
  if(sleep_sid<0 || draw_sid<0 || sleep_value_sid<0 || draw_value_sid<0) return 0;
  size_t s=chunk_begin(p,"OPTN");
  wu32(&p->b,0x80000000u);
  wu32(&p->b,2);
  wu64(&p->b,0x0000000000480214ull);
  wi32(&p->b,1);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,1);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,2);
  wstrptr(p,sleep_sid);
  wstrptr(p,sleep_value_sid);
  wstrptr(p,draw_sid);
  wstrptr(p,draw_value_sid);
  chunk_end(p,s);
  return 1;
}

int write_embi(Pkg *p){
  size_t s=chunk_begin(p,"EMBI");
  wu32(&p->b,1);
  wu32(&p->b,0);
  chunk_end(p,s);
  return 1;
}

/* GEN8 ends with a "random UID" block: a first-random word, four more 32-bit words the
 * GameMaker IDE fills to make an exported data.win look unique, then a frame-time float, a
 * flag byte and a 16-byte pad. This runtime writes a data.win only for its own loader, which
 * reads the header fields and the room order and never these words (nor the +92 timestamp),
 * so a fixed zero block keeps the chunk well-formed, the same size and fully deterministic. */
static int write_gen8_uid_block(Buf *b){
  for(int i=0;i<5;i++) if(!wu64(b,0)) return 0;   /* first-random word + four UID words */
  if(!wf32(b,60.0f) || !wu8(b,1) || !zfill(b,16)) return 0;
  return 1;
}
static char *identifier_from_name(const char *name){
  if(!name || !*name) return gmlc_strdup("source_project");
  size_t n=strlen(name);
  char *out=(char*)malloc(n+1);
  if(!out) return NULL;
  for(size_t i=0;i<n;i++){
    unsigned char ch=(unsigned char)name[i];
    out[i]=(isalnum(ch) || ch=='_') ? (char)ch : '_';
  }
  out[n]=0;
  if(!isalpha((unsigned char)out[0]) && out[0]!='_') out[0]='_';
  return out;
}

int write_gen8(Pkg *pkg, const GmlcProject *p){
  const char *display_name=p->name?p->name:"source_project";
  char *identifier=identifier_from_name(display_name);
  if(!identifier) return 0;
  int sid_name=intern(pkg,display_name);
  int sid_cfg=intern(pkg,"default");
  int sid_id=intern(pkg,identifier);
  free(identifier);
  if(sid_name<0 || sid_cfg<0 || sid_id<0) return 0;
  uint32_t dw=640, dh=480;
  if(p->n_rooms>0){
    const GmlcRoom *room=&p->rooms[0];
    dw=(uint32_t)(room->width>0?room->width:640);
    dh=(uint32_t)(room->height>0?room->height:480);
    if(room->view_enabled){
      int right=0,bottom=0;
      for(int i=0;i<room->n_views && i<8;i++) if(room->views[i].visible){
        int w=room->views[i].wport>0?room->views[i].wport:room->width;
        int h=room->views[i].hport>0?room->views[i].hport:room->height;
        int r=room->views[i].xport+w, b=room->views[i].yport+h;
        if(r>right) right=r;
        if(b>bottom) bottom=b;
      }
      if(right>0) dw=(uint32_t)right;
      if(bottom>0) dh=(uint32_t)bottom;
    }
  }
  if(p->classic_version && p->classic_scaling>0){
    uint64_t scaled_w=((uint64_t)dw*(uint32_t)p->classic_scaling+50u)/100u;
    uint64_t scaled_h=((uint64_t)dh*(uint32_t)p->classic_scaling+50u)/100u;
    if(scaled_w>0 && scaled_w<=UINT32_MAX) dw=(uint32_t)scaled_w;
    if(scaled_h>0 && scaled_h<=UINT32_MAX) dh=(uint32_t)scaled_h;
  }
  const uint8_t bytecode_version=15;
  const uint32_t game_id=0;
  const uint32_t info_flags=0x000000B2u;
  const uint64_t timestamp=0;
  size_t s=chunk_begin(pkg,"GEN8");
  size_t base=pkg->b.len;
  zfill(&pkg->b,128);
  pkg->b.data[base+0]=1;
  pkg->b.data[base+1]=bytecode_version;
  add_str_patch(pkg,(uint32_t)(base+4),sid_name);
  add_str_patch(pkg,(uint32_t)(base+8),sid_cfg);
  patch32(&pkg->b,base+12,(uint32_t)(p->next_instance_id>100000?p->next_instance_id:100000));
  patch32(&pkg->b,base+16,10000000);
  patch32(&pkg->b,base+20,game_id);
  add_str_patch(pkg,(uint32_t)(base+40),sid_id);
  patch32(&pkg->b,base+44,2);
  patch32(&pkg->b,base+48,0);
  patch32(&pkg->b,base+52,0);
  patch32(&pkg->b,base+56,0);
  patch32(&pkg->b,base+60,(uint32_t)dw);
  patch32(&pkg->b,base+64,(uint32_t)dh);
  patch32(&pkg->b,base+68,info_flags);
  patch64(&pkg->b,base+92,timestamp);
  add_str_patch(pkg,(uint32_t)(base+100),sid_name);
  patch32(&pkg->b,base+124,6502);
  int room_order_count=p->n_room_order>0?p->n_room_order:p->n_rooms;
  wu32(&pkg->b,(uint32_t)room_order_count);
  for(int i=0;i<room_order_count;i++)
    wu32(&pkg->b,(uint32_t)(p->n_room_order>0?p->room_order[i]:i));
  if(!write_gen8_uid_block(&pkg->b)) return 0;
  while((pkg->b.len-base)&3) wu8(&pkg->b,0);
  chunk_end(pkg,s);
  return 1;
}

static int object_event_code_index(const GmlcProject *p, int obj_index, int event_index){
  if(obj_index<0 || obj_index>=p->n_objects) return -1;
  const GmlcObject *obj=&p->objects[obj_index];
  if(event_index<0 || event_index>=obj->n_events) return -1;
  int idx=room_code_count(p) + p->n_scripts + total_timeline_moments(p);
  for(int i=0;i<obj_index;i++) idx += p->objects[i].n_events;
  int rank=event_type_rank(obj->events[event_index].event_type);
  for(int i=0;i<obj->n_events;i++){
    int r=event_type_rank(obj->events[i].event_type);
    if(r<rank || (r==rank && i<event_index)) idx++;
  }
  return idx;
}

static int write_objt_event_action(Pkg *pkg, int code_index, int empty_sid){
  wu32(&pkg->b,1);
  wu32(&pkg->b,603);
  wu32(&pkg->b,7);
  wu32(&pkg->b,0);
  wu32(&pkg->b,0);
  wu32(&pkg->b,1);
  wu32(&pkg->b,2);
  wstrptr(pkg,empty_sid);
  wi32(&pkg->b,code_index);
  wu32(&pkg->b,1);
  wi32(&pkg->b,-1);
  wu32(&pkg->b,0);
  wu32(&pkg->b,0);
  wu32(&pkg->b,0);
  return 1;
}

static int write_objt_event(Pkg *pkg, const GmlcObjectEvent *ev,
                            int code_index, int empty_sid){
  uint32_t subtype=(uint32_t)ev->event_number;
  if(ev->event_type==4 && ev->collision_object_id>=0)
    subtype=(uint32_t)ev->collision_object_id;
  wu32(&pkg->b,subtype);
  wu32(&pkg->b,1);
  size_t table=pkg->b.len;
  wu32(&pkg->b,0);
  patch32(&pkg->b,table,(uint32_t)pkg->b.len);
  return write_objt_event_action(pkg,code_index,empty_sid);
}

static int write_objt_events(Pkg *pkg, const GmlcProject *p,
                             int obj_index, int empty_sid){
  const int event_type_count=13;
  const GmlcObject *obj=&p->objects[obj_index];
  wu32(&pkg->b,(uint32_t)event_type_count);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)event_type_count*4);
  for(int t=0;t<event_type_count;t++){
    patch32(&pkg->b,table+(size_t)t*4,(uint32_t)pkg->b.len);
    int count=0;
    for(int ei=0;ei<obj->n_events;ei++) if(obj->events[ei].event_type==t) count++;
    wu32(&pkg->b,(uint32_t)count);
    size_t subtable=pkg->b.len;
    zfill(&pkg->b,(size_t)count*4);
    int wi=0;
    for(int ei=0;ei<obj->n_events;ei++){
      if(obj->events[ei].event_type!=t) continue;
      patch32(&pkg->b,subtable+(size_t)wi*4,(uint32_t)pkg->b.len);
      if(!write_objt_event(pkg,&obj->events[ei],
                           object_event_code_index(p,obj_index,ei),
                           empty_sid)) return 0;
      wi++;
    }
  }
  return 1;
}

int write_objt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"OBJT");
  uint32_t n=(uint32_t)p->n_objects;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  int empty_sid=intern(pkg,"");
  if(empty_sid<0) return 0;
  for(uint32_t i=0;i<n;i++){
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    const GmlcObject *o=&p->objects[i];
    int sid=intern(pkg,o->name);
    if(sid<0) return 0;
    wstrptr(pkg,sid);
    wi32(&pkg->b,gmlc_project_sprite_runtime_id(p,o->sprite_id));
    wu32(&pkg->b,(uint32_t)(o->visible?1:0));
    wu32(&pkg->b,(uint32_t)(o->solid?1:0));
    wi32(&pkg->b,o->depth);
    wu32(&pkg->b,(uint32_t)(o->persistent?1:0));
    wi32(&pkg->b,o->parent_id>=0?o->parent_id:-100);
    wi32(&pkg->b,gmlc_project_sprite_runtime_id(p,o->mask_id));
    wu32(&pkg->b,(uint32_t)(o->physics_enabled?1:0));
    wu32(&pkg->b,(uint32_t)(o->physics_sensor?1:0));
    wi32(&pkg->b,o->physics_shape);
    wf32(&pkg->b,o->physics_density);
    wf32(&pkg->b,o->physics_restitution);
    wi32(&pkg->b,o->physics_group);
    wf32(&pkg->b,o->physics_linear_damping);
    wf32(&pkg->b,o->physics_angular_damping);
    wu32(&pkg->b,(uint32_t)o->n_physics_points);
    wf32(&pkg->b,o->physics_friction);
    wu32(&pkg->b,(uint32_t)(o->physics_awake?1:0));
    wu32(&pkg->b,(uint32_t)(o->physics_kinematic?1:0));
    for(int pi=0;pi<o->n_physics_points;pi++){
      wf32(&pkg->b,o->physics_points[pi].x);
      wf32(&pkg->b,o->physics_points[pi].y);
    }
    if(!write_objt_events(pkg,p,(int)i,empty_sid)) return 0;
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_room_views(Pkg *pkg, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,8);
  size_t table=pkg->b.len;
  zfill(&pkg->b,8*4);
  for(int i=0;i<8;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    const GmlcRoomView *view=&r->views[i];
    wu32(&pkg->b,(uint32_t)(view->visible?1:0));
    wi32(&pkg->b,view->xview); wi32(&pkg->b,view->yview);
    wi32(&pkg->b,view->wview>0?view->wview:r->width);
    wi32(&pkg->b,view->hview>0?view->hview:r->height);
    wi32(&pkg->b,view->xport); wi32(&pkg->b,view->yport);
    wi32(&pkg->b,view->wport>0?view->wport:r->width);
    wi32(&pkg->b,view->hport>0?view->hport:r->height);
    wi32(&pkg->b,view->hborder); wi32(&pkg->b,view->vborder);
    wi32(&pkg->b,view->hspeed); wi32(&pkg->b,view->vspeed);
    wi32(&pkg->b,view->object_id);
  }
  return 1;
}

static int write_room_instances(Pkg *pkg, const GmlcProject *p, int room_index, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_instances);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_instances*4);
  for(int i=0;i<r->n_instances;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    const GmlcRoomInstance *in=&r->instances[i];
    wi32(&pkg->b,in->x);
    wi32(&pkg->b,in->y);
    wi32(&pkg->b,in->object_id);
    wu32(&pkg->b,(uint32_t)in->instance_id);
    wi32(&pkg->b,room_instance_creation_code_index(p,room_index,i));
    wf32(&pkg->b,in->sx==0.0f?1.0f:in->sx);
    wf32(&pkg->b,in->sy==0.0f?1.0f:in->sy);
    wu32(&pkg->b,in->color?in->color:0xFFFFFFFFu);
    wf32(&pkg->b,in->rotation);
  }
  return 1;
}

static int write_room_backgrounds(Pkg *pkg, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_backgrounds);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_backgrounds*4);
  for(int i=0;i<r->n_backgrounds;i++){
    const GmlcRoomBackground *bg=&r->backgrounds[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,(uint32_t)(bg->visible?1:0));
    wu32(&pkg->b,(uint32_t)(bg->foreground?1:0));
    wi32(&pkg->b,bg->background_id);
    wi32(&pkg->b,bg->x); wi32(&pkg->b,bg->y);
    wu32(&pkg->b,(uint32_t)(bg->htiled?1:0));
    wu32(&pkg->b,(uint32_t)(bg->vtiled?1:0));
    wi32(&pkg->b,bg->hspeed); wi32(&pkg->b,bg->vspeed);
    wu32(&pkg->b,(uint32_t)(bg->stretch?1:0));
  }
  return 1;
}

static int write_room_tiles(Pkg *pkg, const GmlcProject *p, const GmlcRoom *r, uint32_t *out_ptr){
  (void)p;
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_tiles);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_tiles*4);
  for(int i=0;i<r->n_tiles;i++){
    const GmlcRoomTile *tile=&r->tiles[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wi32(&pkg->b,tile->x); wi32(&pkg->b,tile->y);
    wi32(&pkg->b,tile->background_id);
    wi32(&pkg->b,tile->source_x); wi32(&pkg->b,tile->source_y);
    wi32(&pkg->b,tile->width); wi32(&pkg->b,tile->height);
    wi32(&pkg->b,tile->depth); wi32(&pkg->b,tile->tile_id);
    wf32(&pkg->b,1.0f); wf32(&pkg->b,1.0f); wu32(&pkg->b,0xFFFFFFFFu);
  }
  return 1;
}

static int write_room_layer_list(Pkg *pkg, const GmlcProject *p, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_layers);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_layers*4);
  for(int i=0;i<r->n_layers;i++){
    const GmlcRoomLayer *ly=&r->layers[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,ly->name?ly->name:"");
    wstrptr(pkg,sid);
    wi32(&pkg->b,ly->layer_id);
    wi32(&pkg->b,ly->type);
    wi32(&pkg->b,ly->depth);
    wf32(&pkg->b,ly->x);
    wf32(&pkg->b,ly->y);
    wf32(&pkg->b,ly->hspeed);
    wf32(&pkg->b,ly->vspeed);
    wu32(&pkg->b,(uint32_t)(ly->visible?1:0));
    if(ly->type==1){
      wu32(&pkg->b,(uint32_t)(ly->visible?1:0));
      wu32(&pkg->b,0);
      wi32(&pkg->b,gmlc_project_sprite_runtime_id(p,ly->bg_sprite_id));
      wu32(&pkg->b,(uint32_t)(ly->bg_htiled?1:0));
      wu32(&pkg->b,(uint32_t)(ly->bg_vtiled?1:0));
      wu32(&pkg->b,(uint32_t)(ly->bg_stretch?1:0));
      wu32(&pkg->b,ly->bg_color);
      wf32(&pkg->b,ly->bg_frame);
      wf32(&pkg->b,ly->bg_speed);
      wu32(&pkg->b,0);
    } else if(ly->type==2){
      wu32(&pkg->b,(uint32_t)ly->n_instance_ids);
      for(int k=0;k<ly->n_instance_ids;k++) wu32(&pkg->b,ly->instance_ids[k]);
    } else if(ly->type==3){
      size_t tiles_pos=pkg->b.len; wu32(&pkg->b,0);
      size_t sprites_pos=pkg->b.len; wu32(&pkg->b,0);
      patch32(&pkg->b,tiles_pos,(uint32_t)pkg->b.len);
      wu32(&pkg->b,0);
      patch32(&pkg->b,sprites_pos,(uint32_t)pkg->b.len);
      wu32(&pkg->b,(uint32_t)ly->n_assets);
      size_t stable=pkg->b.len;
      zfill(&pkg->b,(size_t)ly->n_assets*4);
      for(int a=0;a<ly->n_assets;a++){
        const GmlcRoomAsset *ra=&ly->assets[a];
        patch32(&pkg->b,stable+(size_t)a*4,(uint32_t)pkg->b.len);
        int asid=intern(pkg,ra->name?ra->name:"");
        wstrptr(pkg,asid);
        wi32(&pkg->b,gmlc_project_sprite_runtime_id(p,ra->sprite_id));
        wi32(&pkg->b,ra->x);
        wi32(&pkg->b,ra->y);
        wf32(&pkg->b,ra->sx==0.0f?1.0f:ra->sx);
        wf32(&pkg->b,ra->sy==0.0f?1.0f:ra->sy);
        wu32(&pkg->b,ra->color?ra->color:0xFFFFFFFFu);
        wf32(&pkg->b,ra->speed);
        wu32(&pkg->b,0);
        wf32(&pkg->b,ra->frame);
        wf32(&pkg->b,ra->rotation);
      }
    } else if(ly->type==4){
      wi32(&pkg->b,ly->tile_tileset_id);
      wi32(&pkg->b,ly->tile_cols);
      wi32(&pkg->b,ly->tile_rows);
      int cells=ly->tile_cols>0 && ly->tile_rows>0 ? ly->tile_cols*ly->tile_rows : 0;
      for(int c=0;c<cells;c++) wu32(&pkg->b,ly->tile_data?ly->tile_data[c]:0);
    }
  }
  return 1;
}

static int write_room(Pkg *pkg, const GmlcProject *p, const GmlcRoom *r, int room_index, uint32_t *record_ptr){
  *record_ptr=(uint32_t)pkg->b.len;
  int sid=intern(pkg,r->name);
  wstrptr(pkg,sid);
  wu32(&pkg->b,0);
  wu32(&pkg->b,(uint32_t)r->width);
  wu32(&pkg->b,(uint32_t)r->height);
  wu32(&pkg->b,(uint32_t)(r->speed>0?r->speed:60));
  wu32(&pkg->b,(uint32_t)(r->persistent?1:0));
  wu32(&pkg->b,r->background_color);
  wu32(&pkg->b,(uint32_t)(r->draw_background_color?1:0));
  wi32(&pkg->b,room_creation_code_index(p,room_index));
  /* Low bits encode ROOM flags for legacy views and background-colour clearing.
   * The high word identifies the layout generation used by this bytecode-15 package. */
  wu32(&pkg->b,0x00020000u | (r->view_enabled?1u:0u) |
                   (r->draw_background_color?2u:0u));
  size_t bg_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t view_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t obj_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t tile_pos=pkg->b.len; wu32(&pkg->b,0);
  wu32(&pkg->b,(uint32_t)(r->physics_world?1:0));
  zfill(&pkg->b,16);
  wf32(&pkg->b,r->physics_gravity_x);
  wf32(&pkg->b,r->physics_gravity_y);
  wf32(&pkg->b,r->physics_scale>0.0f?r->physics_scale:0.1f);
  size_t layer_pos=pkg->b.len; wu32(&pkg->b,0);
  uint32_t bg=0, view=0, obj=0, tile=0, layers=0;
  write_room_backgrounds(pkg,r,&bg);
  write_room_views(pkg,r,&view);
  write_room_instances(pkg,p,room_index,r,&obj);
  write_room_tiles(pkg,p,r,&tile);
  write_room_layer_list(pkg,p,r,&layers);
  patch32(&pkg->b,bg_pos,bg);
  patch32(&pkg->b,view_pos,view);
  patch32(&pkg->b,obj_pos,obj);
  patch32(&pkg->b,tile_pos,tile);
  patch32(&pkg->b,layer_pos,layers);
  return 1;
}

int write_room_chunk(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"ROOM");
  uint32_t n=(uint32_t)p->n_rooms;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    uint32_t ptr=0;
    write_room(pkg,p,&p->rooms[i],(int)i,&ptr);
    patch32(&pkg->b,table+i*4,ptr);
  }
  chunk_end(pkg,s);
  return 1;
}

int write_strg(Pkg *pkg){
  size_t s=chunk_begin(pkg,"STRG");
  wu32(&pkg->b,(uint32_t)pkg->strs.n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)pkg->strs.n*4);
  for(int i=0;i<pkg->strs.n;i++){
    uint32_t len=(uint32_t)strlen(pkg->strs.items[i]);
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,len);
    pkg->strs.char_off[i]=(uint32_t)pkg->b.len;
    wbytes(&pkg->b,pkg->strs.items[i],len);
    wu8(&pkg->b,0);
  }
  while(pkg->b.len % 0x80) wu8(&pkg->b,0);
  chunk_end(pkg,s);
  for(int i=0;i<pkg->n_patches;i++){
    StrPatch *sp=&pkg->patches[i];
    if(sp->sid>=0 && sp->sid<pkg->strs.n) patch32(&pkg->b,sp->pos,pkg->strs.char_off[sp->sid]);
  }
  return 1;
}
