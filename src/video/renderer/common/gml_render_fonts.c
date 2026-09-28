/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing FONT parsing, font lifecycle, glyph lookup and runtime raster atlas. */
#include "gml_render_internal.h"
#include "gml_render_text_internal.h"
#include "gml_font_raster.h"
#include "gml_hash.h"
#include "gml_default_font_data.h"
#include "gml_studio_default_font_data.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read per-glyph (right character, signed amount) kerning pairs. Supported records place
 * the pair count at one of two offsets. Select a layout only when every implied record stride
 * reaches the next glyph pointer exactly; otherwise leave kerning disabled. */
static void font_read_kerning(GmlRender *r, GmlFont *f, const uint8_t *d,
                              uint32_t table, uint32_t count){
  if(!r || !r->win || !f || !d || count<2) return;
  static const struct { uint32_t at, step; } layouts[2]={ {14u,16u}, {16u,18u} };
  int chosen=-1;
  for(int candidate=0; candidate<2 && chosen<0; candidate++){
    int holds=1;
    for(uint32_t g=0; g+1<count && holds; g++){
      uint32_t q=u32(d,table+g*4), next=u32(d,table+(g+1)*4);
      if((size_t)q+layouts[candidate].at+2u>r->win->size){ holds=0; break; }
      uint32_t pairs=u16(d,q+layouts[candidate].at);
      if(pairs>4096u || q+layouts[candidate].step+4u*pairs!=next) holds=0;
    }
    if(holds) chosen=candidate;
  }
  if(chosen<0) return;
  uint32_t total=0;
  for(uint32_t g=0; g+1<count; g++) total+=u16(d,u32(d,table+g*4)+layouts[chosen].at);
  if(!total || total>1000000u) return;
  GmlFontKern *pairs=(GmlFontKern*)calloc(total,sizeof(*pairs));
  if(!pairs) return;
  uint32_t written=0;
  for(uint32_t g=0; g+1<count && written<total; g++){
    uint32_t q=u32(d,table+g*4);
    uint32_t n=u16(d,q+layouts[chosen].at);
    uint16_t left=u16(d,q);
    for(uint32_t k=0;k<n && written<total;k++){
      uint32_t at=q+layouts[chosen].step+4u*k;
      if((size_t)at+4u>r->win->size) break;
      pairs[written].left=left;
      pairs[written].right=u16(d,at);
      pairs[written].amount=(int16_t)u16(d,at+2);
      written++;
    }
  }
  f->kerning=pairs; f->n_kerning=(int)written;
  /* Sort once so layout can find each pair by binary search. */
  for(int i=1;i<f->n_kerning;i++){
    GmlFontKern key=f->kerning[i]; int j=i-1;
    while(j>=0 && (f->kerning[j].left>key.left ||
                   (f->kerning[j].left==key.left && f->kerning[j].right>key.right))){
      f->kerning[j+1]=f->kerning[j]; j--;
    }
    f->kerning[j+1]=key;
  }
}
/* The pen advance from `left` to `right`, beyond the left glyph's own shift. */
int font_kerning(const GmlFont *f, unsigned left, unsigned right){
  if(!f || !f->kerning || f->n_kerning<=0 || left>0xFFFFu || right>0xFFFFu) return 0;
  int lo=0, hi=f->n_kerning-1;
  while(lo<=hi){
    int mid=lo+(hi-lo)/2;
    const GmlFontKern *k=&f->kerning[mid];
    if(k->left<left || (k->left==left && k->right<right)) lo=mid+1;
    else if(k->left==left && k->right==right) return k->amount;
    else hi=mid-1;
  }
  return 0;
}
/* FONT records reference a TPAG page and glyph rectangles with advance and bearing.
 * Parse these records from the loaded container. Resource fonts occupy the initial
 * font indices; dynamically added sprite fonts follow them. */
