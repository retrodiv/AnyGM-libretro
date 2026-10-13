/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared text layout, drawing, and classic-font glyph emission. */
#include "gml_render_internal.h"
#include "gml_render_text_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_sprites_internal.h"
#include "gml_render_cpu_text.h"
#include "gml_render_cpu_backend.h"

#include "anygm_compatibility.h"
#include "anygm_host.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Layout resolves mapping before admission. The bounded nearest glyph owner
 * accepts signed scales without changing either font's placement policy. */
static int resident_nearest_text_scale(double xs,double ys){
  return isfinite(xs) && isfinite(ys) &&
         fabs(xs)>=0.0625 && fabs(xs)<=64.0 &&
         fabs(ys)>=0.0625 && fabs(ys)<=64.0;
}



static unsigned text_next_cp(const char **pp){
  if((*pp)[0]=='\\' && (*pp)[1]=='#'){ *pp+=2; return '#'; }
  return utf8_next(pp);
}

/* String measurement keeps a modern hash literal. Drawing still accepts the authored
 * text separator so multi-line descriptions retain their intended layout. */
static int text_is_linebreak(GmlRender *r,const char *p,int drawing){
  return p && (*p=='\r' || *p=='\n' ||
    (*p=='#' && (drawing || anygm_policy_text_uses_hash_line_breaks(r?r->win:NULL))));
}
typedef enum {
  ANYGM_TEXT_LAYOUT_PLAIN,
  ANYGM_TEXT_LAYOUT_EXTENDED,
  ANYGM_TEXT_LAYOUT_SPRITE
} AnygmTextLayoutFamily;
/* Return the byte width of a logical line separator. Extended text consumes a CR LF pair
 * together; plain and sprite text follow the generation policy, including the classic rule
 * that treats each control character as a separate break. */
static int text_linebreak_bytes(GmlRender *r, const char *p,
                                AnygmTextLayoutFamily family,int drawing){
  if(!text_is_linebreak(r,p,drawing)) return 0;
  if(p[0]=='\r' && p[1]=='\n' && r &&
     (family==ANYGM_TEXT_LAYOUT_EXTENDED ||
      anygm_policy_text_pairs_carriage_return_with_line_feed(r->win))) return 2;
  return 1;
}

/* Width of one policy-delimited line, retaining the existing escaped-hash reading. */
static int line_width(GmlRender *r, GmlFont *f, const char *p, const char **end,int drawing){
  int w=0;
  while(*p && !text_is_linebreak(r,p,drawing)){
    unsigned cp;
    if(p[0]=='\\' && p[1]=='#'){ cp='#'; p+=2; }
    else cp=text_next_cp(&p);
    w += glyph_w(r,f,glyph_frame(f,cp),cp) + f->sep;
  }
  if(w>0) w-=f->sep;
  *end=p; return w;
}
/* Advance width of one policy-delimited line; escaped hashes retain their existing reading. */
static int real_line_width(GmlRender *r, GmlFont *f, const char *p, const char **end,int drawing){
  int w=0; unsigned previous=0;
  while(*p && !text_is_linebreak(r,p,drawing)){
    unsigned cp;
    if(p[0]=='\\' && p[1]=='#'){ cp='#'; p+=2; }
    else cp=text_next_cp(&p);
    GmlGlyph *g=real_glyph_demand(r,f,cp);
    /* Apply the pair adjustment before adding the current glyph's advance. */
    if(previous) w+=font_kerning(f,previous,cp);
    if(g) w+=g->shift;
    previous=cp;
  }
  *end=p; return w;
}

/* Text alignment is quantized after the authored horizontal transform. Convert the integer half
 * of that transformed width back to the font's local coordinates so odd source widths keep the
 * measured whole-pixel placement at scale one without moving one pixel right at larger scales.
 * Presentation scaling does not magnify this layout scale. When it cancels an authored reduction
 * and leaves at least a one-pixel raster, retain the source-pixel grid instead of quantizing in
 * multi-source-pixel jumps. */
static double centred_line_offset(int line_width, double xscale){
  double magnitude=fabs(xscale);
  if(!(magnitude>0.0) || !isfinite(magnitude)) return -(double)(line_width/2);
  return -floor((double)line_width*magnitude/2.0)/magnitude;
}

/* A real font under an authored reduction keeps half of its source width. At scale one,
 * alignment uses the source-pixel grid; at enlargement, it quantizes the transformed width.
 * Quantizing each reduced scale would move the centre across unrelated source phases. Keep
 * sprite-font reductions on their established path. */
