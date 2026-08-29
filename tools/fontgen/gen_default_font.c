/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Regenerate src/generated/gml_default_font_data.h from a named TrueType file.
 *
 * Windows-only by construction: the bundled coverage is GDI output and nothing else reproduces
 * it. The layout parameters below are fixed here rather than taken from the system. GDI versions
 * may affect rasterization, so reproduction compares the complete generated artifact:
 *
 *   face          the family name inside the file, loaded privately with AddFontResourceEx so
 *                 nothing has to be installed; the selected bytes are checked against the file
 *   lfHeight      -16, which is 12 point at 96 dpi
 *   quality       NONANTIALIASED_QUALITY: every pixel is 0 or 255, so no system font-smoothing
 *                 setting can reach the result
 *   line box      18 rows, the GML_DEFAULT_FONT_LINE_HEIGHT the consumer scales from
 *   pen           ExtTextOut at (1,0) with TA_LEFT|TA_TOP
 *   code points   32..255 read as Unicode; a code the face has no glyph for is drawn as the
 *                 face's own .notdef, not as whatever the system's font linking substitutes
 *
 * Per glyph: width and height are the ink box measured from the top-left of the line box,
 * offset is the ink's left column, shift is GetCharWidth32W. A blank glyph stores its advance
 * by the full line height, all zero.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gdi_font_input.h"

#define FIRST 32
#define LAST 255
#define LINE_HEIGHT 18
#define PEN_X 1

typedef struct { int w,h,shift,offset; unsigned char *px; } Glyph;