void parse_font(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"FONT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  int nf=(int)n; if(nf>GML_MAX_FONTS) nf=GML_MAX_FONTS;
  for(int i=0;i<nf;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlFont *f=&r->fonts[i];
    memset(f,0,sizeof(*f));
    for(int k=0;k<256;k++) f->glyph_by_char[k]=-1;
    f->real=1;
    f->sprite=-1;   /* real fonts have no sprite: keeps state records unambiguous vs sprite fonts */
    f->bold=u32(d,p+12)!=0;
    f->italic=u32(d,p+16)!=0;
    /* EmSize is u32 in bc14-16 and float, negated for point-sized fonts, in newer exports.
     * Reading the float bits as an integer shifts the glyph table and can produce zero glyphs. */
    int em_is_float=0;
    { uint32_t rawem=u32(d,p+8); float fem; memcpy(&fem,&rawem,4);
      if(fem<0){ f->line_height=(int)(0.5f-fem); em_is_float=1; }
      else if(fem>0 && fem<512.0f && rawem>0x30000000u){ f->line_height=(int)(fem+0.5f); em_is_float=1; }
      else f->line_height=(int)rawem; }
    uint32_t texptr=u32(d,p+28);                    /* glyph-page TPAG record */
    int tsx=u16(d,texptr), tsy=u16(d,texptr+2), tatlas=(int16_t)u16(d,texptr+20);
    f->atlas=tatlas;
    /* glyph table position: +40 (bc14-16), +44 (GMS2 compatibility exports with one field after
     * the scales), +48 (exports with AscenderOffset+Ascender after the scales), +52 (exports with
     * SDFSpread too), +56 (headroom for the next added field).
     * Detect per record: the count must be sane, followed by an in-file pointer list whose
     * first glyph has a plausible char code. First sane candidate wins; with no sane char
     * code anywhere, the first structurally plausible one does. Restricting detection to the
     * earlier offsets can otherwise produce an empty glyph table for a valid record. */
    uint32_t goff=40;
    { int first_ok=-1, best=-1;
      static const uint32_t cand[5]={40,44,48,52,56};
      for(int ci=0;ci<5 && best<0;ci++){ uint32_t co=cand[ci];
        uint32_t cnt=u32(d,p+co);
        if(cnt==0 || cnt>=100000) continue;
        uint32_t q=u32(d,p+co+4);
        if(!(q>p && q+14<=r->win->size)) continue;
        if(first_ok<0) first_ok=(int)co;
        if(u16(d,q)<=0x2FFF) best=(int)co;
      }
      if(best>=0) goff=(uint32_t)best;
      else if(first_ok>=0) goff=(uint32_t)first_ok;
    }
    /* Bytecode 17 added AscenderOffset immediately before the glyph array.  Older records put
     * the array at +40, while every later layout retains the offset at +40 and moves the array
     * farther right as more metrics are appended. */
    int serialized_line_height=0;
    if(goff>40){
      int32_t ascender=(int32_t)u32(d,p+40);
      if(ascender>-32768 && ascender<32768) f->ascender_offset=(int)ascender;
    }
    if(goff>=48){
      uint32_t ascender=u32(d,p+44);
      if(ascender<=32768) f->ascender=(int)ascender;
    }
    if(goff>=52){
      uint32_t spread=u32(d,p+48);
      if(spread<=4096) f->sdf_spread=(int)spread;
    }
    /* Newer FONT records serialize their actual line advance after AscenderOffset,
     * Ascender and SDFSpread.  The point/em size and the tallest packed glyph are not equivalent:
     * using either for vertical alignment moves centred labels by a logical pixel and spaces
     * multiline text incorrectly.  Layouts ending before +56 do not yet carry this field. */
    if(goff>=56){
      int32_t line_height=(int32_t)u32(d,p+52);
      if(line_height>0 && line_height<=4096) serialized_line_height=(int)line_height;
    }
    uint32_t gc=u32(d,p+goff);
    if(gc>100000) gc=0;                             /* guard */
    f->glyphs=calloc(gc?gc:1,sizeof(GmlGlyph));
    f->n_glyphs=(int)gc;
    f->glyphs_sorted=1;
    uint16_t previous_ch=0;
    for(uint32_t g=0;g<gc && f->glyphs;g++){
      uint32_t q=u32(d,p+goff+4+g*4);               /* pointer to glyph record */
      GmlGlyph *gl=&f->glyphs[g];
      gl->ch=u16(d,q);
      gl->sx=tsx+(int)u16(d,q+2); gl->sy=tsy+(int)u16(d,q+4);   /* absolute atlas coords */
      gl->w=(int)u16(d,q+6); gl->h=(int)u16(d,q+8);
      gl->shift=(int16_t)u16(d,q+10);               /* advance */
      gl->offset=(int16_t)u16(d,q+12);              /* left bearing */
      if(g && gl->ch<previous_ch) f->glyphs_sorted=0;
      previous_ch=gl->ch;
      if(gl->ch<256) f->glyph_by_char[gl->ch]=(int)g;
    }
    font_read_kerning(r,f,d,p+goff+4,gc);
    int mh=0;
    for(int g2=0;g2<f->n_glyphs;g2++) if(f->glyphs[g2].h>mh) mh=f->glyphs[g2].h;
    if(serialized_line_height){
      f->line_height=serialized_line_height;
      f->align_height=serialized_line_height;
    }else{
      f->align_height=mh>f->line_height?mh:f->line_height;
      /* Compact FONT records and early float-em records do not serialize a distinct line
       * advance. Their em size can be smaller than the packed glyph cell, so use the complete
       * cell height. Later records may carry an explicitly smaller authored advance and must
       * retain it. */
      if((goff==40 || em_is_float) && mh>f->line_height) f->line_height=mh;
    }
    if(render_setting(r,"GML_LOG_FONT"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] real id=%d name=%s line=%d align=%d maxglyph=%d ascender=%d ascender_offset=%d sdf_spread=%d atlas=%d glyphs=%d\n",
        i, gml_str_by_ptr(r->win,u32(d,p)), f->line_height, f->align_height,mh,
        f->ascender,f->ascender_offset,f->sdf_spread,f->atlas,f->n_glyphs);
    if(render_setting(r,"GML_LOG_FONT_GLYPHS"))
      for(int ch=32;ch<127;ch++){
        int gi=f->glyph_by_char[ch];
        if(gi>=0){ GmlGlyph *gl=&f->glyphs[gi];
          anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[fontglyph] font=%d ch=%d('%c') gi=%d rect=(%d,%d %dx%d) shift=%d off=%d\n",
                  i,ch,ch,gi,gl->sx,gl->sy,gl->w,gl->h,gl->shift,gl->offset); }
      }
  }
  if(r->n_fonts<nf) r->n_fonts=nf;                  /* sprite fonts number after real fonts */
}

/* The built-in default font remains available when FONT contains no user resources. Keep it
 * separate from the asset-id namespace: draw_set_font(-1) selects this data, while ids 0..n-1
 * continue to resolve only to fonts supplied by the loaded package. */