static double centred_real_line_offset(int line_width, double xscale){
  double magnitude=fabs(xscale);
  if(magnitude>0.0 && magnitude<1.0 && isfinite(magnitude))
    return -(double)line_width/2.0;
  return centred_line_offset(line_width,xscale);
}

/* Draw a string with a real FONT-chunk font: each glyph is an atlas sub-rect drawn top-aligned
 * at the baseline-top (GM bakes the ascent whitespace into the glyph height), advancing by shift.
 * A transformed draw rotates both the pen and each glyph quad around that pen. */
static void draw_text_real_plain(GmlRender *r, GmlFont *f, double x, double y, const char *str,
                                 double xs, double ys, double alignment_xscale, double rotation,
                                 double ca, double sa, int use_rot, uint32_t blend, double alpha,
                                 AnygmTextLayoutFamily family);

/* A run drawn through a program that reads its place on the target would pay a device round trip
 * per glyph. Gathering the run onto a plane first and running the program once over the rectangle
 * it covered is the same picture — the program sees the same positions — for one pass. */
static void draw_text_real(GmlRender *r, GmlFont *f, double x, double y, const char *str,
                           double xs, double ys, double alignment_xscale, double rotation,
                           double ca, double sa, int use_rot, uint32_t blend, double alpha,
                           AnygmTextLayoutFamily family){
  uint32_t *saved_fb=NULL;
  int saved_shader=-1;
  if(gml_render_shaded_run_wanted(r) && !use_rot &&
     gml_render_shaded_run_begin(r,&saved_fb,&saved_shader)){
    /* The rectangle the run can reach: its own extent, grown by a line either way so a glyph that
     * hangs above or below its cell is inside it, and clipped to the target. */
    double width=gml_text_width(r,str)*(xs>0?xs:1.0);
    int lh=f->line_height>0?f->line_height:12;
    int nlines=1;
    int x0,y0,x1,y1,margin;
    for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;}
      if(text_is_linebreak(r,q,1)){ nlines++; q+=text_linebreak_bytes(r,q,family,1)-1; } }
    margin=(int)(lh*(ys>0?ys:1.0))+8;
    x0=(int)floor(x-r->cam_x-width-margin);
    y0=(int)floor(y-r->cam_y-margin);
    x1=(int)ceil(x-r->cam_x+width+margin);
    y1=(int)ceil(y-r->cam_y+nlines*lh*(ys>0?ys:1.0)+margin);
    /* Gather plain pixels; pass the drawing colour separately as quad vertex colour. */
    draw_text_real_plain(r,f,x,y,str,xs,ys,alignment_xscale,rotation,ca,sa,use_rot,0xFFFFFFu,1.0,family);
    GmlRenderCpuBackendDispatchScope composition;
    gml_render_cpu_backend_dispatch_begin(r,&composition,GML_RENDER_SCENE_TEXT,0);
    gml_render_shaded_run_end(r,saved_fb,saved_shader,x0,y0,x1,y1,blend,alpha);
    gml_render_cpu_backend_dispatch_end(r,&composition);
    return;
  }
  draw_text_real_plain(r,f,x,y,str,xs,ys,alignment_xscale,rotation,ca,sa,use_rot,blend,alpha,family);
}

