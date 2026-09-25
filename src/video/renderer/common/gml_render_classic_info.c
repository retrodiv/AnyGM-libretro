/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Classic information-page parsing, font preparation, and layout. */
#include "gml_render_internal.h"
#include "gml_render_text_internal.h"
#include "gml_classic_info_font_data.h"
#include "anygm_host.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
