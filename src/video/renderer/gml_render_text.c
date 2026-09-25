/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared text layout, drawing, and information-page rendering. */
#include "gml_render_internal.h"
#include "gml_render_text_internal.h"
#include "gml_font_raster.h"
#include "gml_hash.h"
#include "gml_default_font_data.h"
#include "gml_studio_default_font_data.h"
#include "gml_classic_info_font_data.h"

#include "anygm_compatibility.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



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

/* ClearType uses a separate coverage value for each LCD channel.  The bundled
 * classic-info atlases retain those three coverages; apply them to the requested
 * foreground colour over the already-painted information-page background. */
static int classic_info_gdi_black_component(int value){
  /* Match the integer endpoints of GDI's six-level ClearType transfer ramp. */
  if(value==15) return 14;
  if(value==26) return 25;
  if(value==46) return 45;
  if(value==56) return 54;
  if(value==29) return 28;
  if(value==72) return 71;
  return value;
}

static int classic_info_subpixel_glyph(GmlRender *r,GmlFont *font,GmlGlyph *glyph,
                                       double x,double y,double xscale,double yscale,
                                       uint32_t color,double alpha){
  if(!r||!font||!glyph||!font->subpixel||font->atlas<0||font->atlas>=r->n_atlas||
     alpha<0.999||!r->alphablend||r->blendmode!=0||xscale<=0||yscale<=0) return 0;
  GmlAtlas *atlas=&r->atlas[font->atlas];
  if(!atlas_pixels(r,font->atlas)) return 0;
  int x0=(int)floor(x+0.5),y0=(int)floor(y+0.5);
  int destination_width=(int)ceil(glyph->w*xscale);
  int destination_height=(int)ceil(glyph->h*yscale);
  int first_x=x0<0?-x0:0,first_y=y0<0?-y0:0;
  int last_x=destination_width,last_y=destination_height;
  if(x0+last_x>r->fbw) last_x=r->fbw-x0;
  if(y0+last_y>r->fbh) last_y=r->fbh-y0;
  if(first_x>=last_x||first_y>=last_y) return 1;
  int foreground_r=color&255,foreground_g=(color>>8)&255,foreground_b=(color>>16)&255;
  gml_render_maybe_prepare_draw(r);
  for(int yy=first_y;yy<last_y;yy++){
    uint32_t *destination=r->fb+(size_t)(y0+yy)*r->fbw+x0+first_x;
    int source_y=(int)floor((yy+0.5)/yscale);
    if(source_y<0) source_y=0;
    if(source_y>=glyph->h) source_y=glyph->h-1;
    for(int xx=first_x;xx<last_x;xx++,destination++){
      int source_x=(int)floor((xx+0.5)/xscale);
      if(source_x<0) source_x=0;
      if(source_x>=glyph->w) source_x=glyph->w-1;
      const uint8_t *source=atlas->px+
        ((size_t)(glyph->sy+source_y)*atlas->w+glyph->sx+source_x)*4;
      if(!source[3]) continue;
      int coverage_r=255-source[0],coverage_g=255-source[1],coverage_b=255-source[2];
      uint32_t previous=*destination;
      int background_r=(previous>>16)&255,background_g=(previous>>8)&255,background_b=previous&255;
      int output_r=(foreground_r*coverage_r+background_r*(255-coverage_r)+127)/255;
      int output_g=(foreground_g*coverage_g+background_g*(255-coverage_g)+127)/255;
      int output_b=(foreground_b*coverage_b+background_b*(255-coverage_b)+127)/255;
      if(foreground_r==0) output_r=classic_info_gdi_black_component(output_r);
      if(foreground_g==0) output_g=classic_info_gdi_black_component(output_g);
      if(foreground_b==0) output_b=classic_info_gdi_black_component(output_b);
      *destination=0xFF000000u|((uint32_t)output_r<<16)|((uint32_t)output_g<<8)|(uint32_t)output_b;
    }
  }
  return 1;
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
    gml_render_shaded_run_end(r,saved_fb,saved_shader,x0,y0,x1,y1,blend,alpha);
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
          GmlSprite glyph_sprite={.w=g->w,.h=g->h,.originx=0,.originy=0};
          blit_rotated(r,&glyph_sprite,&gt,glyph_x-r->cam_x,glyph_y-r->cam_y,
                       xs,ys,rotation,blend,alpha);
        } else if(f->subpixel && r->software_overlay &&
           classic_info_subpixel_glyph(r,f,g,glyph_x-r->cam_x,glyph_y-r->cam_y,
                                       xs,ys,blend,alpha)){
          /* Per-channel coverage was composed directly above. */
        } else if(r->software_overlay ||
           !gml_d3_draw_atlas_part_2d(r,f->atlas,g->sx,g->sy,g->w,g->h,
                                      glyph_x,glyph_y,xs,ys,glyph_blend,alpha)){
          /* A glyph drawn through a program this renderer does not execute is a rectangle of a
           * texture page like any other: the program's answer for it is evaluated once and kept,
           * so a screen of text costs one device evaluation per distinct glyph rather than one
           * per glyph drawn. The plain blit stands when there is nothing to run it. */
          if(!(xs>0 && ys>0 &&
               gml_render_shade_atlas_rect_at(r,gt.atlas,gt.sx,gt.sy,gt.sw,gt.sh,
                                              glyph_x,glyph_y,gt.sw*xs,gt.sh*ys,
                                              glyph_blend,alpha)))
            blit(r,&gt,glyph_x-r->cam_x,glyph_y-r->cam_y,xs,ys,glyph_blend,alpha);
        }
        r->interp=saved_interp;
        r->font_sdf_active=saved_sdf_active;
        r->font_sdf_width=saved_sdf_width;
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