static void draw_text_real_plain(GmlRender *r, GmlFont *f, double x, double y, const char *str,
                                 double xs, double ys, double alignment_xscale, double rotation,
                                 double ca, double sa, int use_rot, uint32_t blend, double alpha,
                                 AnygmTextLayoutFamily family){
  int lh=f->line_height>0? f->line_height:12;
  int ah=f->align_height>0?f->align_height:lh;
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;}
    if(text_is_linebreak(r,q,1)){ nlines++; q+=text_linebreak_bytes(r,q,family,1)-1; } }
  double base_y=0;
  double block_height=(nlines-1)*lh+ah;
  if(r->valign==1) base_y=-block_height/2.0; else if(r->valign==2) base_y=-block_height;
  const char *p=str;
  int log_glyphs=render_setting(r,"GML_LOG_TEXT_GLYPHS")!=NULL;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=real_line_width(r,f,p,&end,1);
    double cx=0;
    if(r->halign==1) cx=centred_real_line_offset(lw,alignment_xscale);
    else if(r->halign==2) cx=-lw;
    if(log_glyphs) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
      "[tg] line=%d lw=%d cx0=%.2f x=%.2f y=%.2f xs=%.3f ys=%.3f interp=%d\n",
      li,lw,cx,x,y,xs,ys,r->interp);
    unsigned previous=0;
    while(p<end){
      unsigned cp=text_next_cp(&p);
      GmlGlyph *g=real_glyph_demand(r,f,cp);
      if(previous) cx+=font_kerning(f,previous,cp);
      previous=cp;
      if(log_glyphs) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
        "[tg]   cp=%u('%c') cx=%.2f gx=%.2f src=(%d,%d %dx%d) shift=%d off=%d\n",
        cp,(cp>=32&&cp<127)?(char)cp:'?',cx,x+(cx+(g?g->offset:0))*xs,
        g?g->sx:-1,g?g->sy:-1,g?g->w:-1,g?g->h:-1,g?g->shift:-1,g?g->offset:-1);
      if(g && g->w>0 && g->h>0){
        GmlTpag gt={ .sx=g->sx,.sy=g->sy,.sw=g->w,.sh=g->h,.tx=0,.ty=0,.bw=g->w,.bh=g->h,.atlas=f->atlas };
        /* A signed-distance glyph cell carries one spread of filterable padding around its
         * contour. Position the padded cell before the authored pen so the visible contour
         * retains its intended location. */
        double packing_spread=f->sdf_spread>0?(double)f->sdf_spread:0.0;
        double dx=(cx+g->offset-packing_spread)*xs;
        double dy=(base_y-f->ascender_offset-packing_spread)*ys;
        double glyph_x=use_rot?x+dx*ca+dy*sa:x+dx;
        double glyph_y=use_rot?y-dx*sa+dy*ca:y+dy;
        /* The runner places a glyph at whole pixels of the destination. A sub-pixel draw offset
         * therefore moves the letter by one pixel when it crosses the rounding boundary and never
         * changes the glyph's own shape. Sampling the cell at a fractional phase instead trims or
         * repeats an edge column, which reads as a flicker on the glyph rather than as movement. */
        if(!use_rot && !f->subpixel){
          /* A reduced draw has no whole destination pixel per source texel: rounding the origin
           * re-phases the cell and drops a source column — the gap an odd-width centred line
           * carries between its strokes disappears. Keep the sub-pixel origin below scale one. */
          if(fabs(xs)>=1.0) glyph_x=floor(glyph_x+0.5);
          /* Top-aligned text in an explicit surface retains its authored vertical
           * sampling phase across lines. Direct draws and aligned blocks land
           * their glyphs on destination pixels. */
          if(fabs(ys)>=1.0 && (r->target_id<0 || r->valign!=0))
            glyph_y=floor(glyph_y+0.5);
        }
        uint32_t glyph_blend=f->subpixel?0xFFFFFFu:blend;
        int resident_glyph=0;
        if(r->resident_application_pass && !use_rot && !f->runtime_owned &&
           !f->subpixel && f->sdf_spread<=0 && !r->software_overlay &&
           xs==1.0 && ys==1.0 && glyph_blend==0xFFFFFFu && alpha==1.0 &&
           gml_d3_is_active(r))
          resident_glyph=gml_render_resident_ortho_font_glyph(r,f->atlas,
            g->sx,g->sy,g->w,g->h,glyph_x,glyph_y);
        if(r->resident_application_pass && !use_rot &&
           !f->subpixel && f->sdf_spread<=0 &&
           !r->software_overlay && !gml_d3_is_active(r) &&
           isfinite(alpha) && alpha>=0.0 && alpha<=1.0){
          if(r->interp && !r->classic && !f->runtime_owned &&
             isfinite(xs) && isfinite(ys) &&
             xs>0.0 && ys>0.0 && xs<=1.0 && ys<=1.0)
            resident_glyph=gml_render_resident_filtered_font_glyph(
              r,f->atlas,g->sx,g->sy,g->w,g->h,
              glyph_x-r->cam_x,glyph_y-r->cam_y,xs,ys,glyph_blend,alpha);
          else if((!r->interp || r->classic) &&
                  ((!f->runtime_owned && resident_nearest_text_scale(xs,ys)) ||
                   (f->runtime_owned && alpha==1.0 && isfinite(xs) && isfinite(ys) &&
                    xs>=1.0 && xs<=4.0 && ys>=1.0 && ys<=4.0 &&
                    floor(xs)==xs && floor(ys)==ys)))
            resident_glyph=gml_render_resident_font_glyph(r,f->atlas,g->sx,g->sy,
              g->w,g->h,glyph_x-r->cam_x,glyph_y-r->cam_y,xs,ys,glyph_blend,alpha);
        }
        if(!resident_glyph){
          if(r->resident_application_pass &&
             !gml_render_resident_application_cpu_barrier(r)) return;
          int saved_interp=r->interp;
          int saved_sdf_active=r->font_sdf_active;
          double saved_sdf_width=r->font_sdf_width;
          if(f->sdf_spread>0){
            double scale=fmin(fabs(xs),fabs(ys));
            if(scale<1.0/1024.0) scale=1.0/1024.0;
            r->font_sdf_active=1;
            r->font_sdf_width=255.0/(2.0*(double)f->sdf_spread*scale);
            if(r->font_sdf_width<1.0) r->font_sdf_width=1.0;
            else if(r->font_sdf_width>255.0) r->font_sdf_width=255.0;
            /* Distance fields need a filtered distance sample even when ordinary textures are
             * point sampled; coverage is still reconstructed at each output pixel. */
            r->interp=1;
          }
          if(use_rot){
            GmlRenderCpuBackendDispatchScope glyph_scope;
            gml_render_cpu_backend_dispatch_begin(r,&glyph_scope,GML_RENDER_SCENE_TEXT,0);
            GmlSprite glyph_sprite={.w=g->w,.h=g->h,.originx=0,.originy=0};
            blit_rotated(r,&glyph_sprite,&gt,glyph_x-r->cam_x,glyph_y-r->cam_y,
                         xs,ys,rotation,blend,alpha);
            gml_render_cpu_backend_dispatch_end(r,&glyph_scope);
          } else if(f->subpixel && r->software_overlay &&
             gml_render_cpu_classic_info_subpixel_glyph(r,f,g,glyph_x-r->cam_x,glyph_y-r->cam_y,
                                                         xs,ys,blend,alpha)){
            /* Per-channel coverage was composed directly above. */
          } else if(r->software_overlay ||
             !gml_d3_draw_atlas_part_2d(r,f->atlas,g->sx,g->sy,g->w,g->h,
                                        glyph_x,glyph_y,xs,ys,glyph_blend,alpha)){
            GmlRenderCpuBackendDispatchScope glyph_scope;
            gml_render_cpu_backend_dispatch_begin(r,&glyph_scope,GML_RENDER_SCENE_TEXT,0);
            /* A glyph drawn through a program this renderer does not execute is a rectangle of a
             * texture page like any other: the program's answer for it is evaluated once and kept,
             * so a screen of text costs one device evaluation per distinct glyph rather than one
             * per glyph drawn. The plain blit stands when there is nothing to run it. */
            if(!(xs>0 && ys>0 &&
                 gml_render_shade_atlas_rect_at(r,gt.atlas,gt.sx,gt.sy,gt.sw,gt.sh,
                                                glyph_x,glyph_y,gt.sw*xs,gt.sh*ys,
                                                glyph_blend,alpha)))
              blit(r,&gt,glyph_x-r->cam_x,glyph_y-r->cam_y,xs,ys,glyph_blend,alpha);
            gml_render_cpu_backend_dispatch_end(r,&glyph_scope);
          }
          r->interp=saved_interp;
          r->font_sdf_active=saved_sdf_active;
          r->font_sdf_width=saved_sdf_width;
        }
      }
      if(g) cx += g->shift;
    }
    base_y += lh;
    if(text_is_linebreak(r,end,1)) p=end+text_linebreak_bytes(r,end,family,1); else break;
  }
}