int main(int argc,char **argv){
  if(argc<5){ fprintf(stderr,"usage: gen_default_font <ttf> <face> <advances.txt> <out.h>\n"); return 2; }
  /* The advances are first-party compatibility widths, not read from the
   * typeface. Only the coverage below comes from the font file. */
  int advance[LAST+1]; for(int i=0;i<=LAST;i++) advance[i]=-1;
  { FILE *af=fopen(argv[3],"rb");
    if(!af){ fprintf(stderr,"cannot read the advance table\n"); return 1; }
    char line[256];
    while(fgets(line,sizeof line,af)){
      if(line[0]=='#'||line[0]=='\n') continue;
      int c=0,a=0; if(sscanf(line,"%d %d",&c,&a)==2 && c>=FIRST && c<=LAST) advance[c]=a; }
    fclose(af);
    for(int c=FIRST;c<=LAST;c++) if(advance[c]<0){ fprintf(stderr,"advance table is missing %d\n",c); return 1; } }
  wchar_t path[1024],face[256];
  MultiByteToWideChar(CP_UTF8,0,argv[1],-1,path,1024);
  MultiByteToWideChar(CP_UTF8,0,argv[2],-1,face,256);
  if(!AddFontResourceExW(path,FR_PRIVATE,0)){ fprintf(stderr,"cannot load the named font file\n"); return 1; }
  HDC screen=GetDC(NULL), dc=CreateCompatibleDC(screen);
  HFONT font=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_TT_ONLY_PRECIS,
                         CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,face);
  HGDIOBJ oldf=SelectObject(dc,font);
  if(!font || !verify_gdi_font_input(dc,argv[1])) return 1;
  TEXTMETRICW tm; GetTextMetricsW(dc,&tm);
  int cell=tm.tmMaxCharWidth+16;
  Glyph g[LAST-FIRST+1]; memset(g,0,sizeof g);
  BITMAPINFO bi; memset(&bi,0,sizeof bi);
  bi.bmiHeader.biSize=sizeof bi.bmiHeader; bi.bmiHeader.biWidth=cell;
  bi.bmiHeader.biHeight=-LINE_HEIGHT; bi.bmiHeader.biPlanes=1;
  bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
  int substituted=0;
  for(int c=FIRST;c<=LAST;c++){
    wchar_t wc=(wchar_t)c;
    WORD index=0;
    if(GetGlyphIndicesW(dc,&wc,1,&index,GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR)
      return 1;
    int missing=(index==0xFFFF);
    if(missing) substituted++;
    void *bits=NULL;
    HBITMAP bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
    HDC mem=CreateCompatibleDC(dc);
    HGDIOBJ ob=SelectObject(mem,bm), of=SelectObject(mem,font);
    RECT r={0,0,cell,LINE_HEIGHT}; HBRUSH b=CreateSolidBrush(RGB(0,0,0));
    FillRect(mem,&r,b); DeleteObject(b);
    SetBkMode(mem,TRANSPARENT); SetTextColor(mem,RGB(255,255,255)); SetTextAlign(mem,TA_LEFT|TA_TOP);
    INT adv=advance[c];
    if(missing) index=0;
    if(!ExtTextOutW(mem,PEN_X,0,ETO_GLYPH_INDEX,NULL,(LPCWSTR)&index,1,NULL)) return 1;
    GdiFlush();
    unsigned char *p8=(unsigned char*)bits;
    int x0=cell,x1=-1,y1=-1;
    for(int y=0;y<LINE_HEIGHT;y++) for(int x=0;x<cell;x++){
      unsigned char *p=p8+((size_t)y*cell+x)*4;
      if(p[0]|p[1]|p[2]){ if(x<x0)x0=x; if(x>x1)x1=x; if(y>y1)y1=y; } }
    Glyph *o=&g[c-FIRST];
    if(x1<0){ o->w=adv>0?adv:1; o->h=LINE_HEIGHT; o->offset=PEN_X; o->shift=adv;
              o->px=(unsigned char*)calloc((size_t)o->w*o->h,1); }
    else { o->offset=x0; o->w=x1-x0+1; o->h=y1+1; o->shift=adv;
           o->px=(unsigned char*)malloc((size_t)o->w*o->h);
           for(int y=0;y<o->h;y++) for(int x=0;x<o->w;x++)
             o->px[(size_t)y*o->w+x]=p8[((size_t)y*cell+(x+x0))*4+2]; }
    SelectObject(mem,of); SelectObject(mem,ob); DeleteDC(mem); DeleteObject(bm);
  }
  FILE *out=fopen(argv[4],"wb");
  if(!out){ fprintf(stderr,"cannot write the output\n"); return 1; }
  fprintf(out,
   "/* SPDX-License-Identifier: MIT AND OFL-1.1\n"
   " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>\n"
   " * C declarations and first-party comments are under MIT.\n"
   " * Derived font coverage remains under OFL-1.1; see\n"
   " * LICENSES/font-ofl-1.1.txt for upstream notices and terms. */\n"
   "/* Generated fallback glyph coverage for the classic default font.\n"
   " * Rasterized from OFL-1.1 Liberation Sans 2.1.5 through GDI at 12 point, no antialiasing;\n"
   " * tools/fontgen/gen_default_font.c is the generator and states every parameter.\n"
   " * See LICENSES/font-ofl-1.1.txt and docs/PROVENANCE.md. */\n"
   "#ifndef GML_DEFAULT_FONT_DATA_H\n#define GML_DEFAULT_FONT_DATA_H\n\n#include <stdint.h>\n\n"
   "#define GML_DEFAULT_FONT_FIRST %d\n#define GML_DEFAULT_FONT_LAST %d\n"
   "#define GML_DEFAULT_FONT_LINE_HEIGHT %d\n\n"
   "typedef struct { uint32_t off; uint8_t width, height, shift; int8_t offset; } GmlDefaultGlyph;\n"
   "/* Raster coverage comes from the redistributable typeface named above. Advances are\n"
   " * first-party compatibility widths, not derived from the typeface. */\n"
   "static const GmlDefaultGlyph gml_default_glyphs[] = {\n",FIRST,LAST,LINE_HEIGHT);
  unsigned long off=0; int col=0;
  for(int c=FIRST;c<=LAST;c++){
    Glyph *o=&g[c-FIRST];
    if(col==0) fprintf(out,"  ");
    fprintf(out,"{%lu, %d, %d, %d, %d},",off,o->w,o->h,o->shift,o->offset);
    off+=(unsigned long)o->w*o->h;
    if(++col==4){ fprintf(out,"\n"); col=0; } else fprintf(out,"    ");
  }
  if(col) fprintf(out,"\n");
  fprintf(out,"};\n\nstatic const uint8_t gml_default_font_alpha[] = {\n");
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
  fprintf(stderr,"wrote %s: %d glyphs, %lu coverage bytes, %d codes drawn as .notdef\n",
          argv[4],LAST-FIRST+1,off,substituted);
  SelectObject(dc,oldf); DeleteObject(font); DeleteDC(dc); ReleaseDC(NULL,screen);
  RemoveFontResourceExW(path,FR_PRIVATE,0);
  return 0;
}