#define CLASSIC_INFO_MAX_LINES 128
#define CLASSIC_INFO_LINE_BYTES 768
#define CLASSIC_INFO_MAX_RUNS 32
typedef struct {
  int start, length, font_size, bold, italic, underline;
  uint32_t color;
} ClassicInfoRun;
typedef struct {
  char text[CLASSIC_INFO_LINE_BYTES];
  int length, font_size, bold, italic, align;
  uint32_t color;
  ClassicInfoRun runs[CLASSIC_INFO_MAX_RUNS]; int run_count;
} ClassicInfoLine;
typedef struct { int font_size, bold, italic, underline, align; uint32_t color; } ClassicInfoStyle;

static void classic_info_line_style(ClassicInfoLine *line,const ClassicInfoStyle *style){
  if(line->length) return;
  line->font_size=style->font_size;
  line->bold=style->bold;
  line->italic=style->italic;
  line->align=style->align;
  line->color=style->color;
}
static void classic_info_append(ClassicInfoLine *line,const ClassicInfoStyle *style,
                                const char *bytes,size_t count){
  classic_info_line_style(line,style);
  if(count>(size_t)(CLASSIC_INFO_LINE_BYTES-1-line->length))
    count=(size_t)(CLASSIC_INFO_LINE_BYTES-1-line->length);
  if(count){
    ClassicInfoRun *run=line->run_count?&line->runs[line->run_count-1]:NULL;
    if(!run||run->font_size!=style->font_size||run->bold!=style->bold||
       run->italic!=style->italic||run->underline!=style->underline||
       run->color!=style->color){
      if(line->run_count<CLASSIC_INFO_MAX_RUNS){
        run=&line->runs[line->run_count++];
        *run=(ClassicInfoRun){line->length,0,style->font_size,style->bold,
                              style->italic,style->underline,style->color};
      }
    }
    memcpy(line->text+line->length,bytes,count); line->length+=(int)count;
    if(run) run->length=line->length-run->start;
  }
  line->text[line->length]='\0';
}
static void classic_info_finish_line(ClassicInfoLine *lines,int *count,
                                     ClassicInfoLine *line,const ClassicInfoStyle *style){
  if(*count>=CLASSIC_INFO_MAX_LINES) return;
  classic_info_line_style(line,style);
  while(line->length>0 && (line->text[line->length-1]==' ' || line->text[line->length-1]=='\t'))
    line->text[--line->length]='\0';
  while(line->run_count>0){
    ClassicInfoRun *run=&line->runs[line->run_count-1];
    if(run->start>=line->length){ line->run_count--; continue; }
    run->length=line->length-run->start;
    break;
  }
  lines[(*count)++]=*line;
  memset(line,0,sizeof(*line));
  classic_info_line_style(line,style);
}
static int classic_info_word(const uint8_t *text,size_t size,size_t *at,
                             char *word,size_t word_cap,int *parameter,int *has_parameter){
  size_t i=*at,n=0;
  while(i<size && ((text[i]>='A'&&text[i]<='Z')||(text[i]>='a'&&text[i]<='z'))){
    if(n+1<word_cap) word[n++]=(char)text[i];
    i++;
  }
  word[n]='\0';
  int sign=1,value=0,have=0;
  if(i<size && text[i]=='-'){ sign=-1; i++; }
  while(i<size && text[i]>='0'&&text[i]<='9'){
    have=1;
    if(value<1000000) value=value*10+(text[i]-'0');
    i++;
  }
  if(i<size && text[i]==' ') i++;
  *at=i; *parameter=value*sign; *has_parameter=have;
  return n>0;
}
static int classic_info_parse(const uint8_t *record,size_t record_size,
                              ClassicInfoLine *lines,int *line_count){
  if(!record || record_size<12) return 0;
  uint32_t caption=u32(record,8);
  size_t text_length_at=12u+(size_t)caption+8u*4u+8u;
  if(text_length_at>record_size || record_size-text_length_at<4u) return 0;
  uint32_t text_length=u32(record,(uint32_t)text_length_at);
  size_t text_at=text_length_at+4u;
  if((size_t)text_length>record_size-text_at) return 0;
  const uint8_t *text=record+text_at;
  size_t size=text_length,start=0;
  for(size_t i=0;i+5<=size;i++) if(text[i]=='\\' && !memcmp(text+i+1,"pard",4)){
    start=i; break;
  }
  uint32_t colors[32]={0}; int color_count=1;
  for(size_t i=0;i+4<size && color_count<(int)(sizeof(colors)/sizeof(colors[0]));i++){
    if(text[i]!='\\'||memcmp(text+i+1,"red",3)) continue;
    size_t at=i+4; int red=0,green=0,blue=0,have=0;
    while(at<size&&text[at]>='0'&&text[at]<='9'){ have=1; red=red*10+text[at++]-'0'; }
    if(!have||at+6>=size||memcmp(text+at,"\\green",6)) continue;
    at+=6; have=0;
    while(at<size&&text[at]>='0'&&text[at]<='9'){ have=1; green=green*10+text[at++]-'0'; }
    if(!have||at+5>=size||memcmp(text+at,"\\blue",5)) continue;
    at+=5; have=0;
    while(at<size&&text[at]>='0'&&text[at]<='9'){ have=1; blue=blue*10+text[at++]-'0'; }
    if(!have) continue;
    if(red>255) red=255;
    if(green>255) green=255;
    if(blue>255) blue=255;
    colors[color_count++]=(uint32_t)red|((uint32_t)green<<8)|((uint32_t)blue<<16);
    i=at;
  }
  ClassicInfoStyle style={24,0,0,0,0,0};
  ClassicInfoStyle stack[32]; int stack_count=0;
  ClassicInfoLine line; memset(&line,0,sizeof(line));
  classic_info_line_style(&line,&style);
  int count=0;
  for(size_t i=start;i<size && count<CLASSIC_INFO_MAX_LINES;){
    unsigned char ch=text[i++];
    if(ch=='\r'||ch=='\n') continue;
    if(ch=='{'){
      if(stack_count<(int)(sizeof(stack)/sizeof(stack[0]))) stack[stack_count++]=style;
      continue;
    }
    if(ch=='}'){
      if(stack_count>0) style=stack[--stack_count];
      classic_info_line_style(&line,&style);
      continue;
    }
    if(ch!='\\'){
      char literal=(char)ch;
      classic_info_append(&line,&style,&literal,1);
      continue;
    }
    if(i>=size) break;
    ch=text[i];
    if(ch=='\\'||ch=='{'||ch=='}'){
      i++; char literal=(char)ch;
      classic_info_append(&line,&style,&literal,1);
      continue;
    }
    if(ch=='\'' && i+2<size){
      int hi=text[i+1],lo=text[i+2];
      hi=(hi>='0'&&hi<='9')?hi-'0':((hi|32)>='a'&&(hi|32)<='f')?(hi|32)-'a'+10:-1;
      lo=(lo>='0'&&lo<='9')?lo-'0':((lo|32)>='a'&&(lo|32)<='f')?(lo|32)-'a'+10:-1;
      if(hi>=0&&lo>=0){ char literal=(char)((hi<<4)|lo); classic_info_append(&line,&style,&literal,1); }
      i+=3; continue;
    }
    if(ch=='~'||ch=='_'){
      i++; char literal=ch=='~'?' ':'-'; classic_info_append(&line,&style,&literal,1); continue;
    }
    if(ch=='-'||ch=='*'){ i++; continue; }
    char word[24]; int parameter=0,has_parameter=0;
    if(!classic_info_word(text,size,&i,word,sizeof(word),&parameter,&has_parameter)){
      i++; continue;
    }
    if(!strcmp(word,"par")||!strcmp(word,"line")){
      classic_info_finish_line(lines,&count,&line,&style);
    } else if(!strcmp(word,"fs") && has_parameter){
      if(parameter<8) parameter=8;
      if(parameter>144) parameter=144;
      style.font_size=parameter; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"b")){
      style.bold=!has_parameter||parameter!=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"i")){
      style.italic=!has_parameter||parameter!=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"ul")){
      style.underline=!has_parameter||parameter!=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"ulnone")){
      style.underline=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"qc")){
      style.align=1; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"qr")){
      style.align=2; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"ql")||!strcmp(word,"pard")){
      style.align=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"plain")){
      style.font_size=24; style.bold=style.italic=style.underline=0; style.color=0;
      classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"cf") && has_parameter){
      if(parameter>=0&&parameter<color_count) style.color=colors[parameter];
      classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"tab")){
      classic_info_append(&line,&style,"    ",4);
    } else if(!strcmp(word,"emdash")){
      classic_info_append(&line,&style,"--",2);
    } else if(!strcmp(word,"endash")){
      classic_info_append(&line,&style,"-",1);
    } else if(!strcmp(word,"bullet")){
      classic_info_append(&line,&style,"*",1);
    }
  }
  if(line.length && count<CLASSIC_INFO_MAX_LINES)
    classic_info_finish_line(lines,&count,&line,&style);
  *line_count=count;
  return count>0;
}