static GmlFont *active_font(GmlRender *r){
  if(!r) return NULL;
  if(r->font<0)
    return r->default_font.glyphs && r->default_font.n_glyphs>0 ? &r->default_font : NULL;
  return r->font<r->n_fonts ? &r->fonts[r->font] : NULL;
}

static int text_width_font(GmlRender *r, GmlFont *f, const char *str,
                           AnygmTextLayoutFamily family){
  if(!f||!str) return 0;
  int best=0; const char *p=str;
  for(;;){
    const char *end; int w = f->real ? real_line_width(r,f,p,&end,0) : line_width(r,f,p,&end,0);
    if(w>best) best=w;
    if(!text_is_linebreak(r,end,0)) break;
    p=end+text_linebreak_bytes(r,end,family,0);
  }
  return best;
}

int gml_text_width(GmlRender *r, const char *str){
  GmlFont *f=active_font(r);
  int best=text_width_font(r,f,str,ANYGM_TEXT_LAYOUT_PLAIN);
  if(render_setting(r,"GML_LOG_WIDTH")){
    int max=200; const char *m=render_setting(r,"GML_LOG_WIDTH_MAX"); if(m) max=atoi(m);
    if(f && r->text_width_log_count<max){
      int sw=-1, nf=-1;
      if(!f->real && f->sprite>=0 && f->sprite<r->n_spr){ sw=r->spr[f->sprite].w; nf=r->spr[f->sprite].n_frames; }
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[width] font=%d real=%d sprite=%d sw=%d frames=%d prop=%d sep=%d map=%d width=%d \"%.*s\"\n",
        r->font,f->real,f->sprite,sw,nf,f->prop,f->sep,f->map_len,best,80,str);
      r->text_width_log_count++;
    }
  }
  return best;
}