static int first_generation_default_advance(int ch,int fallback){
  /* First-generation glyph advances are independent of padded-cell width and fallback coverage.
   * Keep the layout metrics separate from the redistributable fallback raster. */
  switch(ch){
    case ' ': return 7;
    case '-': return 5;
    case ':': return 6;
    case '0': case '8': return 11;
    case '7': return 10;
    case 'A': case 'B': case 'D': case 'H': case 'O': return 11;
    case 'C': case 'E': case 'G': case 'L': case 'R': case 'S':
    case 'T': case 'Y': case 'Z': return 10;
    case 'I': return 5;
    case 'M': case 'N': return 12;
    default: return fallback;
  }
}

int build_default_font(GmlRender *r){
  enum { AW=512, AH=128 };
  /* Select the built-in font from the runtime generation, not the room-resource layout.
   * A revision-15 export can have second-generation layers and first-generation font policy. */
  const int studio=r->win&&!anygm_policy_uses_first_generation_studio(r->win)&&
    anygm_policy_has_modern_layer_semantics(r->win);
  const int first_generation=r->win&&anygm_policy_uses_first_generation_studio(r->win);
  const int source_first=studio?GML_STUDIO_DEFAULT_FONT_FIRST:GML_DEFAULT_FONT_FIRST;
  const int source_last=studio?GML_STUDIO_DEFAULT_FONT_LAST:GML_DEFAULT_FONT_LAST;
  const int ng=source_last-source_first+1;
  const GmlDefaultGlyph *source_glyphs=studio?
    gml_studio_default_glyphs:gml_default_glyphs;
  const uint8_t *source_alpha=studio?
    gml_studio_default_font_alpha:gml_default_font_alpha;
  GmlGlyph *glyphs=calloc((size_t)ng,sizeof(*glyphs));
  uint8_t *px=calloc((size_t)AW*AH,4);
  if(!glyphs || !px){ free(glyphs); free(px); return 0; }
  GmlAtlas *na=realloc(r->atlas,(size_t)(r->n_atlas+1)*sizeof(*na));
  if(!na){ free(glyphs); free(px); return 0; }
  r->atlas=na;
  int atlas_id=r->n_atlas++;
  GmlAtlas *a=&r->atlas[atlas_id];
  memset(a,0,sizeof(*a));
  a->px=px; a->w=AW; a->h=AH; a->decode_attempted=1;

  GmlFont *f=&r->default_font;
  memset(f,0,sizeof(*f));
  for(int i=0;i<256;i++) f->glyph_by_char[i]=-1;
  f->real=1; f->sprite=-1; f->atlas=atlas_id;
  f->line_height=studio?GML_STUDIO_DEFAULT_FONT_LINE_HEIGHT:
    (first_generation?15:GML_DEFAULT_FONT_LINE_HEIGHT);
  f->align_height=f->line_height;
  f->glyphs=glyphs; f->n_glyphs=ng; f->glyphs_sorted=1;
  int ax=0, ay=0, row_height=0;
  for(int i=0;i<ng;i++){
    const GmlDefaultGlyph *src=&source_glyphs[i];
    int w=src->width, h=src->height;
    if(ax+w>AW){ ax=0; ay+=row_height; row_height=0; }
    if(h>row_height) row_height=h;
    if(ay+h>AH) break;
    GmlGlyph *g=&glyphs[i];
    g->ch=(uint16_t)(source_first+i);
    g->sx=ax; g->sy=ay; g->w=w; g->h=h;
    g->shift=src->shift; g->offset=src->offset;
    if(first_generation){
      g->shift=first_generation_default_advance(g->ch,g->shift);
    }
    f->glyph_by_char[g->ch]=i;
    const uint8_t *cov=source_alpha+src->off;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
      uint8_t alpha=cov[y*w+x];
      if(alpha){
        uint8_t *q=px+((size_t)(ay+y)*AW+ax+x)*4;
        q[0]=q[1]=q[2]=255;
        q[3]=alpha;
      }
    }
    ax+=w;
  }
  if(render_setting(r,"GML_LOG_FONT"))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] built-in atlas=%d glyphs=%d line=%d\n",atlas_id,ng,f->line_height);
  return 1;
}



unsigned utf8_next(const char **pp);
static unsigned glyph_code_from_cp(unsigned cp);
int glyph_frame(GmlFont *f, unsigned cp);
static void font_build_fast(GmlFont *f);

static int font_find_matching_sprite(GmlRender *r, int sprite, int first, int prop, int sep){
  if(!r) return -1;
  for(int i=r->n_fonts-1;i>=0;i--){
    GmlFont *font=&r->fonts[i];
    if(!font->real && font->sprite==sprite && font->first==first && font->prop==prop &&
       font->sep==sep && !font->map && font->map_len==0) return i;
  }
  return -1;
}

static int font_sprite_map_matches(GmlFont *font, const char *map){
  if(!font || !map || !font->map || font->map_len<=0) return 0;
  const char *cursor=map;
  for(int i=0;i<font->map_len;i++){
    if(!*cursor || font->map[i]!=utf8_next(&cursor)) return 0;
  }
  return !*cursor;
}

static int font_find_matching_sprite_ext(GmlRender *r, int sprite, const char *map,
                                         int prop, int sep){
  if(!r) return -1;
  for(int i=r->n_fonts-1;i>=0;i--){
    GmlFont *font=&r->fonts[i];
    if(!font->real && font->sprite==sprite && font->prop==prop && font->sep==sep &&
       font_sprite_map_matches(font,map)) return i;
  }
  return -1;
}