static int classic_info_font_build(GmlRender *r,int source_index){
  for(int i=0;i<r->classic_info_font_cache_count;i++){
    if(r->classic_info_font_cache[i].source_index==source_index)
      return r->classic_info_font_cache[i].font_id;
  }
  if(source_index<0||source_index>=GML_CLASSIC_INFO_FONT_SOURCE_COUNT||
     r->n_fonts>=GML_MAX_FONTS) return -1;
  const GmlClassicInfoFontSource *source=&gml_classic_info_font_sources[source_index];
  const int first=GML_CLASSIC_INFO_FONT_FIRST,last=GML_CLASSIC_INFO_FONT_LAST;
  const int glyph_count=last-first+1,atlas_width=512;
  int ax=0,ay=0,row_height=0;
  for(int i=0;i<glyph_count;i++){
    const GmlClassicInfoGlyph *glyph=&source->glyphs[i];
    if(ax+glyph->width>atlas_width){ ax=0; ay+=row_height; row_height=0; }
    if(glyph->height>row_height) row_height=glyph->height;
    ax+=glyph->width;
  }
  int atlas_height=ay+row_height;
  if(atlas_height<1) return -1;
  int power_of_two=64; while(power_of_two<atlas_height) power_of_two*=2;
  atlas_height=power_of_two;
  uint8_t *pixels=calloc((size_t)atlas_width*atlas_height,4);
  GmlGlyph *glyphs=calloc((size_t)glyph_count,sizeof(*glyphs));
  if(!pixels||!glyphs){ free(pixels); free(glyphs); return -1; }
  GmlAtlas *atlases=realloc(r->atlas,(size_t)(r->n_atlas+1)*sizeof(*atlases));
  if(!atlases){ free(pixels); free(glyphs); return -1; }
  r->atlas=atlases;
  int atlas_id=r->n_atlas++;
  GmlAtlas *atlas=&r->atlas[atlas_id];
  memset(atlas,0,sizeof(*atlas));
  atlas->w=atlas_width; atlas->h=atlas_height; atlas->px=pixels; atlas->decode_attempted=1;
  ax=0; ay=0; row_height=0;
  for(int i=0;i<glyph_count;i++){
    const GmlClassicInfoGlyph *source_glyph=&source->glyphs[i];
    int width=source_glyph->width,height=source_glyph->height;
    if(ax+width>atlas_width){ ax=0; ay+=row_height; row_height=0; }
    if(height>row_height) row_height=height;
    GmlGlyph *glyph=&glyphs[i];
    glyph->ch=(uint16_t)(first+i); glyph->sx=ax; glyph->sy=ay;
    glyph->w=width; glyph->h=height;
    glyph->shift=source_glyph->shift; glyph->offset=source_glyph->offset;
    const uint8_t *coverage=source->rgb_coverage+source_glyph->off;
    for(int y=0;y<height;y++) for(int x=0;x<width;x++){
      const uint8_t *sample=coverage+((size_t)y*width+x)*3;
      if(sample[0]||sample[1]||sample[2]){
        uint8_t *pixel=pixels+((size_t)(ay+y)*atlas_width+ax+x)*4;
        pixel[0]=(uint8_t)(255-sample[0]);
        pixel[1]=(uint8_t)(255-sample[1]);
        pixel[2]=(uint8_t)(255-sample[2]);
        pixel[3]=255;
      }
    }
    ax+=width;
  }
  int id=r->n_fonts++;
  GmlFont *font=&r->fonts[id];
  memset(font,0,sizeof(*font));
  for(int i=0;i<256;i++) font->glyph_by_char[i]=-1;
  font->real=1; font->sprite=-1; font->atlas=atlas_id; font->subpixel=1;
  font->line_height=source->line_height; font->align_height=source->line_height;
  font->glyphs=glyphs; font->n_glyphs=glyph_count; font->glyphs_sorted=1;
  for(int i=0;i<glyph_count;i++) font->glyph_by_char[first+i]=i;
  if(r->classic_info_font_cache_count<(int)(sizeof(r->classic_info_font_cache)/
                                             sizeof(r->classic_info_font_cache[0]))){
    typeof(r->classic_info_font_cache[0]) *cached=
      &r->classic_info_font_cache[r->classic_info_font_cache_count++];
    cached->source_index=source_index; cached->font_id=id;
  }
  return id;
}

