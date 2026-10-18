/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared shader and sampler policy, preserving the established hybrid/content-shader path. */
#include "gml_render_shaders.h"
#include "gml_render_internal.h"
#include "gml_render_palette_internal.h"
#include "gml_render_texture.h"
#include "anygm_host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A terminal record borrows its program's controls. Complete it before a setter changes those
 * controls, while shader_reset alone may leave the original program retained for presentation. */
static void flush_recorded_program(GmlRender *r,int shader){
  if(r && r->pending_presentation && r->presentation.shader>=0 &&
     (shader<0 || r->presentation.shader==shader)) gml_render_maybe_prepare_draw(r);
}

void gml_render_shader_set_current(GmlRender *r,int shader){
  if(r) r->active_shader=shader;
}
int gml_render_shader_current(const GmlRender *r){
  return r?r->active_shader:-1;
}
/* The colour modulation the active vertex program applies to every vertex colour it passes on, as
 * red, green, blue and alpha factors. Answers zero when no such program is set, so callers keep
 * their unmodulated path; the identity is answered while a recognized program's uniform has not
 * arrived yet, because a shader is not a reason to blank what content drew. */
int gml_render_cpu_backend_shader_vertex_colour_blend(const GmlRender *r,double blend[4]){
  if(!r || !blend || r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal)
    return 0;
  const struct GmlShaderPal *recognized=&r->shader_pal[r->active_shader];
  if(!recognized->vertex_colour_blend) return 0;
  for(int component=0;component<4;component++)
    blend[component]=recognized->vertex_colour_blend_alpha_only && component<3
      ? 1.0 : (double)recognized->vertex_colour_blend_value[component];
  return 1;
}
#define GML_RENDER_SHADER_HANDLE_STRIDE 64
#define GML_RENDER_SHADER_HANDLE(shader,slot) \
  ((shader)*GML_RENDER_SHADER_HANDLE_STRIDE+(slot))

/* Whether the software evaluator recognizes a declared shader family. */
static int shader_pal_recognized(const struct GmlShaderPal *p){
  return p->threshold_palette || p->indexed_brightness ||
         p->channel_mask || p->channel_alpha_key || p->alpha_discard ||
         p->ordered_dither || p->quantise4 ||
         p->solid_alpha_mask || p->solid_blur_alpha || p->lut || p->lut_indexed || p->grid ||
         p->dual_sample || p->radial_wave || p->uv_wave_mode || p->grayscale;
}

int gml_render_shader_cpu_passthrough(const GmlRender *r){
  if(!r || r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal)
    return 0;
  const struct GmlShaderPal *p=&r->shader_pal[r->active_shader];
  if(shader_pal_recognized(p) || p->vertex_colour_blend || p->vertex_colour_blend_alpha_only)
    return 0;
  /* Ordinary CPU draws use the source unchanged when no software family or
   * applicable external executor owns the program. Preserve that policy,
   * without compiling the content or changing its readable selection. */
  return !r->shader_executor || !gml_render_shader_content_candidate(r,r->active_shader);
}

int gml_render_shader_channel_alpha_key(const GmlRender *r,uint32_t *key){
  if(key) *key=0u;
  if(!key || !r || !r->shader_pal || r->active_shader<0 ||
     r->active_shader>=r->n_shader_pal) return 0;
  const struct GmlShaderPal *p=&r->shader_pal[r->active_shader];
  if(!p->channel_alpha_key || p->channel_alpha_key_channel<0 ||
     p->channel_alpha_key_channel>2 || !isfinite(p->channel_alpha_key_cutoff) ||
     p->threshold_palette || p->indexed_brightness || p->channel_mask ||
     p->alpha_discard || p->ordered_dither || p->quantise4 ||
     p->solid_alpha_mask || p->solid_blur_alpha || p->lut || p->lut_indexed ||
     p->grid || p->dual_sample || p->radial_wave || p->uv_wave_mode || p->grayscale ||
     p->vertex_colour_blend || p->vertex_colour_blend_alpha_only) return 0;
  unsigned first=0u;
  for(;first<256u;first++){
    /* The unchanged CPU predicate multiplies by a float reciprocal. Resolve
     * this finite byte partition once,not CPU float arithmetic per fragment. */
    float channel=(float)first*(1.0f/255.0f);
    if(p->channel_alpha_key_inclusive ? channel>p->channel_alpha_key_cutoff :
                                       channel>=p->channel_alpha_key_cutoff) break;
  }
  *key=((uint32_t)(p->channel_alpha_key_channel+1)<<9)|first;
  return 1;
}