int gml_font_add_sprite(GmlRender *r, int sprite, int first, int prop, int sep){
  if(!r || sprite<0 || sprite>=r->n_spr || r->spr[sprite].n_frames<=0) return -1;
  /* Preserve fresh resource ids while capacity remains. At the bounded pool limit, an exact
   * sprite-font request can safely retain its rendering semantics by reusing the newest equal
   * record instead of changing draw_set_font to the built-in fallback. */
  if(r->n_fonts>=GML_MAX_FONTS)
    return font_find_matching_sprite(r,sprite,first,prop,sep);
  int id=r->n_fonts++;
  r->fonts[id]=(GmlFont){.sprite=sprite,.first=first,.prop=prop,.sep=sep};
  if(render_setting(r,"GML_LOG_TEXT")){ GmlSprite *s=&r->spr[sprite];
    int spfr=(int)' '-first; int spw=-1;
    if(spfr>=0&&spfr<s->n_frames){
      if(s->runtime_rgba) spw=s->w;
      else if(s->frame){ int ti=s->frame[spfr]; if(ti>=0&&ti<r->n_tpag) spw=r->tpag[ti].sw; }
    }
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] id=%d spr=%d(%s) first=%d prop=%d sep=%d nframes=%d cell=%dx%d space_sw=%d\n",
      id,sprite,s->name?s->name:"?",first,prop,sep,s->n_frames,s->w,s->h,spw); }
  return id;
}
int gml_font_add_sprite_ext(GmlRender *r, int sprite, const char *map, int prop, int sep){
  if(!r || sprite<0 || sprite>=r->n_spr || r->spr[sprite].n_frames<=0) return -1;
  if(!map || !*map) return gml_font_add_sprite(r,sprite,0,prop,sep);
  if(r->n_fonts>=GML_MAX_FONTS)
    return font_find_matching_sprite_ext(r,sprite,map,prop,sep);
  int cap=64, len=0;
  uint32_t *cp=malloc((size_t)cap*sizeof(uint32_t));
  if(!cp) return -1;
  const char *p=map;
  while(*p){
    if(len>=cap){
      int nc=cap*2;
      uint32_t *np=realloc(cp,(size_t)nc*sizeof(uint32_t));
      if(!np){ free(cp); return -1; }
      cp=np; cap=nc;
    }
    cp[len++]=utf8_next(&p);
  }
  int id=r->n_fonts++;
  r->fonts[id]=(GmlFont){.sprite=sprite,.first=0,.prop=prop,.sep=sep,.map=cp,.map_len=len};
  font_build_fast(&r->fonts[id]);
  if(render_setting(r,"GML_LOG_TEXT")){ GmlSprite *s=&r->spr[sprite];
    /* Include a bounded map preview as well as its length. */
    char preview[97]; int at=0;
    for(int i=0;i<len && at<(int)sizeof(preview)-5;i++){
      unsigned c=cp?cp[i]:0;
      if(c>=32 && c<127) preview[at++]=(char)c;
      else at+=snprintf(preview+at,sizeof(preview)-(size_t)at,"<%u>",c);
    }
    preview[at<(int)sizeof(preview)?at:(int)sizeof(preview)-1]=0;
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] id=%d spr=%d(%s) map_len=%d prop=%d sep=%d nframes=%d cell=%dx%d map=\"%s\"\n",
      id,sprite,s->name?s->name:"?",len,prop,sep,s->n_frames,s->w,s->h,preview); }
  return id;
}

/* font_add(file, size): rasterize a TTF at runtime (GM loads fonts from the game dir — ports
 * ship localized fonts as loose .ttf files). Glyphs are baked as white+alpha cells into a new
 * atlas page, each cell spanning the FULL line height with the ink placed at ascent+bearing —
 * the same "ascent whitespace baked into the glyph" convention as data.win FONT glyphs, so
 * draw_text_real needs no changes. `size` is the raster pixel height; applying
 * a second 96/72 screen-DPI conversion makes loose fonts one third too large. */