static int classic_info_font(GmlRender *r,int half_points,int bold,int italic,
                             double *scale,int *source_index){
  int best=-1,best_distance=INT_MAX;
  for(int i=0;i<GML_CLASSIC_INFO_FONT_SOURCE_COUNT;i++){
    const GmlClassicInfoFontSource *source=&gml_classic_info_font_sources[i];
    if(source->bold!=!!bold||source->italic!=!!italic) continue;
    int distance=abs(source->half_points-half_points);
    if(distance<best_distance){ best=i; best_distance=distance; }
  }
  if(best<0) return -1;
  if(source_index) *source_index=best;
  if(scale) *scale=(double)half_points/gml_classic_info_font_sources[best].half_points;
  return classic_info_font_build(r,best);
}

typedef struct { int font_id; double scale; int line_height,y_offset; } ClassicInfoResolvedFont;

static ClassicInfoResolvedFont classic_info_resolve_font(GmlRender *r,int half_points,
                                                          int bold,int italic){
  ClassicInfoResolvedFont resolved={-1,1,0,0};
  int source_index=-1;
  resolved.font_id=classic_info_font(r,half_points,bold,italic,&resolved.scale,&source_index);
  if(resolved.font_id>=0){
    resolved.line_height=r->fonts[resolved.font_id].line_height;
    resolved.y_offset=gml_classic_info_font_sources[source_index].y_offset;
  }
  return resolved;
}

