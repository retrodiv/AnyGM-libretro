/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Runtime fonts, glyph layout, text drawing, and information-page rendering. */
#include "gml_render_internal.h"
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
static int font_kerning(const GmlFont *f, unsigned left, unsigned right){
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



static unsigned utf8_next(const char **pp);
static unsigned glyph_code_from_cp(unsigned cp);
static int glyph_frame(GmlFont *f, unsigned cp);
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
static int glyph_w(GmlRender *r, GmlFont *f, int frame, unsigned cp){
  GmlSprite *s=&r->spr[f->sprite];
  if(frame<0||frame>=s->n_frames){
    (void)cp;
    return s->w;        /* out-of-range: blank cell */
  }
  if(s->runtime_rgba) return s->w;
  int ti=s->frame[frame]; if(ti<0||ti>=r->n_tpag) return s->w;
  return f->prop ? r->tpag[ti].sw : (s->w?s->w:r->tpag[ti].sw);
}

static unsigned utf8_next(const char **pp){
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
static unsigned text_next_cp(const char **pp){
  if((*pp)[0]=='\\' && (*pp)[1]=='#'){ *pp+=2; return '#'; }
  return utf8_next(pp);
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
static int glyph_frame(GmlFont *f, unsigned cp){
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

static int text_is_linebreak(GmlRender *r,const char *p){
  return p && (*p=='\r' || *p=='\n' ||
    (*p=='#' && anygm_policy_text_uses_hash_line_breaks(r?r->win:NULL)));
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
                                AnygmTextLayoutFamily family){
  if(!text_is_linebreak(r,p)) return 0;
  if(p[0]=='\r' && p[1]=='\n' && r &&
     (family==ANYGM_TEXT_LAYOUT_EXTENDED ||
      anygm_policy_text_pairs_carriage_return_with_line_feed(r->win))) return 2;
  return 1;
}

/* Width of one policy-delimited line, retaining the existing escaped-hash reading. */
static int line_width(GmlRender *r, GmlFont *f, const char *p, const char **end){
  int w=0;
  while(*p && !text_is_linebreak(r,p)){
    unsigned cp;
    if(p[0]=='\\' && p[1]=='#'){ cp='#'; p+=2; }
    else cp=text_next_cp(&p);
    w += glyph_w(r,f,glyph_frame(f,cp),cp) + f->sep;
  }
  if(w>0) w-=f->sep;
  *end=p; return w;
}
/* ---- real FONT-chunk font helpers ---- */
static GmlGlyph *real_glyph(GmlFont *f, unsigned cp){
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
  if(!A->px || A->w<=0) return NULL;
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
  return gl;
}
static GmlGlyph *real_glyph_demand(GmlRender *r, GmlFont *f, unsigned cp){
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
/* Advance width of one policy-delimited line; escaped hashes retain their existing reading. */
static int real_line_width(GmlRender *r, GmlFont *f, const char *p, const char **end){
  int w=0; unsigned previous=0;
  while(*p && !text_is_linebreak(r,p)){
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
      if(text_is_linebreak(r,q)){ nlines++; q+=text_linebreak_bytes(r,q,family)-1; } }
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
    if(text_is_linebreak(r,q)){ nlines++; q+=text_linebreak_bytes(r,q,family)-1; } }
  double base_y=0;
  double block_height=(nlines-1)*lh+ah;
  if(r->valign==1) base_y=-block_height/2.0; else if(r->valign==2) base_y=-block_height;
  const char *p=str;
  int log_glyphs=render_setting(r,"GML_LOG_TEXT_GLYPHS")!=NULL;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=real_line_width(r,f,p,&end);
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
    if(text_is_linebreak(r,end)) p=end+text_linebreak_bytes(r,end,family); else break;
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
    const char *end; int w = f->real ? real_line_width(r,f,p,&end) : line_width(r,f,p,&end);
    if(w>best) best=w;
    if(!text_is_linebreak(r,end)) break;
    p=end+text_linebreak_bytes(r,end,family);
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
    if(text_is_linebreak(r,p)){ nlines++; p+=text_linebreak_bytes(r,p,family)-1; } }
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
    if(text_is_linebreak(r,p)){ nlines++; p+=text_linebreak_bytes(r,p,family)-1; } }
  double base_y=0;
  if(r->valign==1) base_y=-(nlines*lh)/2.0; else if(r->valign==2) base_y=-nlines*lh;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=line_width(r,f,p,&end);
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
    if(text_is_linebreak(r,end)) p=end+text_linebreak_bytes(r,end,family); else break;
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
                                 AnygmTextLayoutFamily family){
  if(!r || !font || !str || !wrapped || cap<2 || w<=0) return str;
  size_t offset=0;
  const char *p=str;
  for(;;){
    const char *explicit_end=p;
    while(*explicit_end && !text_is_linebreak(r,explicit_end))
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
    p=explicit_end+text_linebreak_bytes(r,explicit_end,family);
  }
  wrapped[offset]=0;
  return wrapped;
}

double gml_text_width_ext(GmlRender *r,const char *str,double sep,double w){
  (void)sep;
  if(!r || !str) return 0;
  GmlFont *font=active_font(r);
  char wrapped[2048];
  const char *layout=text_wrap_ext(r,font,str,w,wrapped,sizeof wrapped,
                                   ANYGM_TEXT_LAYOUT_EXTENDED);
  return text_width_font(r,font,layout,ANYGM_TEXT_LAYOUT_EXTENDED);
}

double gml_text_height_ext(GmlRender *r,const char *str,double sep,double w){
  if(!r || !str) return 0;
  GmlFont *font=active_font(r);
  char wrapped[2048];
  const char *layout=text_wrap_ext(r,font,str,w,wrapped,sizeof wrapped,
                                   ANYGM_TEXT_LAYOUT_EXTENDED);
  if(sep<0) return text_height_font(r,font,layout,ANYGM_TEXT_LAYOUT_EXTENDED);
  int lines=1;
  for(const char *p=layout;*p;p++){
    if(*p=='\\' && p[1]=='#'){ p++; continue; }
    if(text_is_linebreak(r,p)){
      lines++;
      p+=text_linebreak_bytes(r,p,ANYGM_TEXT_LAYOUT_EXTENDED)-1;
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
  str=text_wrap_ext(r,font,str,w,wrapped,sizeof wrapped,family);
  if(sep<0){
    draw_text_transformed_font(r,font,x,y,str,xs,ys,rot,blend,alpha,family);
    return;
  }
  /* custom line separation: draw line by line at y + i*sep */
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;}
    if(text_is_linebreak(r,q)){ nlines++; q+=text_linebreak_bytes(r,q,family)-1; } }
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
    while(*p && !(text_is_linebreak(r,p) &&
          !(*p=='#' && p>str && p[-1]=='\\')) && k<sizeof(lbuf)-1)
      lbuf[k++]=*p++;
    lbuf[k]=0;
    double line_y=base+li*sep;
    draw_text_transformed_font(r,font,x+line_y*ys*sa,y+line_y*ys*ca,
                               lbuf,xs,ys,rr,blend,alpha,family);
    if(!text_is_linebreak(r,p)) break;
    p+=text_linebreak_bytes(r,p,family); li++;
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