static int font_add_file_checked(GmlRender *r,const char *path,double point_size,
                                 int bold,int italic,int first,int last,
                                 const uint8_t *expected_sha256){
  if(!r || !path || !path[0] || strlen(path)>4095 || !isfinite(point_size) ||
     r->n_fonts<0 || r->n_fonts>=GML_MAX_FONTS) return -1;
  if(first<0) first=0;
  if(last>0xFFFF) last=0xFFFF;
  if(last<first) return -1;
  uint8_t *ttf=NULL;
  size_t font_size=0;
  if(!r->win || !anygm_vfs_read_all(r->win->host,path,&ttf,&font_size,32u*1024u*1024u) ||
     font_size==0 || font_size>INT_MAX){ free(ttf); return -1; }
  uint8_t digest[32];
  gml_sha256(ttf,font_size,digest);
  if(expected_sha256 && memcmp(digest,expected_sha256,sizeof digest)){
    free(ttf);
    return -1;
  }
  GmlFontRasterFace *face=NULL;
  if(!gml_font_raster_face_open(ttf,font_size,NULL,&face)){
    free(ttf);
    return -1;
  }
  free(ttf);
  int px=point_size<=4?4:point_size>=256?256:(int)lround(point_size);
  /* A negative-height Win32 font request maps the requested size to the font's em square.
   * ScaleForPixelHeight instead maps it to ascent-descent; the two happen to agree for many
   * Latin fonts but undersize fonts whose external leading extends beyond the em. */
  float scale=gml_font_raster_scale_for_em(face,(float)px);
  GmlFontRasterMetrics font_metrics;
  if(!(scale>0.0f) || !gml_font_raster_metrics(face,&font_metrics)){
    gml_font_raster_face_close(face);
    return -1;
  }
  int ascent=(int)lround(font_metrics.ascent*scale);
  int lh=(int)lround((font_metrics.ascent-font_metrics.descent+
                      font_metrics.line_gap)*scale);
  if(lh<=0) lh=px;
  /* The last two font_add arguments are a Unicode codepoint range.  Keep only characters the
   * font actually maps: reserving a cell for every hole in a broad CJK range can turn a small
   * font into a several-hundred-megabyte atlas and is outside the sparse glyph-set contract. */
  int requested=last-first+1, ncp=0;
  uint32_t *cps=malloc((size_t)requested*sizeof(*cps));
  if(!cps){ gml_font_raster_face_close(face); return -1; }
  for(int cp=first;cp<=last;cp++)
    if(gml_font_raster_has_glyph(face,cp)) cps[ncp++]=(uint32_t)cp;
  if(ncp<=0){ free(cps); gml_font_raster_face_close(face); return -1; }

  /* First pass: measure cells (cell width = ceil(advance), height = line height). */
  int *cw=malloc((size_t)ncp*sizeof(int));
  if(!cw){ free(cps); gml_font_raster_face_close(face); return -1; }
  int aw=512, ax=0, ay=0, rowh=lh+1, widest=1;
  for(int i=0;i<ncp;i++){
    int cp=(int)cps[i];
    GmlFontRasterGlyphMetrics metrics;
    if(!gml_font_raster_glyph_metrics(face,cp,scale,&metrics)){
      free(cw); free(cps); gml_font_raster_face_close(face); return -1;
    }
    int w=(int)ceilf(metrics.advance*scale);
    if(metrics.x1>w) w=metrics.x1;
    if(w<1) w=1;
    cw[i]=w+1;
    if(cw[i]>widest) widest=cw[i];
  }
  while(aw<widest && aw<4096) aw*=2;
  if(widest>aw){
    free(cw); free(cps); gml_font_raster_face_close(face); return -1;
  }
  for(int i=0;i<ncp;i++){
    if(ax+cw[i]>aw){ ax=0; ay+=rowh; }
    ax+=cw[i];
  }
  int ah=ay+rowh;
  size_t atlas_pixels=(size_t)aw*(size_t)ah;
  enum { MAX_RUNTIME_FONT_ATLAS_BYTES=128*1024*1024 };
  if(atlas_pixels>(size_t)MAX_RUNTIME_FONT_ATLAS_BYTES/4){
    free(cw); free(cps); gml_font_raster_face_close(face); return -1;
  }
  /* new atlas page */
  int atlas_id=-1;
  for(int i=0;i<r->n_atlas;i++)
    if(r->atlas[i].runtime_font_page && !r->atlas[i].px){ atlas_id=i; break; }
  int appended=atlas_id<0;
  if(appended){
    GmlAtlas *na=realloc(r->atlas,(size_t)(r->n_atlas+1)*sizeof(GmlAtlas));
    if(!na){
      free(cw); free(cps); gml_font_raster_face_close(face); return -1;
    }
    r->atlas=na;
    atlas_id=r->n_atlas;
  }
  GmlAtlas *A=&r->atlas[atlas_id];
  memset(A,0,sizeof(*A));
  A->runtime_font_page=1;
  A->w=aw; A->h=ah;
  A->px=calloc(atlas_pixels,4);
  if(!A->px){
    free(cw); free(cps); gml_font_raster_face_close(face); return -1;
  }
  A->decode_attempted=1;
  if(appended) r->n_atlas++;
  /* second pass: rasterize each glyph into its cell */
  GmlGlyph *glyphs=calloc((size_t)ncp,sizeof(GmlGlyph));
  if(!glyphs){
    free(A->px); memset(A,0,sizeof(*A)); A->runtime_font_page=1;
    if(appended) r->n_atlas--;
    free(cw); free(cps); gml_font_raster_face_close(face); return -1;
  }
  ax=0; ay=0;
  int ng=0;
  for(int i=0;i<ncp;i++){
    if(ax+cw[i]>aw){ ax=0; ay+=rowh; }
    int cp=(int)cps[i];
    GmlFontRasterGlyphMetrics metrics;
    if(!gml_font_raster_glyph_metrics(face,cp,scale,&metrics)){
      free(glyphs);
      free(A->px); memset(A,0,sizeof(*A)); A->runtime_font_page=1;
      if(appended) r->n_atlas--;
      free(cw); free(cps); gml_font_raster_face_close(face); return -1;
    }
    int gw=metrics.x1-metrics.x0, gh=metrics.y1-metrics.y0;
    if(gw>0 && gh>0){
      unsigned char *bmp=malloc((size_t)gw*gh);
      if(bmp){
        (void)gml_font_raster_render_glyph(face,cp,scale,bmp,(size_t)gw*gh,
                                            gw,gh,gw);
        int ox=metrics.x0<0?0:metrics.x0;
        int oy=ascent+metrics.y0;
        if(oy<0) oy=0;
        for(int yy=0;yy<gh;yy++){
          int py=ay+oy+yy; if(py>=ah) break;
          for(int xx=0;xx<gw;xx++){
            int pxx=ax+ox+xx; if(pxx>=aw) continue;
            uint8_t a8=bmp[yy*gw+xx];
            if(a8){ uint8_t *q=A->px+((size_t)py*aw+pxx)*4; q[0]=255;q[1]=255;q[2]=255;q[3]=a8; }
          }
        }
        free(bmp);
      }
    }
    GmlGlyph *gl=&glyphs[ng];
    gl->ch=(uint16_t)cp;
    gl->sx=ax; gl->sy=ay; gl->w=cw[i]-1; gl->h=lh;
    gl->shift=(int16_t)lround(metrics.advance*scale); gl->offset=0;
    ng++;
    ax+=cw[i];
  }
  free(cw); free(cps);
  int id=r->n_fonts++;
  GmlFont *f=&r->fonts[id];
  memset(f,0,sizeof(*f));
  for(int k=0;k<256;k++) f->glyph_by_char[k]=-1;
  f->real=1; f->atlas=atlas_id; f->line_height=lh; f->align_height=lh; f->runtime_owned=1;
  f->sprite=-1;
  /* Retain the request; selecting or synthesizing a different face is not a metadata query. */
  f->bold=bold!=0; f->italic=italic!=0;
  f->glyphs=glyphs; f->n_glyphs=ng; f->glyphs_sorted=1;
  /* The face stays open for on-demand glyphs outside the requested range. */
  f->runtime_face=face; f->runtime_scale=scale; f->runtime_ascent=ascent;
  f->runtime_pen_x=ax; f->runtime_pen_y=ay; f->runtime_row_h=rowh;
  f->runtime_glyph_cap=ng;
  f->runtime_source_path=strdup(path);
  memcpy(f->runtime_source_sha256,digest,sizeof digest);
  f->runtime_pixel_height=px; f->runtime_first=first; f->runtime_last=last;
  if(!f->runtime_source_path){ gml_font_delete(r,id); r->n_fonts--; return -1; }
  for(int g=0;g<ng;g++) if(glyphs[g].ch<256) f->glyph_by_char[glyphs[g].ch]=g;
  if(render_setting(r,"GML_LOG_FONT"))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] ttf id=%d path=%s pt=%.1f px=%d lh=%d range=%d-%d atlas=%d(%dx%d) glyphs=%d\n",
      id,path,point_size,px,lh,first,last,atlas_id,aw,ah,ng);
  return id;
}