static ClassicInfoRun *classic_info_run_at(ClassicInfoLine *line,int position){
  for(int i=0;i<line->run_count;i++){
    ClassicInfoRun *run=&line->runs[i];
    if(position>=run->start&&position<run->start+run->length) return run;
  }
  return NULL;
}

static double classic_info_advance(GmlRender *r,ClassicInfoLine *line,int position){
  ClassicInfoRun *run=classic_info_run_at(line,position);
  if(!run) return 0;
  ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,run->font_size,run->bold,run->italic);
  if(resolved.font_id<0) return 0;
  GmlGlyph *glyph=real_glyph(&r->fonts[resolved.font_id],(unsigned char)line->text[position]);
  return glyph?glyph->shift*resolved.scale:0;
}

static double classic_info_range_width(GmlRender *r,ClassicInfoLine *line,int first,int last){
  double result=0;
  for(int position=first;position<last;position++)
    result+=classic_info_advance(r,line,position);
  return result;
}

static double classic_info_row_step(int half_points,int bold,int line_height){
  (void)bold;
  double step=ceil(half_points*(2.0/3.0)*1.06);
  if(line_height>step) step=line_height;
  return step<1?1:step;
}

static double classic_info_draw_row(GmlRender *r,ClassicInfoLine *line,int first,int last,
                                    double y,int page_width,uint32_t text_background){
  int maximum_height=0,maximum_half_points=line->font_size,step_bold=line->bold;
  for(int i=0;i<line->run_count;i++){
    ClassicInfoRun *run=&line->runs[i];
    int run_first=run->start,run_last=run->start+run->length;
    if(run_last<=first||run_first>=last) continue;
    ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,run->font_size,run->bold,run->italic);
    if(resolved.line_height>maximum_height) maximum_height=resolved.line_height;
    if(run->font_size>maximum_half_points){ maximum_half_points=run->font_size; step_bold=run->bold; }
  }
  if(maximum_height<=0){
    ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,line->font_size,line->bold,line->italic);
    maximum_height=resolved.line_height;
  }
  double row_width=classic_info_range_width(r,line,first,last);
  double pen=line->align==1?page_width*0.5-0.5-row_width*0.5:
             (line->align==2?page_width-3-row_width:3);
  int background_x0=(int)floor(pen+0.5);
  int background_x1=(int)ceil(pen+row_width)+1;
  int background_y0=(int)floor(y)-1;
  int background_y1=background_y0+maximum_height;
  if(background_x0<0) background_x0=0;
  if(background_y0<0) background_y0=0;
  if(background_x1>r->fbw) background_x1=r->fbw;
  if(background_y1>r->fbh) background_y1=r->fbh;
  for(int yy=background_y0;yy<background_y1;yy++){
    uint32_t *destination=r->fb+(size_t)yy*r->fbw+background_x0;
    for(int xx=background_x0;xx<background_x1;xx++)
      *destination++=0xFF000000u|text_background;
  }
  int position=first;
  while(position<last){
    ClassicInfoRun *run=classic_info_run_at(line,position);
    if(!run){ position++; continue; }
    int run_last=run->start+run->length; if(run_last>last) run_last=last;
    ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,run->font_size,run->bold,run->italic);
    if(resolved.font_id>=0){
      int length=run_last-position;
      char text[CLASSIC_INFO_LINE_BYTES];
      if(length>=(int)sizeof(text)) length=(int)sizeof(text)-1;
      memcpy(text,line->text+position,(size_t)length); text[length]='\0';
      r->font=resolved.font_id; r->halign=0;
      double run_y=y+(maximum_height-resolved.line_height)-(run->bold?1:0)+resolved.y_offset;
      gml_draw_text_transformed(r,pen,run_y,text,resolved.scale,resolved.scale,0,run->color,1);
      if(run->underline){
        double width=classic_info_range_width(r,line,position,run_last);
        int x0=(int)floor(pen+0.5),x1=(int)floor(pen+width+0.5);
        int underline_y=(int)floor(run_y+resolved.line_height*resolved.scale-2.0+0.5);
        if(x0<0) x0=0;
        if(x1>r->fbw) x1=r->fbw;
        if(underline_y>=0&&underline_y<r->fbh&&x1>x0){
          uint32_t rgb=0xFF000000u|((run->color&255u)<<16)|(run->color&0xFF00u)|
                       ((run->color>>16)&255u);
          uint32_t *row=r->fb+(size_t)underline_y*r->fbw;
          for(int x=x0;x<x1;x++) row[x]=rgb;
        }
      }
      r->font=-1;
    }
    pen+=classic_info_range_width(r,line,position,run_last);
    position=run_last;
  }
  return classic_info_row_step(maximum_half_points,step_bold,maximum_height);
}

