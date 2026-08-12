/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Regenerate src/generated/gml_studio_default_font_data.h from a named TrueType file.
 *
 * Portable by construction: the rasterizer is the vendored stb_truetype, so the table can be
 * rebuilt on any host from the same input file. Every parameter is fixed here rather than read
 * from the machine, so two runs on two machines produce the same bytes:
 *
 *   advance       9 pixels for every code point, which is what fixes the scale: the face is
 *                 monospaced, and the scale is the one that maps its advance width to 9 pixels
 *                 exactly rather than a nominal point size rounded by a rasterizer
 *   line box      20 rows, the GML_STUDIO_DEFAULT_FONT_LINE_HEIGHT the consumer scales from
 *   baseline      row 16 of the line box; capitals occupy rows 5..15 at this scale
 *   pen           x = 0; a glyph whose left bearing is negative is clipped at the box edge
 *   coverage      stb_truetype's antialiased coverage, 0..255, integer-positioned
 *   code points   32..127 read as Unicode; a code the face has no glyph for is drawn as the
 *                 face's own .notdef
 *
 * Per glyph: width and height are the ink box measured from the top-left of the line box,
 * offset is the ink's left column, shift is the advance. A blank glyph stores its advance by
 * the full line height, all zero.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FIRST 32
#define LAST 127
#define LINE_HEIGHT 20
#define BASELINE 16
#define ADVANCE 9
#define CELL 32

typedef struct { int w,h,shift,offset; unsigned char *px; } Glyph;

static unsigned char *read_file(const char *path,size_t *size){
  FILE *f=fopen(path,"rb"); if(!f) return NULL;
  fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  unsigned char *d=(unsigned char*)malloc((size_t)n);
  if(!d || fread(d,1,(size_t)n,f)!=(size_t)n){ fclose(f); free(d); return NULL; }
  fclose(f); *size=(size_t)n; return d;
}

int main(int argc,char **argv){
  if(argc<3){ fprintf(stderr,"usage: gen_studio_font <ttf> <out.h>\n"); return 2; }
  size_t size=0; unsigned char *ttf=read_file(argv[1],&size);
  if(!ttf){ fprintf(stderr,"cannot read the font file\n"); return 1; }
  stbtt_fontinfo font;
  if(!stbtt_InitFont(&font,ttf,stbtt_GetFontOffsetForIndex(ttf,0))){ fprintf(stderr,"not a TrueType file\n"); return 1; }
  /* The face is monospaced: every advance is the same number of font units, and the scale is
   * the one that makes it exactly ADVANCE pixels. */
  int advance_units=0,lsb=0; stbtt_GetCodepointHMetrics(&font,'A',&advance_units,&lsb);
  if(advance_units<=0){ fprintf(stderr,"the face has no advance for 'A'\n"); return 1; }
  float scale=(float)ADVANCE/(float)advance_units;
  Glyph g[LAST-FIRST+1]; memset(g,0,sizeof g);
  unsigned char cell[LINE_HEIGHT*CELL];
  int substituted=0;
  for(int c=FIRST;c<=LAST;c++){
    int index=stbtt_FindGlyphIndex(&font,c);
    if(index==0) substituted++;
    memset(cell,0,sizeof cell);
    int x0,y0,x1,y1;
    stbtt_GetGlyphBitmapBox(&font,index,scale,scale,&x0,&y0,&x1,&y1);
    int bw=x1-x0,bh=y1-y0;
    if(bw>0 && bh>0){
      unsigned char *bm=(unsigned char*)calloc((size_t)bw*bh,1);
      stbtt_MakeGlyphBitmap(&font,bm,bw,bh,bw,scale,scale,index);
      for(int y=0;y<bh;y++){
        int ty=BASELINE+y0+y; if(ty<0||ty>=LINE_HEIGHT) continue;
        for(int x=0;x<bw;x++){
          int tx=x0+x; if(tx<0||tx>=CELL) continue;
          cell[ty*CELL+tx]=bm[y*bw+x];
        }
      }
      free(bm);
    }
    int ix0=CELL,ix1=-1,iy1=-1;
    for(int y=0;y<LINE_HEIGHT;y++) for(int x=0;x<CELL;x++)
      if(cell[y*CELL+x]){ if(x<ix0)ix0=x; if(x>ix1)ix1=x; if(y>iy1)iy1=y; }
    Glyph *o=&g[c-FIRST];
    if(ix1<0){ o->w=ADVANCE; o->h=LINE_HEIGHT; o->offset=0; o->shift=ADVANCE;
               o->px=(unsigned char*)calloc((size_t)o->w*o->h,1); }
    else { o->offset=ix0; o->w=ix1-ix0+1; o->h=iy1+1; o->shift=ADVANCE;
           o->px=(unsigned char*)malloc((size_t)o->w*o->h);
           for(int y=0;y<o->h;y++) for(int x=0;x<o->w;x++)
             o->px[(size_t)y*o->w+x]=cell[y*CELL+(x+ix0)]; }
  }
  FILE *out=fopen(argv[2],"wb");
  if(!out){ fprintf(stderr,"cannot write the output\n"); return 1; }
  fprintf(out,
   "/* SPDX-License-Identifier: MIT AND OFL-1.1\n * Copyright (c) 2026 retrodiv <retrodiv@proton.me>\n * C declarations and first-party comments are under MIT.\n * Derived glyph coverage remains under OFL-1.1; see\n * LICENSES/font-roboto-mono-ofl-1.1.txt for the upstream notices. */\n/* Roboto Mono Medium fallback coverage, rasterized from upstream v3.001\n * through stb_truetype with a nine-pixel advance and 20-row line box. */\n"
   "#ifndef GML_STUDIO_DEFAULT_FONT_DATA_H\n#define GML_STUDIO_DEFAULT_FONT_DATA_H\n\n#include <stdint.h>\n\n"
   "#define GML_STUDIO_DEFAULT_FONT_FIRST %d\n#define GML_STUDIO_DEFAULT_FONT_LAST %d\n"
   "#define GML_STUDIO_DEFAULT_FONT_LINE_HEIGHT %d\n\n"
   "static const GmlDefaultGlyph gml_studio_default_glyphs[] = {\n",FIRST,LAST,LINE_HEIGHT);
  unsigned long off=0; int col=0;
  for(int c=FIRST;c<=LAST;c++){
    Glyph *o=&g[c-FIRST];
    if(col==0) fprintf(out,"  ");
    fprintf(out,"{%lu, %d, %d, %d, %d},",off,o->w,o->h,o->shift,o->offset);
    off+=(unsigned long)o->w*o->h;
    if(++col==4){ fprintf(out,"\n"); col=0; } else fprintf(out,"    ");
  }
  if(col) fprintf(out,"\n");
  fprintf(out,"};\n\nstatic const uint8_t gml_studio_default_font_alpha[] = {\n");
  col=0;
  for(int c=FIRST;c<=LAST;c++){
    Glyph *o=&g[c-FIRST];
    for(size_t i=0,n=(size_t)o->w*o->h;i<n;i++){
      if(col==0) fprintf(out,"  ");
      fprintf(out,"%u,",o->px[i]);
      if(++col==24){ fprintf(out,"\n"); col=0; }
    }
  }
  if(col) fprintf(out,"\n");
  fprintf(out,"};\n\n#endif\n");
  fclose(out);
  fprintf(stderr,"wrote %s: %d glyphs, %lu coverage bytes, scale %.6f, %d codes drawn as .notdef\n",
          argv[2],LAST-FIRST+1,off,scale,substituted);
  free(ttf);
  return 0;
}