static int text_height_font(GmlRender *r, GmlFont *f, const char *str,
                            AnygmTextLayoutFamily family){
  if(!f) return 0;
  int lh, nlines=1;
  if(f->real) lh=f->line_height>0?f->line_height:12;
  else { GmlSprite *s=&r->spr[f->sprite]; lh=s->h>0?s->h:8; }
  if(str) for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;}
    if(text_is_linebreak(r,p,0)){ nlines++; p+=text_linebreak_bytes(r,p,family,0)-1; } }
  return lh*nlines;
}

int gml_text_height(GmlRender *r, const char *str){
  return text_height_font(r,active_font(r),str,ANYGM_TEXT_LAYOUT_PLAIN);
}


static void draw_text_transformed_font(GmlRender *r, GmlFont *f,
                               double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha,
                               AnygmTextLayoutFamily family){
  double authored_xscale=xs;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  double alignment_xscale=authored_xscale;
  if(fabs(authored_xscale)<1.0 && fabs(xs)>=1.0)
    alignment_xscale=copysign(1.0,authored_xscale);
  if(!f||!str) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(xs==0||ys==0||alpha<=0) return;
  /* Shaded-run gathering writes a CPU target before the individual glyphs are emitted.
   * Runtime glyph demand mutates only its versioned atlas; each mutable scene submission
   * flushes before a later lookup can move that page. */
  if(f->real && r->resident_application_pass &&
     gml_render_shaded_run_wanted(r) &&
     !gml_render_resident_application_cpu_barrier(r)) return;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  double ca,sa; render_rotation_sincos(rr,&ca,&sa);
  int use_rot = fabs(rr)>0.001 && fabs(rr-360.0)>0.001;
  if(render_setting(r,"GML_LOG_TEXT")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[text] x=%.0f y=%.0f font=%d halign=%d valign=%d scale=(%.2f,%.2f) rot=%.1f col=%06X a=%.2f \"%s\"\n",
    x,y,r->font,r->halign,r->valign,xs,ys,rr,(unsigned)(blend&0xffffff),alpha,str);
  if(f->real){
    draw_text_real(r,f,x,y,str,xs,ys,alignment_xscale,rr,ca,sa,use_rot,blend,alpha,family);
    return;
  }
  if(f->sprite<0 || f->sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[f->sprite];
  if(s->n_frames<=0) return;
  /* line height = sprite cell height (GM uses the sprite's declared h, not the
   * glyph's trimmed sub-rect sh — otherwise text lines collapse when glyphs are cropped). */
  int lh=s->h; if(lh<=0) lh=8;
  /* count lines for valign */
  int nlines=1; for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;}
    if(text_is_linebreak(r,p,1)){ nlines++; p+=text_linebreak_bytes(r,p,family,1)-1; } }
  double base_y=0;
  if(r->valign==1) base_y=-(nlines*lh)/2.0; else if(r->valign==2) base_y=-nlines*lh;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=line_width(r,f,p,&end,1);
    double base_x=0;
    if(r->halign==1) base_x=centred_line_offset(lw,alignment_xscale);
    else if(r->halign==2) base_x=1-lw;
    double cx=base_x;
    while(p<end){
      unsigned cp=text_next_cp(&p);
      int fr=glyph_frame(f,cp);
      /* A sprite font advances over the space without painting its cell. An
       * authored space frame may contain nonblank pixels; the advance below
       * still uses that frame's own width, leaving text layout unchanged. */
      if(fr>=0 && fr<s->n_frames && cp!=' '){
        double glyph_ox=cx*xs,glyph_oy=base_y*ys;
        double glyph_x=use_rot?x+glyph_ox*ca+glyph_oy*sa:x+glyph_ox;
        double glyph_y=use_rot?y-glyph_ox*sa+glyph_oy*ca:y+glyph_oy;
        int resident_glyph=0;
        if(r->resident_application_pass && !use_rot && !s->runtime_rgba &&
           !gml_d3_is_active(r) && s->frame &&
           isfinite(alpha) && alpha>0.0 && alpha<=1.0 &&
           resident_nearest_text_scale(xs,ys) && (!r->interp || r->classic)){
          int page_index=s->frame[fr];
          if(page_index>=0 && page_index<r->n_tpag){
            GmlTpag *page=&r->tpag[page_index];
            double left=x+(cx-s->originx+(f->prop?0:page->tx))*xs-r->cam_x;
            double top=y+(base_y-s->originy+page->ty)*ys-r->cam_y;
            resident_glyph=gml_render_resident_sprite_font_glyph(
              r,page_index,left,top,xs,ys,blend,alpha);
          }
        }
        if(!resident_glyph){
          if(r->resident_application_pass &&
             !gml_render_resident_application_cpu_barrier(r)) return;
          if(gml_d3_draw_sprite_2d(r,f->sprite,fr,glyph_x,glyph_y,xs,ys,rr,blend,alpha)){
            /* The D3 hook projects the glyph quad at the current draw depth. */
          } else if(s->runtime_rgba){
            GmlRenderCpuBackendDispatchScope glyph_scope;
            gml_render_cpu_backend_dispatch_begin(r,&glyph_scope,GML_RENDER_SCENE_TEXT,0);
            const uint8_t *fr_rgba=runtime_frame_rgba(s,fr);
            if(fr_rgba){
              if(use_rot){
                double gx=x + cx*xs*ca + base_y*ys*sa;
                double gy=y - cx*xs*sa + base_y*ys*ca;
                const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)fr*s->h:NULL;
                const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)fr*s->h:NULL;
                blit_rgba_sprite(r,s,fr_rgba,s->w,s->h,gx,gy,xs,ys,rr,
                                 s->originx,s->originy,blend,alpha,1,rmin,rmax,
                                 s->runtime_opaque);
              } else {
                const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)fr*s->h:NULL;
                const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)fr*s->h:NULL;
                blit_rgba_sprite(r,s,fr_rgba,s->w,s->h,x+cx*xs,y+base_y*ys,xs,ys,0,
                                 s->originx,s->originy,blend,alpha,1,rmin,rmax,
                                 s->runtime_opaque);
              }
            }
            gml_render_cpu_backend_dispatch_end(r,&glyph_scope);
          } else if(s->frame){ int ti=s->frame[fr];
            if(ti>=0 && ti<r->n_tpag){ GmlTpag *t=&r->tpag[ti];
            GmlRenderCpuBackendDispatchScope glyph_scope;
            gml_render_cpu_backend_dispatch_begin(r,&glyph_scope,GML_RENDER_SCENE_TEXT,0);
            GmlTpag gt=*t;
            gt.interp_draw_cache=NULL; gt.interp_draw_runs=NULL;
            gt.interp_draw_cache_bytes=0; gt.interp_draw_run_count=0;
            gt.interp_draw_cache_valid=0; gt.interp_draw_pending_count=0;
            if(use_rot){
              double ox=cx*xs;
              double oy=base_y*ys;
              double gx=x + ox*ca + oy*sa;
              double gy=y - ox*sa + oy*ca;
              blit_rotated(r,s,&gt,gx,gy,xs,ys,rr,blend,alpha);
            } else {
              /* A proportional sprite font advances by the cropped glyph width,
               * so the cursor is already at the glyph's left edge. A fixed-cell
               * font retains the horizontal offset within its full cell. */
              blit(r,&gt, x + (cx-s->originx+(f->prop?0:gt.tx))*xs - r->cam_x,
                y + (base_y-s->originy+gt.ty)*ys - r->cam_y, xs,ys, blend, alpha);
            }
            gml_render_cpu_backend_dispatch_end(r,&glyph_scope);
            } }
          }
        }
      cx += glyph_w(r,f,fr,cp)+f->sep;
    }
    base_y += lh;
    if(text_is_linebreak(r,end,1)) p=end+text_linebreak_bytes(r,end,family,1); else break;
  }
}
void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha){
  draw_text_transformed_font(r,active_font(r),x,y,str,xs,ys,rot,blend,alpha,
                             ANYGM_TEXT_LAYOUT_PLAIN);
}
void gml_draw_text(GmlRender *r, double x, double y, const char *str){
  if(!r) return;
  gml_draw_text_transformed(r,x,y,str,1,1,0,r->color,r->alpha);
}
static int text_span_width(GmlRender *r,GmlFont *font,const char *begin,const char *end){
  int width=0; unsigned previous=0;
  const char *p=begin;
  while(p<end){
    unsigned cp;
    if(p[0]=='\\' && p+1<end && p[1]=='#'){ cp='#'; p+=2; }
    else cp=text_next_cp(&p);
    if(font->real){
      GmlGlyph *glyph=real_glyph_demand(r,font,cp);
      if(previous) width+=font_kerning(font,previous,cp);
      previous=cp;
      if(glyph) width+=glyph->shift;
    } else {
      width+=glyph_w(r,font,glyph_frame(font,cp),cp)+font->sep;
    }
  }
  if(!font->real && width>0) width-=font->sep;
  return width;
}