static int classic_info_host_render(GmlRender *r,uint32_t *pixels,int width,int height,
                                    const uint8_t *record,size_t record_size){
  const AnygmHostServices *host=r&&r->win?r->win->host:NULL;
  if(!host||!host->rich_text_render||!pixels||width<=0||height<=0||
     !record||record_size<12u) return 0;
  uint32_t caption_size=u32(record,8);
  size_t text_size_at=12u+(size_t)caption_size+8u*4u+8u;
  if(text_size_at>record_size||record_size-text_size_at<4u) return 0;
  uint32_t text_size=u32(record,(uint32_t)text_size_at);
  if((size_t)text_size>record_size-text_size_at-4u) return 0;
  const uint8_t *text=record+text_size_at+4u;
  uint32_t raw_background=u32(record,0);
  uint32_t background=((raw_background&255u)<<16)|(raw_background&0xFF00u)|
                      ((raw_background>>16)&255u);
  return host->rich_text_render(host->userdata,text,text_size,background,pixels,
                                (uint32_t)width,(uint32_t)height,
                                (size_t)width*sizeof(*pixels))==ANYGM_OK;
}

static int classic_info_native_cached(GmlRender *r,uint32_t *framebuffer,
                                      int width,int height,const uint8_t *record,
                                      size_t record_size){
  if(r->classic_info_native_record!=record||
     r->classic_info_native_record_size!=record_size||
     r->classic_info_native_w!=width||r->classic_info_native_h!=height){
    free(r->classic_info_native_pixels);
    r->classic_info_native_pixels=NULL;
    r->classic_info_native_record=record;
    r->classic_info_native_record_size=record_size;
    r->classic_info_native_w=width; r->classic_info_native_h=height;
    r->classic_info_native_attempted=0;
  }
  if(!r->classic_info_native_attempted){
    r->classic_info_native_attempted=1;
    size_t count=(size_t)width*(size_t)height;
    if(width>0&&height>0&&count<=SIZE_MAX/sizeof(uint32_t)){
      r->classic_info_native_pixels=(uint32_t*)malloc(count*sizeof(uint32_t));
      if(r->classic_info_native_pixels&&
         !classic_info_host_render(r,r->classic_info_native_pixels,width,height,
                                   record,record_size)){
        free(r->classic_info_native_pixels);
        r->classic_info_native_pixels=NULL;
      }
    }
  }
  if(!r->classic_info_native_pixels) return 0;
  memcpy(framebuffer,r->classic_info_native_pixels,
         (size_t)width*(size_t)height*sizeof(uint32_t));
  return 1;
}