int gml_font_add_file(GmlRender *r,const char *path,double point_size,
                       int bold,int italic,int first,int last){
  return font_add_file_checked(r,path,point_size,bold,italic,first,last,NULL);
}

void gml_font_delete(GmlRender *r, int font){
  if(!r || font<0 || font>=r->n_fonts) return;
  GmlFont *f=&r->fonts[font];
  /* Embedded FONT resources belong to the package and may share its texture pages. Runtime
   * sprite fonts own only their mapping, while runtime TTF fonts own both glyph metadata and
   * the private atlas page allocated by font_add. */
  if(f->real && !f->runtime_owned) return;
  if(f->runtime_owned && f->atlas>=0 && f->atlas<r->n_atlas){
    GmlAtlas *atlas=&r->atlas[f->atlas];
    gml_render_atlas_scene_retire_one(r,f->atlas);
    free(atlas->px);
    memset(atlas,0,sizeof(*atlas));
    atlas->runtime_font_page=1;
  }
  if(f->runtime_face) gml_font_raster_face_close(f->runtime_face);
  free(f->map); free(f->glyphs); free(f->kerning);
  free(f->runtime_source_path);
  memset(f,0,sizeof(*f));
  f->sprite=-1; f->atlas=-1;
  for(int i=0;i<256;i++) f->glyph_by_char[i]=-1;
}

/* glyph advance width for char index within a sprite font. Characters outside the font's
 * glyph range (notably the space, when `first` > 32) still advance the cursor: GM lays them
 * out at the fixed cell width (these sprite fonts are non-proportional), so a space is a blank
 * cell, not zero-width. */
int glyph_w(GmlRender *r, GmlFont *f, int frame, unsigned cp){
  GmlSprite *s=&r->spr[f->sprite];
  if(frame<0||frame>=s->n_frames){
    (void)cp;
    return s->w;        /* out-of-range: blank cell */
  }
  if(s->runtime_rgba) return s->w;
  int ti=s->frame[frame]; if(ti<0||ti>=r->n_tpag) return s->w;
  return f->prop ? r->tpag[ti].sw : (s->w?s->w:r->tpag[ti].sw);
}