static const char *text_unit_end(const char *p){
  if(p[0]=='\\' && p[1]=='#') return p+2;
  const char *next=p;
  (void)text_next_cp(&next);
  return next;
}

static void text_wrap_copy(char *wrapped,size_t cap,size_t *offset,
                           const char *begin,const char *end){
  while(begin<end && *offset<cap-1) wrapped[(*offset)++]=*begin++;
}

static const char *text_wrap_ext(GmlRender *r,GmlFont *font,const char *str,
                                 double w,char *wrapped,size_t cap,
                                 AnygmTextLayoutFamily family,int drawing){
  if(!r || !font || !str || !wrapped || cap<2 || w<=0) return str;
  size_t offset=0;
  const char *p=str;
  for(;;){
    const char *explicit_end=p;
    while(*explicit_end && !text_is_linebreak(r,explicit_end,drawing))
      explicit_end=text_unit_end(explicit_end);

    const char *line_start=p;
    while(line_start<explicit_end){
      const char *scan=line_start;
      const char *fit_end=line_start;
      const char *space_end=NULL;
      while(scan<explicit_end){
        const char *next=text_unit_end(scan);
        int width=text_span_width(r,font,line_start,next);
        /* Word wrapping is decided by the first glyph that crosses the limit. A separating space
         * may move the pen past it and becomes the break point for the following word; breaking
         * on that space instead puts the preceding word on the next line. */
        if(width>(int)w && *scan!=' '){
          const char *wrap_end=space_end && space_end>line_start ? space_end : fit_end;
          if(wrap_end==line_start) wrap_end=next;
          /* A break-space belongs to neither output line. Excluding its advance keeps the
           * visible first line centred by its painted content. */
          const char *paint_end=wrap_end;
          while(paint_end>line_start && paint_end[-1]==' ') paint_end--;
          text_wrap_copy(wrapped,cap,&offset,line_start,paint_end);
          if(offset<cap-1) wrapped[offset++]='\n';
          line_start=wrap_end;
          break;
        }
        fit_end=next;
        if(*scan==' ') space_end=next;
        scan=next;
      }
      if(scan>=explicit_end){
        text_wrap_copy(wrapped,cap,&offset,line_start,explicit_end);
        line_start=explicit_end;
      }
    }

    if(!*explicit_end) break;
    if(offset<cap-1) wrapped[offset++]='\n';
    p=explicit_end+text_linebreak_bytes(r,explicit_end,family,drawing);
  }
  wrapped[offset]=0;
  return wrapped;
}