int gml_render_shader_is_compiled(const GmlRender *r,int shader){
  if(!r || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  if(r->shader_pal[shader].gpu_failed) return 0;
  /* A declared shader may be reported compiled even when the software renderer does not
   * execute its effect. The host option selects that policy for unrecognized programs that
   * sample a picture, so authored fallback paths remain selectable when needed.
   *
   * A fragment that samples no texture computes its output from other inputs. Leaving its
   * draw unshaded paints unrelated pixels, so this class reports uncompiled unless a
   * recognized software family executes it. A picture may arrive through any bound sampler,
   * not only gm_BaseTexture; the distinction is sampling, not the uniform name. */
  if(shader_pal_recognized(&r->shader_pal[shader])) return 1;
  if(r->shader_pal[shader].gpu_binding_overflow) return 0;
  /* The paragraph above was written when nothing here could run a fragment. A device that can is
   * the case it excludes: told yes with one present, the program is executed and the content gets
   * the picture it wrote, which is the whole point of the answer. Told yes with none, the
   * primitive is painted flat in a colour the shader was going to discard, which is why the
   * refusal stands whenever there is no device. */
  if(r->shader_pal[shader].procedural) return r->shader_device_expected?1:0;
  /* With an adopted graphics context, content programs with both sources can execute on the
   * device. Use the finalized session flag rather than transient per-frame presence, so a
   * context reset does not change a support answer content may have cached. Initial loading
   * starts authored events only after context adoption has its final answer. */
  if(r->shader_device_expected &&
     r->shader_pal[shader].source_vertex_es && r->shader_pal[shader].source_fragment_es) return 1;
  return r->shader_report_all_compiled?1:0;
}

int gml_render_shader_uniform_handle(GmlRender *r,int shader,const char *name){
  if(r && name && shader>=0 && shader<r->n_shader_pal && r->shader_pal){
    const struct GmlShaderPal *recognized=&r->shader_pal[shader];
    if(recognized->threshold_palette)
      for(int index=0;index<10;index++)
        if(!strcmp(name,recognized->threshold_palette_uniform[index]))
          return GML_RENDER_SHADER_HANDLE(shader,index);
    if(recognized->indexed_brightness &&
       !strcmp(name,recognized->indexed_brightness_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,54);
    if(recognized->ordered_dither && !strcmp(name,recognized->ordered_dither_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,40);
    if(recognized->quantise4)
      for(int index=0;index<4;index++)
        if(!strcmp(name,recognized->quantise4_uniform[index]))
          return GML_RENDER_SHADER_HANDLE(shader,41+index);
    if(recognized->lut && !strcmp(name,recognized->lut_row_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,1);
    if(recognized->lut_indexed){
      if(!strcmp(name,recognized->lut_uvs_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,15);
      if(!strcmp(name,recognized->lut_offset_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,16);
      if(!strcmp(name,recognized->lut_colors_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,17);
      if(recognized->lut_has_colorise &&
         !strcmp(name,recognized->lut_colorise_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,18);
      if(recognized->lut_has_bounds &&
         !strcmp(name,recognized->lut_bounds_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,19);
    }
    if(recognized->grid){
      if(!strcmp(name,recognized->grid_pixel_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,7);
      if(!strcmp(name,recognized->grid_uvs_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,8);
      if(!strcmp(name,recognized->grid_id_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,9);
    }
    if(recognized->dual_sample){
      if(!strcmp(name,recognized->dual_uniform[0]))
        return GML_RENDER_SHADER_HANDLE(shader,10);
      if(!strcmp(name,recognized->dual_uniform[1]))
        return GML_RENDER_SHADER_HANDLE(shader,11);
    }
    if(recognized->radial_wave)
      for(int index=0;index<6;index++)
        if(!strcmp(name,recognized->radial_wave_uniform[index]))
          return GML_RENDER_SHADER_HANDLE(shader,44+index);
    if(recognized->uv_wave_mode && !strcmp(name,recognized->uv_wave_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,50);
    if(recognized->grayscale &&
       recognized->grayscale_has_alpha_uniform &&
       !strcmp(name,recognized->grayscale_alpha_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,14);
    if(recognized->solid_alpha_mask &&
       !strcmp(name,recognized->solid_alpha_mask_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,51);
    if(recognized->solid_blur_alpha &&
       !strcmp(name,recognized->solid_blur_alpha_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,52);
    if(recognized->vertex_colour_blend &&
       !strcmp(name,recognized->vertex_colour_blend_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,53);
  }
  if(r && name && name[0] && shader>=0 && shader<r->n_shader_pal && r->shader_pal){
    /* No recognized family owns this name: keep it for the content's own program. A name the
     * program does not declare costs one unused entry, which the device ignores. A recognized
     * family runs in software and has no such program, so its unknown names stay ignored. */
    struct GmlShaderPal *recognized=&r->shader_pal[shader];
    if(shader_pal_recognized(recognized)) return GML_RENDER_SHADER_HANDLE(shader,63);
    for(int index=0;index<recognized->generic_uniform_count;index++)
      if(!strcmp(name,recognized->generic_uniform[index].name))
        return GML_RENDER_GENERIC_HANDLE(shader,index);
    if(recognized->generic_uniform_count<GML_SHADER_GENERIC_UNIFORMS){
      int index=recognized->generic_uniform_count++;
      memset(&recognized->generic_uniform[index],0,sizeof recognized->generic_uniform[index]);
      snprintf(recognized->generic_uniform[index].name,
               sizeof recognized->generic_uniform[index].name,"%s",name);
      return GML_RENDER_GENERIC_HANDLE(shader,index);
    }
    recognized->gpu_binding_overflow=1;
  }
  return shader>=0?GML_RENDER_SHADER_HANDLE(shader,63):-1;
}

int gml_render_shader_sampler_handle(GmlRender *r,int shader,const char *name){
  if(r && name && shader>=0 && shader<r->n_shader_pal && !strcmp(name,"gm_BaseTexture"))
    return 0;
  if(r && name && shader>=0 && shader<r->n_shader_pal && r->shader_pal){
    const struct GmlShaderPal *recognized=&r->shader_pal[shader];
    for(int index=0;index<recognized->generic_sampler_count;index++)
      if(!strcmp(name,recognized->generic_sampler[index].name))
        return GML_RENDER_GENERIC_SAMPLER_HANDLE(shader,index);
  }
  return shader>=0?GML_RENDER_SHADER_HANDLE(shader,2):-1;
}

static int generic_handle_parts(const GmlRender *r,int handle,int *shader,int *index,int *sampler){
  if(handle<0 || !(handle&GML_RENDER_GENERIC_HANDLE_FLAG)) return 0;
  *shader=(handle>>8)&0xFFF;
  *index=handle&0x7F;
  *sampler=(handle&GML_RENDER_GENERIC_SAMPLER_FLAG)!=0;
  return r && *shader<r->n_shader_pal && r->shader_pal;
}

static int texture_filter_stage(const GmlRender *r,int handle){
  int shader,index,sampler;
  if(generic_handle_parts(r,handle,&shader,&index,&sampler)){
    if(!sampler || index>=r->shader_pal[shader].generic_sampler_count) return -1;
    return index+1;
  }
  return handle>=0 && handle<GML_RENDER_TEXTURE_STAGES?handle:-1;
}
void gml_render_texture_filter_all(GmlRender *r,int enabled){
  if(!r) return;
  flush_recorded_program(r,-1);
  r->interp=enabled?1:0;
  for(int i=0;i<GML_RENDER_TEXTURE_STAGES-1;i++) r->texture_filter[i]=r->interp;
}
void gml_render_texture_filter_set(GmlRender *r,int sampler,int enabled){
  if(!r) return;
  int stage=texture_filter_stage(r,sampler);
  if(stage>=0) flush_recorded_program(r,-1);
  if(stage==0) r->interp=enabled?1:0;
  else if(stage>0) r->texture_filter[stage-1]=enabled?1:0;
}
int gml_render_texture_filter_get(const GmlRender *r,int sampler){
  if(!r) return 0;
  int stage=texture_filter_stage(r,sampler);
  return stage==0?r->interp:stage>0?r->texture_filter[stage-1]:0;
}

void gml_render_shader_uniform_set_values(GmlRender *r,int handle,const double *values,
                                          uint32_t count,int integer){
  int shader,index,sampler;
  if(!r || !values || count==0) return;
  if(!generic_handle_parts(r,handle,&shader,&index,&sampler) || sampler) return;
  {
    struct GmlShaderPal *recognized=&r->shader_pal[shader];
    if(index>=recognized->generic_uniform_count) return;
    flush_recorded_program(r,shader);
    if(count>16u) count=16u;
    for(uint32_t component=0;component<count;component++)
      recognized->generic_uniform[index].value[component]=(float)values[component];
    recognized->generic_uniform[index].count=(int)count;
    recognized->generic_uniform[index].integer=integer?1:0;
  }
}

void gml_render_shader_uniform_set(GmlRender *r,int handle,const double values[4]){
  if(!r || !values || handle<0) return;
  if(handle&GML_RENDER_GENERIC_HANDLE_FLAG){
    gml_render_shader_uniform_set_values(r,handle,values,4u,0);
    return;
  }
  int shader=handle/GML_RENDER_SHADER_HANDLE_STRIDE;
  int slot=handle%GML_RENDER_SHADER_HANDLE_STRIDE;
  if(shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return;
  flush_recorded_program(r,shader);
  struct GmlShaderPal *recognized=&r->shader_pal[shader];
  if(recognized->indexed_brightness && slot==54){
    recognized->indexed_brightness_value=(float)values[0];
    recognized->indexed_brightness_set=1;
    return;
  }
  if(recognized->threshold_palette && slot>=0 && slot<10){
    uint8_t colour[3];
    for(int component=0;component<3;component++){
      int channel=(int)floor(values[component]*255.0+0.5);
      if(channel<0) channel=0;
      else if(channel>255) channel=255;
      colour[component]=(uint8_t)channel;
    }
    memcpy(recognized->threshold_palette_colour[slot],colour,sizeof colour);
    recognized->threshold_palette_set|=1u<<slot;
    return;
  }
  if(slot==40){
    if(recognized->ordered_dither) recognized->ordered_dither_alpha=(float)values[0];
    return;
  }
  if(slot>=41 && slot<=44){
    if(recognized->quantise4){
      int index=slot-41;
      for(int component=0;component<3;component++)
        recognized->quantise4_colour[index][component]=(float)values[component];
      recognized->quantise4_set|=1<<index;
    }
    return;
  }
  if(slot==1){
    if(recognized->lut) recognized->lut_row=(float)values[0];
    return;
  }
  if(recognized->lut_indexed && slot>=15 && slot<=19){
    if(slot==15)
      for(int component=0;component<4;component++)
        recognized->lut_uvs[component]=(float)values[component];
    else if(slot==16) recognized->lut_offset=(float)values[0];
    else if(slot==17) recognized->lut_colors=(float)values[0];
    else if(slot==18)
      for(int component=0;component<4;component++)
        recognized->lut_colorise[component]=(float)values[component];
    else
      for(int component=0;component<4;component++)
        recognized->lut_bounds[component]=(float)values[component];
    return;
  }
  if(recognized->grid){
    if(slot==7){
      recognized->grid_pixel[0]=(float)values[0];
      recognized->grid_pixel[1]=(float)values[1];
      return;
    }
    if(slot==8){
      for(int component=0;component<4;component++)
        recognized->grid_uvs[component]=(float)values[component];
      return;
    }
    if(slot==9){ recognized->grid_id=(float)values[0]; return; }
  }
  if(recognized->dual_sample){
    if(slot==10){ recognized->dual_value[0]=(float)values[0]; return; }
    if(slot==11){ recognized->dual_value[1]=(float)values[0]; return; }
  }
  if(recognized->radial_wave && slot>=44 && slot<50){
    int index=slot-44;
    recognized->radial_wave_value[index][0]=(float)values[0];
    if(index==1 || index==2)
      recognized->radial_wave_value[index][1]=(float)values[1];
    return;
  }
  if(recognized->uv_wave_mode && slot==50){
    recognized->uv_wave_time=(float)values[0];
    return;
  }
  if(recognized->grayscale && slot==14){
    recognized->grayscale_alpha=(float)values[0];
    return;
  }
  if(recognized->solid_alpha_mask && slot==51){
    uint32_t packed=0;
    for(int component=0;component<3;component++){
      recognized->solid_alpha_mask_colour[component]=(float)values[component];
      int channel=(int)floor(values[component]*255.0+0.5);
      if(channel<0) channel=0;
      else if(channel>255) channel=255;
      packed|=(uint32_t)channel<<(16-component*8);
    }
    recognized->solid_alpha_mask_rgb=packed;
    return;
  }
  if(recognized->vertex_colour_blend && slot==53){
    for(int component=0;component<4;component++){
      double value=values[component];
      if(value<0.0) value=0.0;
      else if(value>1.0) value=1.0;
      recognized->vertex_colour_blend_value[component]=(float)value;
    }
    recognized->vertex_colour_blend_set=1;
    return;
  }
  if(recognized->solid_blur_alpha && slot==52){
    uint32_t packed=0;
    for(int component=0;component<3;component++){
      recognized->solid_blur_alpha_colour[component]=(float)values[component];
      int channel=(int)floor(values[component]*255.0+0.5);
      if(channel<0) channel=0;
      else if(channel>255) channel=255;
      packed|=(uint32_t)channel<<(16-component*8);
    }
    recognized->solid_blur_alpha_rgb=packed;
    return;
  }
}

int gml_render_shader_texture_stage_set(
  GmlRender *r,int stage,int texture,GmlRenderShaderTextureBinding *binding){
  if(binding) memset(binding,0,sizeof(*binding));
  if(!r) return 0;
  int shader=stage/GML_RENDER_SHADER_HANDLE_STRIDE;
  int slot=stage%GML_RENDER_SHADER_HANDLE_STRIDE;
  uint32_t kind=(uint32_t)texture&GML_TEX_KIND_MASK;
  {
    int generic_shader,generic_index,generic_sampler;
    if(generic_handle_parts(r,stage,&generic_shader,&generic_index,&generic_sampler) &&
       generic_sampler){
      struct GmlShaderPal *recognized=&r->shader_pal[generic_shader];
      if(generic_index>=recognized->generic_sampler_count) return 0;
      if(kind!=GML_TEX_SPR_TAG && kind!=GML_TEX_SURF_TAG) return 0;
      flush_recorded_program(r,generic_shader);
      recognized->generic_sampler[generic_index].texture=texture;
      const char *name=recognized->generic_sampler[generic_index].name;
      int palette=(recognized->lut && !strcmp(name,recognized->lut_sampler)) ||
                  (recognized->grid && !strcmp(name,recognized->grid_sampler));
      /* Named samplers are collected before software-family recognition. Keep the program
       * binding, but also connect the recognized kernel to the texture its name selects.
       * Otherwise the generic handle silently bypasses the palette stage. Handle identities
       * remain unchanged, including those content has retained across a state restore. */
      if(palette){
        r->lut_pal_sprite=kind==GML_TEX_SPR_TAG?(texture>>10)&0xFFFF:-1;
        r->lut_pal_frame=kind==GML_TEX_SPR_TAG?texture&0x3FF:0;
      }
      if(binding){
        binding->kind=palette && kind==GML_TEX_SPR_TAG
          ?GML_RENDER_SHADER_TEXTURE_PALETTE:GML_RENDER_SHADER_TEXTURE_CONTENT;
        binding->sampler=generic_index;
        if(kind==GML_TEX_SPR_TAG){ binding->sprite=(texture>>10)&0xFFFF; binding->frame=texture&0x3FF; }
        else binding->surface=texture&0xFFFF;
      }
      return 1;
    }
  }
  if(kind!=GML_TEX_SPR_TAG) return 0;
  int sprite=(texture>>10)&0xFFFF;
  int frame=texture&0x3FF;
  (void)shader; (void)slot;
  r->lut_pal_sprite=sprite;
  r->lut_pal_frame=frame;
  if(binding){
    binding->kind=GML_RENDER_SHADER_TEXTURE_PALETTE;
    binding->sprite=sprite;
    binding->frame=frame;
  }
  return 1;
}

#undef GML_RENDER_SHADER_HANDLE
#undef GML_RENDER_SHADER_HANDLE_STRIDE
int gml_render_shader_content_candidate(const GmlRender *r,int shader){
  if(!r || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  {
    const struct GmlShaderPal *p=&r->shader_pal[shader];
    if(p->gpu_failed || p->gpu_binding_overflow) return 0;
    /* A fragment that samples nothing carries none of the picture it is drawn over, so leaving it
     * unrun is not a weaker version of the effect but an unrelated one. It is a candidate only
     * where a device is present to execute it. */
    if(p->procedural && !r->shader_device_present) return 0;
    if(!p->source_vertex_es || !p->source_fragment_es) return 0;
    return !shader_pal_recognized(p);
  }
}
int gml_render_content_device_candidate_present(const GmlRender *r){
  if(!r || !r->shader_pal) return 0;
  for(int shader=0;shader<r->n_shader_pal;shader++){
    const struct GmlShaderPal *p=&r->shader_pal[shader];
    if(p->source_vertex_es && p->source_fragment_es &&
       !p->gpu_binding_overflow && !shader_pal_recognized(p)) return 1;
  }
  return 0;
}
int gml_render_shader_sources(const GmlRender *r,int shader,GmlRenderShaderSources *out){
  if(!out) return 0;
  memset(out,0,sizeof *out);
  if(!r || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  out->vertex_es=r->shader_pal[shader].source_vertex_es;
  out->fragment_es=r->shader_pal[shader].source_fragment_es;
  out->vertex_gl=r->shader_pal[shader].source_vertex_gl;
  out->fragment_gl=r->shader_pal[shader].source_fragment_gl;
  return out->vertex_es && out->fragment_es;
}

/* Resolve the finite source-byte partition as metadata. The device compares byte indices;
 * it does not emulate the software evaluator's floating-point normalization per fragment. */
static unsigned threshold_palette_byte_start(float threshold,int upper){
  unsigned value=0;
  for(;value<256u;value++){
    float normalized=(float)value*(1.0f/255.0f);
    if(upper?normalized>=threshold:normalized>threshold) break;
  }
  return value;
}

int gml_render_shader_threshold_palette_snapshot(const GmlRender *r,int shader_id,
                                                GmlPlanShader *out){
  const struct GmlShaderPal *p;
  if(!r || !out || !r->shader_pal || shader_id<0 || shader_id>=r->n_shader_pal) return 0;
  p=&r->shader_pal[shader_id];
  int indexed=!p->threshold_palette && p->indexed_brightness && p->indexed_brightness_set;
  if((!indexed && (!p->threshold_palette || p->threshold_palette_set!=0x3ffu)) ||
     p->vertex_colour_blend || p->generic_sampler_count || p->generic_uniform_count) return 0;
  if(indexed && (p->vertex_colour_blend_alpha_only || !isfinite(p->indexed_brightness_value))) return 0;
  float red_threshold=indexed?p->indexed_brightness_red:p->threshold_palette_red;
  const float (*green_threshold)[4]=indexed?p->indexed_brightness_green:p->threshold_palette_green;
  if(!isfinite(red_threshold) || red_threshold<0.0f || red_threshold>1.0f) return 0;
  for(unsigned branch=0;branch<2u;branch++) for(unsigned band=0;band<4u;band++){
    float value=green_threshold[branch][band];
    if(!isfinite(value) || value<0.0f || value>1.0f ||
       (band && value>=green_threshold[branch][band-1u])) return 0;
  }
  memset(out,0,sizeof *out);
  out->identity=(uint32_t)shader_id;
  out->builtin_kind=GML_PLAN_BUILTIN_SHADER_THRESHOLD_PALETTE;
  for(unsigned channel=0;channel<4u;channel++) out->vertex_colour[channel]=1.0f;
  for(unsigned index=0;index<10u;index++){
    GmlPlanUniform *uniform=&out->uniforms[out->uniform_count++];
    snprintf(uniform->name,sizeof uniform->name,"u_palette%u",index);
    uniform->count=3u;
    if(indexed){
      /* Each nonempty byte partition has one representative. Empty bands are
       * unreachable by the same integer thresholds used by the device. Reuse
       * the existing evaluator ten times, never once per source pixel. */
      unsigned branch=index/5u,band=index%5u;
      unsigned green=band<4u?threshold_palette_byte_start(green_threshold[branch][band],0):0u;
      if(green>255u) green=255u;
      uint32_t mapped=indexed_brightness_map_px(p,0xFF000000u|
        (branch?0x00FF0000u:0u)|(green<<8));
      for(unsigned channel=0;channel<3u;channel++)
        uniform->value[channel]=(float)((mapped>>(16u-8u*channel))&255u)/255.0f;
    }else for(unsigned channel=0;channel<3u;channel++)
      uniform->value[channel]=(float)p->threshold_palette_colour[index][channel]/255.0f;
  }
  GmlPlanUniform *red=&out->uniforms[out->uniform_count++];
  snprintf(red->name,sizeof red->name,"u_threshold_red");
  red->count=1u;
  red->integer=1u;
  red->value[0]=(float)threshold_palette_byte_start(red_threshold,1);
  for(unsigned branch=0;branch<2u;branch++){
    GmlPlanUniform *green=&out->uniforms[out->uniform_count++];
    snprintf(green->name,sizeof green->name,"u_threshold_green%u",branch);
    green->count=4u;
    green->integer=1u;
    for(unsigned band=0;band<4u;band++)
      green->value[band]=(float)threshold_palette_byte_start(green_threshold[branch][band],0);
  }
  GmlPlanUniform *bypass=&out->uniforms[out->uniform_count++];
  snprintf(bypass->name,sizeof bypass->name,"u_palette_bypass");
  bypass->count=1u;
  bypass->integer=1u;
  GmlPlanUniform *coverage=&out->uniforms[out->uniform_count++];
  snprintf(coverage->name,sizeof coverage->name,"u_palette_alpha");
  coverage->count=2u;
  coverage->integer=1u;
  coverage->value[1]=-1.0f;
  return 1;
}

int gml_render_shader_lut_snapshot(GmlRender *r,int shader_id,GmlPlanShader *out,
                                    GmlRenderResourceSource *palette){
  if(!r || !out || !palette || !r->shader_pal || shader_id<0 ||
     shader_id>=r->n_shader_pal || !r->spr || r->lut_pal_sprite<0 ||
     r->lut_pal_sprite>=r->n_spr) return 0;
  const struct GmlShaderPal *p=&r->shader_pal[shader_id];
  if(!p->lut || p->lut_indexed || p->vertex_colour_blend ||
     p->vertex_colour_blend_alpha_only || !isfinite(p->lut_row) || p->lut_row<0.0f ||
     p->generic_sampler_count<0 || p->generic_sampler_count>GML_PLAN_MAX_SAMPLERS) return 0;
  if(p->threshold_palette || p->indexed_brightness || p->channel_mask ||
     p->channel_alpha_key || p->alpha_discard || p->ordered_dither || p->quantise4 ||
     p->solid_alpha_mask || p->solid_blur_alpha || p->grid || p->dual_sample ||
     p->radial_wave || p->uv_wave_mode || p->grayscale) return 0;
  GmlSprite *sprite=&r->spr[r->lut_pal_sprite];
  if(sprite->w<=0 || sprite->h<=0 || sprite->w>4096 || sprite->h>4096 ||
     sprite->n_frames<=0) return 0;
  int linear=r->texture_filter[0];
  for(int index=0;index<p->generic_sampler_count;index++)
    if(!strcmp(p->generic_sampler[index].name,p->lut_sampler)){
      linear=r->texture_filter[index];break;
    }
  int layout[4]={0,0,0,0},extent[4]={sprite->w,sprite->h,sprite->w,sprite->h};
  if(sprite->runtime_rgba){
    if(!gml_render_runtime_sprite_scene_source_view(r,sprite,r->lut_pal_frame,palette)) return 0;
  } else {
    if(!sprite->frame) return 0;
    int frame=((r->lut_pal_frame%sprite->n_frames)+sprite->n_frames)%sprite->n_frames;
    int page=sprite->frame[frame];
    if(page<0 || page>=r->n_tpag) return 0;
    const GmlTpag *t=&r->tpag[page];
    if(!gml_render_atlas_scene_source_view(r,t->atlas,palette) || t->sx<0 || t->sy<0 ||
       t->tx<0 || t->ty<0 || t->sw<=0 || t->sh<=0 ||
       t->sw>sprite->w-t->tx || t->sh>sprite->h-t->ty ||
       (uint32_t)t->sx>palette->width || (uint32_t)t->sy>palette->height ||
       (uint32_t)t->sw>palette->width-(uint32_t)t->sx ||
       (uint32_t)t->sh>palette->height-(uint32_t)t->sy) return 0;
    layout[0]=t->sx;layout[1]=t->sy;layout[2]=t->tx;layout[3]=t->ty;
    extent[2]=t->sw;extent[3]=t->sh;
  }
  /* Missing trim texels use each source fragment as the CPU fallback. Hardware
   * filtering cannot interpolate that varying fallback at a stored trim edge. */
  if(linear && (layout[2] || layout[3] || extent[0]!=extent[2] || extent[1]!=extent[3])) return 0;
  memset(out,0,sizeof *out);
  out->identity=(uint32_t)shader_id;
  out->builtin_kind=GML_PLAN_BUILTIN_SHADER_LUT;
  for(unsigned channel=0;channel<4u;channel++) out->vertex_colour[channel]=1.0f;
  static const char *names[]={"u_lut_origin","u_lut_extent","u_lut_row",
                             "u_lut_v","u_lut_linear","u_palette_bypass"};
  out->uniform_count=6u;
  for(unsigned index=0;index<6u;index++){
    GmlPlanUniform *uniform=&out->uniforms[index];
    snprintf(uniform->name,sizeof uniform->name,"%s",names[index]);
    uniform->count=index<2u?4u:1u;
    uniform->integer=index!=3u;
  }
  for(unsigned channel=0;channel<4u;channel++){
    out->uniforms[0].value[channel]=(float)layout[channel];
    out->uniforms[1].value[channel]=(float)extent[channel];
  }
  /* Clamp before converting: a finite row need not fit in an integer. Keep the
   * CPU float product for nearest-row selection, not a fragment float emulation. */
  float row=p->lut_row*(float)sprite->h;
  out->uniforms[2].value[0]=row>=(float)sprite->h?(float)(sprite->h-1):(float)(int)row;
  out->uniforms[3].value[0]=fminf(p->lut_row,1.0f);
  out->uniforms[4].value[0]=linear?1.0f:0.0f;
  out->sampler_count=1u;
  snprintf(out->samplers[0].name,sizeof out->samplers[0].name,"u_lut_texture");
  out->samplers[0].image=1u;
  out->samplers[0].linear=linear?1u:0u;
  return 1;
}

int gml_render_shader_plan_snapshot(const GmlRender *r,int shader_id,
                                    uint32_t content_generation,const float colour[4],
                                    GmlPlanShader *out){
  GmlRenderShaderSources sources;
  GmlRenderShaderUniform uniforms[GML_PLAN_MAX_UNIFORMS];
  GmlRenderShaderSampler samplers[GML_PLAN_MAX_SAMPLERS];
  uint32_t count;
  if(!out || !r || !r->shader_pal || shader_id<0 || shader_id>=r->n_shader_pal ||
     r->shader_pal[shader_id].gpu_binding_overflow ||
     r->shader_pal[shader_id].generic_uniform_count<0 ||
     r->shader_pal[shader_id].generic_uniform_count>GML_PLAN_MAX_UNIFORMS ||
     r->shader_pal[shader_id].generic_sampler_count<0 ||
     r->shader_pal[shader_id].generic_sampler_count>GML_PLAN_MAX_SAMPLERS ||
     !gml_render_shader_sources(r,shader_id,&sources)) return 0;
  memset(out,0,sizeof *out);
  out->identity=(uint32_t)shader_id;
  out->content_generation=content_generation;
  if(colour) memcpy(out->vertex_colour,colour,sizeof out->vertex_colour);
  else for(int component=0;component<4;component++) out->vertex_colour[component]=1.0f;
  out->vertex_es=sources.vertex_es;
  out->fragment_es=sources.fragment_es;
  out->vertex_gl=sources.vertex_gl;
  out->fragment_gl=sources.fragment_gl;
  count=gml_render_shader_uniforms(r,shader_id,uniforms,GML_PLAN_MAX_UNIFORMS);
  for(uint32_t index=0;index<count;index++){
    GmlPlanUniform *value=&out->uniforms[out->uniform_count++];
    memcpy(value->name,uniforms[index].name,sizeof value->name);
    memcpy(value->value,uniforms[index].value,sizeof value->value);
    value->count=uniforms[index].count;
    value->integer=uniforms[index].integer;
  }
  count=gml_render_shader_samplers(r,shader_id,samplers,GML_PLAN_MAX_SAMPLERS);
  for(uint32_t index=0;index<count;index++){
    GmlPlanSampler *sampler=&out->samplers[out->sampler_count++];
    memcpy(sampler->name,samplers[index].name,sizeof sampler->name);
    sampler->image=GML_PLAN_NO_IMAGE;
    sampler->linear=samplers[index].interpolation?1u:0u;
  }
  return 1;
}
uint32_t gml_render_shader_uniforms(const GmlRender *r,int shader,
                                    GmlRenderShaderUniform *out,uint32_t capacity){
  uint32_t written=0;
  if(!r || !out || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  {
    const struct GmlShaderPal *p=&r->shader_pal[shader];
    for(int index=0;index<p->generic_uniform_count && written<capacity;index++){
      if(p->generic_uniform[index].count<=0) continue;
      memset(&out[written],0,sizeof out[written]);
      snprintf(out[written].name,sizeof out[written].name,"%s",p->generic_uniform[index].name);
      memcpy(out[written].value,p->generic_uniform[index].value,sizeof out[written].value);
      out[written].count=(uint32_t)p->generic_uniform[index].count;
      out[written].integer=(uint32_t)p->generic_uniform[index].integer;
      written++;
    }
  }
  return written;
}
uint32_t gml_render_shader_samplers(const GmlRender *r,int shader,
                                    GmlRenderShaderSampler *out,uint32_t capacity){
  uint32_t written=0;
  if(!r || !out || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  {
    const struct GmlShaderPal *p=&r->shader_pal[shader];
    for(int index=0;index<p->generic_sampler_count && written<capacity;index++){
      memset(&out[written],0,sizeof out[written]);
      snprintf(out[written].name,sizeof out[written].name,"%s",p->generic_sampler[index].name);
      out[written].texture=p->generic_sampler[index].texture;
      out[written].interpolation=r->texture_filter[index];
      out[written].sprite=-1;
      out[written].frame=-1;
      out[written].surface=-1;
      if(out[written].texture>0){
        unsigned code=(unsigned)out[written].texture;
        if((code&GML_TEX_KIND_MASK)==GML_TEX_SURF_TAG) out[written].surface=(int)(code&0xFFFFu);
        else if((code&GML_TEX_KIND_MASK)==GML_TEX_SPR_TAG){
          out[written].sprite=(int)((code>>10)&0xFFFFu);
          out[written].frame=(int)(code&0x3FFu);
        }
      }
      written++;
    }
  }
  return written;
}
void gml_render_shader_mark_failed(GmlRender *r,int shader){
  if(!r || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return;
  r->shader_pal[shader].gpu_failed=1;
}
void gml_render_set_shader_executor(GmlRender *r,GmlRenderShaderExecutor executor,void *context){
  if(!r) return;
  r->shader_executor=executor;
  r->shader_executor_context=executor?context:NULL;
}
void gml_render_set_shader_device(GmlRender *r,int present){
  if(r) r->shader_device_present=present?1:0;
}
int gml_render_shader_device_present(const GmlRender *r){ return r?r->shader_device_present:0; }
void gml_render_set_shader_device_expected(GmlRender *r,int expected){ if(r) r->shader_device_expected=expected?1:0; }

uint32_t gml_render_shader_executed_count(const GmlRender *r){ return r?r->shaded_draws:0u; }

/* What the content has set on a program, as one number. Two draws with the same fingerprint get
 * the same picture out of the same source, which is what lets a shaded frame be reused. */
uint64_t gml_render_shader_uniform_fingerprint(const GmlRender *r,int shader){
  uint64_t hash=1469598103934665603ull;
  if(!r || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  {
    const struct GmlShaderPal *p=&r->shader_pal[shader];
    const unsigned char *bytes;
    size_t count;
    bytes=(const unsigned char*)p->generic_uniform;
    count=(size_t)p->generic_uniform_count*sizeof p->generic_uniform[0];
    for(size_t i=0;i<count;i++){ hash^=bytes[i]; hash*=1099511628211ull; }
    for(int i=0;i<p->generic_sampler_count;i++){
      unsigned t=(unsigned)p->generic_sampler[i].texture;
      for(int b=0;b<4;b++){ hash^=(unsigned char)(t>>(b*8)); hash*=1099511628211ull; }
    }
  }
  return hash;
}

void gml_render_shader_account(GmlRender *r,int shader,int kind,int executed){
  if(!r || shader<0 || shader>=r->n_shader_pal) return;
  if(kind<0 || kind>=GML_RENDER_SHADER_DRAW_KINDS) kind=GML_RENDER_SHADER_DRAW_OTHER;
  if(!r->shader_account){
    size_t cells=(size_t)r->n_shader_pal*GML_RENDER_SHADER_DRAW_KINDS*2u;
    if(!cells) return;
    r->shader_account=(uint32_t*)calloc(cells,sizeof *r->shader_account);
    if(!r->shader_account) return;
    r->shader_account_shaders=r->n_shader_pal;
  }
  if(shader>=r->shader_account_shaders) return;
  r->shader_account[((size_t)shader*GML_RENDER_SHADER_DRAW_KINDS+(size_t)kind)*2u+(executed?1u:0u)]++;
}

/* One line per shader that was drawn with, naming what reached the device and what did not. */
void gml_render_shader_account_report(GmlRender *r){
  static const char *kinds[GML_RENDER_SHADER_DRAW_KINDS]={"surface","sprite","rect","text","other"};
  if(!r || !r->shader_account) return;
  for(int shader=0;shader<r->shader_account_shaders;shader++){
    uint32_t executed=0,plain=0;
    char detail[240];
    size_t used=0;
    for(int kind=0;kind<GML_RENDER_SHADER_DRAW_KINDS;kind++){
      uint32_t no=r->shader_account[((size_t)shader*GML_RENDER_SHADER_DRAW_KINDS+(size_t)kind)*2u];
      uint32_t yes=r->shader_account[((size_t)shader*GML_RENDER_SHADER_DRAW_KINDS+(size_t)kind)*2u+1u];
      executed+=yes;
      plain+=no;
      if((yes||no) && used<sizeof detail-40)
        used+=(size_t)snprintf(detail+used,sizeof detail-used," %s=%u/%u",kinds[kind],yes,yes+no);
    }
    if(!executed && !plain) continue;
    detail[used]='\0';
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_INFO,
      "[shader-account] shader=%d executed=%u plain=%u recognized=%d procedural=%d device=%d%s\n",
      shader,executed,plain,
      r->shader_pal?shader_pal_recognized(&r->shader_pal[shader]):0,
      r->shader_pal?r->shader_pal[shader].procedural:0,
      r->shader_device_present,detail);
  }
}