unsigned utf8_next(const char **pp){
  const unsigned char *p=(const unsigned char*)*pp;
  if(!*p) return 0;
  if(p[0]<0x80){ *pp=(const char*)p+1; return p[0]; }
  if((p[0]&0xE0)==0xC0 && (p[1]&0xC0)==0x80){
    unsigned cp=((unsigned)(p[0]&0x1F)<<6)|(p[1]&0x3F);
    *pp=(const char*)p+2; return cp;
  }
  if((p[0]&0xF0)==0xE0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80){
    unsigned cp=((unsigned)(p[0]&0x0F)<<12)|((unsigned)(p[1]&0x3F)<<6)|(p[2]&0x3F);
    *pp=(const char*)p+3; return cp;
  }
  if((p[0]&0xF8)==0xF0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80){
    unsigned cp=((unsigned)(p[0]&0x07)<<18)|((unsigned)(p[1]&0x3F)<<12)|((unsigned)(p[2]&0x3F)<<6)|(p[3]&0x3F);
    *pp=(const char*)p+4; return cp;
  }
  *pp=(const char*)p+1;
  return p[0];
}
static unsigned glyph_code_from_cp(unsigned cp){
  switch(cp){
    case 0x2026: return 0x85; /* ellipsis in the GM/Windows-1252 sprite-font range */
    case 0x2018: return 0x91;
    case 0x2019: return 0x92;
    case 0x201C: return 0x93;
    case 0x201D: return 0x94;
    case 0x2013: return 0x96;
    case 0x2014: return 0x97;
    default: return cp;
  }
}
static void font_build_fast(GmlFont *f){
  if(!f) return;
  for(int i=0;i<256;i++) f->map_fast[i]=-1;
  if(!f->map || f->map_len<=0) return;
  for(int i=0;i<f->map_len;i++){
    unsigned gc=glyph_code_from_cp(f->map[i]);
    if(gc<256 && f->map_fast[gc]<0) f->map_fast[gc]=i;
    if(f->map[i]<256 && f->map_fast[f->map[i]]<0) f->map_fast[f->map[i]]=i;
  }
}
void gml_render_rebuild_font_maps(GmlRender *r){
  if(!r) return;
  for(int i=0;i<GML_MAX_FONTS;i++) font_build_fast(&r->fonts[i]);
}
int glyph_frame(GmlFont *f, unsigned cp){
  if(f->map && f->map_len>0){
    unsigned want=glyph_code_from_cp(cp);
    if(want<256 && f->map_fast[want]>=0) return f->map_fast[want];
    for(int i=0;i<f->map_len;i++)
      if(f->map[i]==cp || glyph_code_from_cp(f->map[i])==want) return i;
    return -1;
  }
  unsigned gc=glyph_code_from_cp(cp);
  return gc<=0x7fffffffU ? (int)gc - f->first : -1;
}

/* ---- real FONT-chunk font helpers ---- */
GmlGlyph *real_glyph(GmlFont *f, unsigned cp){
  if(cp<256){ int gi=f->glyph_by_char[cp]; return gi>=0? &f->glyphs[gi] : NULL; }
  if(f->glyphs_sorted){
    int lo=0, hi=f->n_glyphs;
    while(lo<hi){
      int mid=lo+(hi-lo)/2;
      unsigned ch=f->glyphs[mid].ch;
      if(ch<cp) lo=mid+1; else hi=mid;
    }
    return lo<f->n_glyphs && f->glyphs[lo].ch==cp ? &f->glyphs[lo] : NULL;
  }
  for(int i=0;i<f->n_glyphs;i++) if(f->glyphs[i].ch==cp) return &f->glyphs[i];
  return NULL;
}
/* Rasterize one missing glyph into a runtime TTF font's private atlas. The pen continues from
 * where font_add stopped; the atlas grows downward under the same 128 MiB ceiling font_add
 * enforces, and a failure simply draws nothing for that glyph. */
static GmlGlyph *font_runtime_rasterize(GmlRender *r, GmlFont *f, unsigned cp){
  if(!f->runtime_face || cp>0xFFFFu || f->atlas<0 || f->atlas>=r->n_atlas) return NULL;
  if(!gml_font_raster_has_glyph(f->runtime_face,(int)cp)) return NULL;
  GmlFontRasterGlyphMetrics metrics;
  if(!gml_font_raster_glyph_metrics(f->runtime_face,(int)cp,f->runtime_scale,&metrics)) return NULL;
  GmlAtlas *A=&r->atlas[f->atlas];
  if(!A->px || A->w<=0 || A->scene_source_version==UINT64_MAX) return NULL;
  int page_grew=0;
  int w=(int)ceilf(metrics.advance*f->runtime_scale);
  if(metrics.x1>w) w=metrics.x1;
  if(w<1) w=1;
  int cell=w+1;
  if(cell>A->w) return NULL;
  if(f->runtime_pen_x+cell>A->w){ f->runtime_pen_x=0; f->runtime_pen_y+=f->runtime_row_h; }
  int needed_h=f->runtime_pen_y+f->runtime_row_h;
  if(needed_h>A->h){
    enum { MAX_RUNTIME_FONT_ATLAS_BYTES=128*1024*1024 };
    int grown_h=A->h*2; if(grown_h<needed_h) grown_h=needed_h;
    if((size_t)A->w*(size_t)grown_h>(size_t)MAX_RUNTIME_FONT_ATLAS_BYTES/4) return NULL;
    uint8_t *grown=(uint8_t*)realloc(A->px,(size_t)A->w*(size_t)grown_h*4u);
    if(!grown) return NULL;
    memset(grown+(size_t)A->w*(size_t)A->h*4u,0,
           (size_t)A->w*(size_t)(grown_h-A->h)*4u);
    A->px=grown; A->h=grown_h;
    page_grew=1;
    /* Growth changes both the byte span and borrowed pointer even if a later
     * glyph-record allocation fails, so publish its version immediately. */
    if(A->scene_source_version) A->scene_source_version++;
  }
  if(f->n_glyphs==f->runtime_glyph_cap){
    int cap=f->runtime_glyph_cap?f->runtime_glyph_cap*2:16;
    GmlGlyph *glyphs=(GmlGlyph*)realloc(f->glyphs,(size_t)cap*sizeof(*glyphs));
    if(!glyphs) return NULL;
    f->glyphs=glyphs; f->runtime_glyph_cap=cap;
  }
  int gw=metrics.x1-metrics.x0, gh=metrics.y1-metrics.y0;
  if(gw>0 && gh>0){
    unsigned char *bmp=(unsigned char*)malloc((size_t)gw*gh);
    if(bmp){
      (void)gml_font_raster_render_glyph(f->runtime_face,(int)cp,f->runtime_scale,
                                         bmp,(size_t)gw*gh,gw,gh,gw);
      int ox=metrics.x0<0?0:metrics.x0;
      int oy=f->runtime_ascent+metrics.y0;
      if(oy<0) oy=0;
      for(int yy=0;yy<gh;yy++){
        int py=f->runtime_pen_y+oy+yy; if(py>=A->h) break;
        for(int xx=0;xx<gw;xx++){
          int pxx=f->runtime_pen_x+ox+xx; if(pxx>=A->w) continue;
          uint8_t a8=bmp[yy*gw+xx];
          if(a8){ uint8_t *q=A->px+((size_t)py*A->w+pxx)*4; q[0]=255;q[1]=255;q[2]=255;q[3]=a8; }
        }
      }
      free(bmp);
    }
  }
  GmlGlyph *gl=&f->glyphs[f->n_glyphs++];
  gl->ch=(uint16_t)cp;
  gl->sx=f->runtime_pen_x; gl->sy=f->runtime_pen_y;
  gl->w=cell-1; gl->h=f->line_height;
  gl->shift=(int16_t)lround(metrics.advance*f->runtime_scale); gl->offset=0;
  f->glyphs_sorted=0;
  if(cp<256) f->glyph_by_char[cp]=f->n_glyphs-1;
  f->runtime_pen_x+=cell;
  if(A->scene_source_version && !page_grew) A->scene_source_version++;
  return gl;
}
GmlGlyph *real_glyph_demand(GmlRender *r, GmlFont *f, unsigned cp){
  GmlGlyph *g=real_glyph(f,cp);
  if(g || !f->runtime_face) return g;
  return font_runtime_rasterize(r,f,cp);
}