/* GM7 and later wrapped extended draws omit an empty line before the first glyph and retain one
 * empty line between nonempty lines. GM5 preserves repeated blank lines, as does its original
 * runner. Plain text and string metrics keep their own separator policy. Work on the
 * already-wrapped buffer so word breaks remain untouched. */
static void text_compact_classic_extended_blanks(char *text){
  char *read=text,*write=text;
  int have_content=0,pending_blank=0;
  while(*read){
    char *end=read;
    while(*end && *end!='\n') end++;
    int ink=0;
    for(char *p=read;p<end;p++)
      if(*p!=' ' && *p!='\t' && *p!='\r') { ink=1; break; }
    if(ink){
      if(have_content){
        *write++='\n';
        if(pending_blank) *write++='\n';
      }
      size_t length=(size_t)(end-read);
      memmove(write,read,length);
      write+=length;
      have_content=1;
      pending_blank=0;
    }else if(have_content && *end){
      pending_blank=1;
    }
    read=*end?end+1:end;
  }
  *write=0;
}

double gml_text_width_ext(GmlRender *r,const char *str,double sep,double w){
  (void)sep;
  if(!r || !str) return 0;
  GmlFont *font=active_font(r);
  char wrapped[2048];
  const char *layout=text_wrap_ext(r,font,str,w,wrapped,sizeof wrapped,
                                   ANYGM_TEXT_LAYOUT_EXTENDED,0);
  return text_width_font(r,font,layout,ANYGM_TEXT_LAYOUT_EXTENDED);
}