void gml_draw_classic_game_information(GmlRender *r,uint32_t *framebuffer,
                                       int width,int height,
                                       const uint8_t *record,size_t record_size){
  if(!r||!framebuffer||width<=0||height<=0||record_size>8u*1024u*1024u) return;
  if(classic_info_native_cached(r,framebuffer,width,height,record,record_size)){
    gml_render_begin(r,framebuffer,width,height,0,0);
    r->fb_opaque_known=1; r->fb_all_opaque=1; r->fb_all_transparent=0;
    return;
  }
  ClassicInfoLine lines[CLASSIC_INFO_MAX_LINES]; int line_count=0;
  if(!classic_info_parse(record,record_size,lines,&line_count)) return;
  uint32_t saved_color=r->color;
  double saved_alpha=r->alpha;
  int saved_halign=r->halign, saved_valign=r->valign, saved_font=r->font;
  int saved_alphablend=r->alphablend;
  int saved_software_overlay=r->software_overlay;
  uint32_t information_color=u32(record,0);
  uint32_t background=0xFFFFFFu;
  if((information_color&0xFF000000u)!=0xFF000000u)
    background=((information_color&255u)<<16)|(information_color&0xFF00u)|
               ((information_color>>16)&255u);
  size_t pixels=(size_t)width*(size_t)height;
  for(size_t i=0;i<pixels;i++) framebuffer[i]=background;
  uint32_t outer_border=0xABADB3u;
  for(int x=0;x<width;x++){
    framebuffer[x]=outer_border;
    framebuffer[(size_t)(height-1)*width+x]=outer_border;
  }
  for(int y=0;y<height;y++){
    framebuffer[(size_t)y*width]=outer_border;
    framebuffer[(size_t)y*width+width-1]=outer_border;
  }
  if(width>2&&height>2){
    for(int x=1;x<width-1;x++){
      framebuffer[(size_t)width+x]=0xFFFFFFu;
      framebuffer[(size_t)(height-2)*width+x]=0xFFFFFFu;
    }
    for(int y=1;y<height-1;y++){
      framebuffer[(size_t)y*width+1]=0xFFFFFFu;
      framebuffer[(size_t)y*width+width-2]=0xFFFFFFu;
    }
  }
  uint32_t text_background=background;
  unsigned background_green=(text_background>>8)&255u;
  if(background_green>0&&background_green<128)
    text_background=(text_background&~0xFF00u)|((background_green+1u)<<8);
  gml_render_begin(r,framebuffer,width,height,0,0);
  r->font=-1; r->color=0; r->alpha=1; r->valign=0; r->alphablend=1;
  r->software_overlay=1;
  r->fb_opaque_known=1; r->fb_all_opaque=1; r->fb_all_transparent=0;
  double y=4;
  for(int i=0;i<line_count && y<height;i++){
    ClassicInfoLine *line=&lines[i];
    if(!line->length){
      ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,line->font_size,line->bold,line->italic);
      double blank_step=classic_info_row_step(line->font_size,line->bold,resolved.line_height);
      /* A 9-point empty RichEdit paragraph retains its half-pixel leading;
       * keeping the fractional phase makes later lines snap like GDI. */
      if(line->font_size==18&&!line->bold&&!line->italic) blank_step+=0.5;
      y+=blank_step;
      continue;
    }
    int first=0;
    while(first<line->length&&y<height){
      int position=first,last_space=-1,last=line->length,next=line->length;
      double row_width=0,space_width=0,maximum_width=width>6?width-6:width;
      for(;position<line->length;position++){
        double advance=classic_info_advance(r,line,position);
        if(line->text[position]==' '){ last_space=position; space_width=row_width; }
        if(row_width+advance>maximum_width&&position>first){
          if(last_space>=first){
            last=last_space+1;
            row_width=space_width+classic_info_advance(r,line,last_space);
            next=last_space+1;
          }
          else { last=position; next=position; }
          while(next<line->length&&line->text[next]==' ') next++;
          break;
        }
        row_width+=advance;
      }
      (void)row_width;
      y+=classic_info_draw_row(r,line,first,last,y,width,text_background);
      first=next;
    }
  }
  r->color=saved_color; r->alpha=saved_alpha;
  r->halign=saved_halign; r->valign=saved_valign; r->font=saved_font;
  r->alphablend=saved_alphablend;
  r->software_overlay=saved_software_overlay;
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
        if(gml_d3_draw_sprite_2d(r,f->sprite,fr,glyph_x,glyph_y,xs,ys,rr,blend,alpha)){
          /* The D3 hook projects the glyph quad at the current draw depth. */
        } else if(s->runtime_rgba){
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
        } else if(s->frame){ int ti=s->frame[fr];
          if(ti>=0 && ti<r->n_tpag){ GmlTpag *t=&r->tpag[ti];
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
          } } }
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