int gml_render_restore_runtime_font(GmlRender *r,int id,const char *path,
                                    const uint8_t sha256[32],int pixel_height,
                                    int bold,int italic,int first,int last,
                                    const uint32_t *characters,int count){
  if(!r || id<0 || id>=GML_MAX_FONTS || !path || !sha256 || !characters ||
     pixel_height<4 || pixel_height>256 || first<0 || last>65535 || last<first ||
     bold<0 || bold>1 || italic<0 || italic>1 || count<1 || count>65536) return 0;
  GmlFont *live=&r->fonts[id];
  if(live->real && !live->runtime_owned) return 0;
  uint8_t seen[8192]={0};
  for(int i=0;i<count;i++){
    unsigned cp=characters[i];
    if(cp>65535 || (seen[cp>>3]&(1u<<(cp&7)))) return 0;
    seen[cp>>3]|=(uint8_t)(1u<<(cp&7));
  }
  int matching=live->runtime_owned && live->runtime_source_path &&
    !strcmp(live->runtime_source_path,path) && live->runtime_face &&
    !memcmp(live->runtime_source_sha256,sha256,32) &&
    live->runtime_pixel_height==pixel_height && live->runtime_first==first &&
    live->bold==bold && live->italic==italic &&
    live->runtime_last==last && live->n_glyphs==count && live->glyphs;
  for(int i=0;matching && i<count;i++) matching=live->glyphs[i].ch==characters[i];
  if(matching) return 1;

  /* Build without changing the live resource. The same checked file read supplies both the
   * identity and the existing rasterizer, so a second open cannot race the digest check. */
  GmlRender *staged=calloc(1,sizeof(*staged));
  if(!staged) return 0;
  staged->win=r->win;
  int built=font_add_file_checked(staged,path,pixel_height,bold,italic,first,last,sha256);
  int ok=built==0;
  GmlFont *font=&staged->fonts[0];
  int initial=font->n_glyphs;
  if(initial>count) ok=0;
  for(int i=0;ok && i<initial;i++) ok=font->glyphs[i].ch==characters[i];
  for(int i=initial;ok && i<count;i++)
    ok=real_glyph_demand(staged,font,characters[i])!=NULL;
  if(ok && font->n_glyphs!=count) ok=0;
  int target_atlas=-1;
  if(ok && live->runtime_owned && live->atlas>=0 && live->atlas<r->n_atlas)
    target_atlas=live->atlas;
  for(int i=0;ok && target_atlas<0 && i<r->n_atlas;i++)
    if(r->atlas[i].runtime_font_page && !r->atlas[i].px) target_atlas=i;
  if(ok && target_atlas<0){
    GmlAtlas *pages=realloc(r->atlas,(size_t)(r->n_atlas+1)*sizeof(*pages));
    if(!pages) ok=0;
    else {
      r->atlas=pages;
      target_atlas=r->n_atlas++;
      memset(&r->atlas[target_atlas],0,sizeof(*pages));
    }
  }
  if(ok){
    if(r->n_fonts<=id) r->n_fonts=id+1;
    gml_font_delete(r,id);
    r->atlas[target_atlas]=staged->atlas[font->atlas];
    memset(&staged->atlas[font->atlas],0,sizeof(*staged->atlas));
    r->fonts[id]=*font;
    r->fonts[id].atlas=target_atlas;
    memset(font,0,sizeof(*font));
  }
  gml_render_free(staged);
  free(staged);
  return ok;
}