double gml_text_height_ext(GmlRender *r,const char *str,double sep,double w){
  if(!r || !str) return 0;
  GmlFont *font=active_font(r);
  char wrapped[2048];
  const char *layout=text_wrap_ext(r,font,str,w,wrapped,sizeof wrapped,
                                   ANYGM_TEXT_LAYOUT_EXTENDED,0);
  if(sep<0) return text_height_font(r,font,layout,ANYGM_TEXT_LAYOUT_EXTENDED);
  int lines=1;
  for(const char *p=layout;*p;p++){
    if(*p=='\\' && p[1]=='#'){ p++; continue; }
    if(text_is_linebreak(r,p,0)){
      lines++;
      p+=text_linebreak_bytes(r,p,ANYGM_TEXT_LAYOUT_EXTENDED,0)-1;
    }
  }
  double line_height=text_height_font(r,font,"",ANYGM_TEXT_LAYOUT_EXTENDED);
  return line_height+(lines-1)*sep;
}

static void draw_text_ext_transformed_font(GmlRender *r, GmlFont *font,
                                   double x, double y, const char *str,
                                   double sep, double w, double xs, double ys, double rot,
                                   uint32_t blend, double alpha,
                                   AnygmTextLayoutFamily family){
  if(!r || !str || !font) return;
  char wrapped[2048];
  str=text_wrap_ext(r,font,str,w,wrapped,sizeof wrapped,family,1);
  if(str==wrapped && anygm_policy_text_compacts_extended_blank_lines(r->win))
    text_compact_classic_extended_blanks(wrapped);
  if(sep<0){
    draw_text_transformed_font(r,font,x,y,str,xs,ys,rot,blend,alpha,family);
    return;
  }
  /* custom line separation: draw line by line at y + i*sep */
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;}
    if(text_is_linebreak(r,q,1)){ nlines++; q+=text_linebreak_bytes(r,q,family,1)-1; } }
  /* Separation is the distance between successive line origins, not the full block height.
   * The first line still occupies one font line-height; omitting it shifts even a single-line
   * centred or bottom-aligned string away from the requested anchor. */
  double block_height=text_height_font(r,font,"",family)+(nlines-1)*sep;
  double base=0;
  if(r->valign==1) base=-block_height/2.0; else if(r->valign==2) base=-block_height;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  double ca,sa; render_rotation_sincos(rr,&ca,&sa);
  int sv=r->valign; r->valign=0;
  const char *p=str; int li=0;
  char lbuf[1024];
  while(1){
    size_t k=0;
    while(*p && !(text_is_linebreak(r,p,1) &&
          !(*p=='#' && p>str && p[-1]=='\\')) && k<sizeof(lbuf)-1)
      lbuf[k++]=*p++;
    lbuf[k]=0;
    double line_y=base+li*sep;
    draw_text_transformed_font(r,font,x+line_y*ys*sa,y+line_y*ys*ca,
                               lbuf,xs,ys,rr,blend,alpha,family);
    if(!text_is_linebreak(r,p,1)) break;
    p+=text_linebreak_bytes(r,p,family,1); li++;
  }
  r->valign=sv;
}

void gml_draw_text_ext_transformed(GmlRender *r, double x, double y, const char *str,
                                   double sep, double w, double xs, double ys, double rot,
                                   uint32_t blend, double alpha){
  draw_text_ext_transformed_font(r,active_font(r),x,y,str,sep,w,xs,ys,rot,blend,alpha,
                                 ANYGM_TEXT_LAYOUT_EXTENDED);
}

/* draw_text_ext: word-wrap at pixel width `w` (-1 = none) with line separation `sep`
 * (-1 = font default). GM wraps between words. */
void gml_draw_text_ext(GmlRender *r, double x, double y, const char *str, double sep, double w){
  if(!r) return;
  gml_draw_text_ext_transformed(r,x,y,str,sep,w,1,1,0,r->color,r->alpha);
}

void gml_draw_text_sprite(GmlRender *r, double x, double y, const char *str,
                          double sep, double w, int sprite, int first, double scale){
  if(!r || sprite<0 || sprite>=r->n_spr || r->spr[sprite].n_frames<=0) return;
  GmlFont font={.sprite=sprite,.first=first,.prop=0,.sep=0};
  draw_text_ext_transformed_font(r,&font,x,y,str,sep,w,scale,scale,0,0xFFFFFF,1,
                                 ANYGM_TEXT_LAYOUT_SPRITE);
}
