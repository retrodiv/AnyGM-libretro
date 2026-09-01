/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Regenerate src/generated/gml_classic_info_font_data.h from four named TrueType files.
 *
 * This generator rasterizes the table using GDI ClearType on Windows, as
 * gen_default_font.c does for its table. What keeps the result reproducible
 * is that nothing is read from the machine's installed fonts or left to its discretion:
 *
 *   input         the four styles are loaded privately from the named files with
 *                 AddFontResourceExW(FR_PRIVATE); the selected bytes must match the supplied
 *                 file, and glyph-index drawing prevents font linking to installed faces
 *   faces         the size/style set and each face's line height and y offset are fixed below;
 *                 they are layout the runtime measures against and a regeneration must not move
 *   height        -MulDiv(half_points, 96, 144): the RTF half-point size at 96 dpi
 *   quality       CLEARTYPE_QUALITY; the generator refuses to run unless font smoothing is on
 *                 and in ClearType mode, and it records the smoothing type, contrast and
 *                 subpixel orientation it saw into the generated header, so the exact smoothing
 *                 state that produced the bytes is part of the artifact
 *   code points   bytes 32..255 mapped through Windows-1252, the table's text encoding;
 *                 missing codes use this face's own .notdef glyph
 *   coverage      per-channel ClearType coverage, 255 minus the channel of black text drawn on
 *                 a white 32-bit DIB; the box keeps rows from the top of the line cell to the
 *                 last inked row and columns over the inked span, exactly as the consumer
 *                 expects; a glyph with no ink stores its advance by the full line height, zero
 *   shift         the GDI advance; offset is the ink's first column relative to the pen
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gdi_font_input.h"

#define FIRST 32
#define LAST 255
#define PAD_X 8
#define CELL_W 160
#define CELL_H 96

typedef struct { int half_points, bold, italic, line_height, y_offset; } FaceSpec;

/* The committed size/style set. line_height and y_offset are layout the renderer and the
 * classic info composer already measure against; they are inputs here, never re-derived. */
static const FaceSpec FACES[] = {
  {72,1,0,56,0}, {48,1,0,37,0}, {14,0,0,10,0}, {14,1,0,10,0}, {18,0,0,14,0},
  {18,0,1,14,0}, {18,1,0,14,0}, {20,0,0,15,0}, {20,1,1,15,0}, {24,0,0,17,0},
  {24,1,0,17,0}, {26,1,0,19,0}, {28,0,0,20,0}, {28,1,0,20,0}, {32,0,0,23,0},
  {32,0,1,23,0}, {32,1,0,23,0}, {36,0,0,27,-1}, {36,1,0,27,0}, {36,1,1,27,0},
  {38,0,0,28,0}, {44,1,1,32,0}, {52,1,0,38,0},
};
#define N_FACES ((int)(sizeof FACES / sizeof FACES[0]))

/* Windows-1252: the 0x80..0x9F row; every other byte maps to its own code point. */
static const unsigned short CP1252_HIGH[32] = {
  0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
  0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
  0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
  0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x2026,
};

typedef struct { unsigned off; int w,h,shift,offset; unsigned char *rgb; } Glyph;

static const char *style_name(int bold,int italic){
  if(bold&&italic) return "bold_italic";
  if(bold) return "bold";
  if(italic) return "italic";
  return "regular";
}

int main(int argc,char **argv){
  if(argc<7){
    fprintf(stderr,"usage: gen_classic_info_font <family> <regular.ttf> <bold.ttf> "
                   "<italic.ttf> <bolditalic.ttf> <out.h>\n");
    return 2;
  }
  const char *family=argv[1], *out_path=argv[6];
  wchar_t wfamily[64];
  if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,family,-1,wfamily,64)) return 1;

  for(int i=2;i<6;i++){
    wchar_t wpath[1024];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[i],-1,wpath,1024)) return 1;
    if(!AddFontResourceExW(wpath,FR_PRIVATE,0)){
      fprintf(stderr,"cannot load %s privately\n",argv[i]); return 1;
    }
  }

  BOOL smoothing=FALSE; UINT type=0,contrast=0,orientation=0;
  SystemParametersInfoW(SPI_GETFONTSMOOTHING,0,&smoothing,0);
  SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE,0,&type,0);
  SystemParametersInfoW(SPI_GETFONTSMOOTHINGCONTRAST,0,&contrast,0);
  SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION,0,&orientation,0);
  if(!smoothing||type!=FE_FONTSMOOTHINGCLEARTYPE){
    fprintf(stderr,"font smoothing is not in ClearType mode (on=%d type=%u); refusing\n",
            (int)smoothing,type);
    return 1;
  }
  fprintf(stderr,"smoothing: on, type %u, contrast %u, orientation %u\n",
          type,contrast,orientation);

  HDC screen=GetDC(NULL);
  HDC hdc=CreateCompatibleDC(screen);
  BITMAPINFO bmi; memset(&bmi,0,sizeof bmi);
  bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth=CELL_W; bmi.bmiHeader.biHeight=-CELL_H;
  bmi.bmiHeader.biPlanes=1; bmi.bmiHeader.biBitCount=32; bmi.bmiHeader.biCompression=BI_RGB;
  void *bits=NULL;
  HBITMAP dib=CreateDIBSection(hdc,&bmi,DIB_RGB_COLORS,&bits,NULL,0);
  if(!dib||!bits){ fprintf(stderr,"cannot create the DIB\n"); return 1; }
  SelectObject(hdc,dib);

  FILE *out=fopen(out_path,"wb");
  if(!out){ fprintf(stderr,"cannot open %s\n",out_path); return 1; }
  fprintf(out,
    "/* SPDX-License-Identifier: MIT AND OFL-1.1\n"
    " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>\n"
    " * The C declarations and first-party comments are MIT; derived font data remains\n"
    " * under OFL-1.1 with upstream notices in LICENSES/font-ofl-1.1.txt. */\n"
    "/* Classic information-page glyph coverage rasterized from %s through GDI\n"
    " * ClearType (smoothing type %u, contrast %u, subpixel orientation %u).\n"
    " * Concrete input verification values are recorded outside this repository. */\n"
    "#ifndef GML_CLASSIC_INFO_FONT_DATA_H\n"
    "#define GML_CLASSIC_INFO_FONT_DATA_H\n\n"
    "#include <stdint.h>\n\n"
    "#define GML_CLASSIC_INFO_FONT_FIRST 32\n"
    "#define GML_CLASSIC_INFO_FONT_LAST 255\n\n"
    "typedef struct { uint32_t off; uint8_t width, height, shift; int8_t offset; } GmlClassicInfoGlyph;\n"
    "typedef struct {\n"
    "  int half_points, bold, italic, line_height, y_offset;\n"
    "  const GmlClassicInfoGlyph *glyphs;\n"
    "  const uint8_t *rgb_coverage;\n"
    "} GmlClassicInfoFontSource;\n",
    family,type,contrast,orientation);

  const int glyph_count=LAST-FIRST+1;
  Glyph *glyphs=calloc(glyph_count,sizeof *glyphs);

  for(int face=0;face<N_FACES;face++){
    const FaceSpec *spec=&FACES[face];
    int height=-MulDiv(spec->half_points,96,144);
    HFONT font=CreateFontW(height,0,0,0,spec->bold?FW_BOLD:FW_NORMAL,spec->italic?TRUE:FALSE,
                           FALSE,FALSE,DEFAULT_CHARSET,OUT_TT_ONLY_PRECIS,CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,wfamily);
    if(!font){ fprintf(stderr,"cannot create the %d/%d/%d face\n",
                       spec->half_points,spec->bold,spec->italic); return 1; }
    HGDIOBJ previous=SelectObject(hdc,font);
    if(!verify_gdi_font_input(hdc,argv[2+spec->bold+2*spec->italic])) return 1;
    wchar_t selected[64]={0};
    GetTextFaceW(hdc,64,selected);
    if(_wcsicmp(selected,wfamily)!=0){
      fprintf(stderr,"GDI substituted the face: asked for %s, got %ls\n",family,selected);
      return 1;
    }
    TEXTMETRICW tm; GetTextMetricsW(hdc,&tm);
    if((int)tm.tmHeight!=spec->line_height)
      fprintf(stderr,"note: face %s_%d GDI line height %d, table keeps %d\n",
              style_name(spec->bold,spec->italic),spec->half_points,
              (int)tm.tmHeight,spec->line_height);
    SetTextAlign(hdc,TA_TOP|TA_LEFT|TA_NOUPDATECP);
    SetBkMode(hdc,OPAQUE);
    SetBkColor(hdc,RGB(255,255,255));
    SetTextColor(hdc,RGB(0,0,0));

    unsigned running=0;
    for(int i=0;i<glyph_count;i++){
      int byte=FIRST+i;
      wchar_t ch=(byte>=0x80&&byte<=0x9F)?(wchar_t)CP1252_HIGH[byte-0x80]:(wchar_t)byte;
      RECT all={0,0,CELL_W,CELL_H};
      HBRUSH white=(HBRUSH)GetStockObject(WHITE_BRUSH);
      FillRect(hdc,&all,white);
      WORD index=0;
      if(GetGlyphIndicesW(hdc,&ch,1,&index,GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR)
        return 1;
      if(index==0xFFFF) index=0;
      if(!ExtTextOutW(hdc,PAD_X,0,ETO_GLYPH_INDEX,NULL,(LPCWSTR)&index,1,NULL)) return 1;
      GdiFlush();
      int advance=0;
      if(!GetCharWidth32W(hdc,ch,ch,&advance)) advance=0;

      int rows=spec->line_height<CELL_H?spec->line_height:CELL_H;
      int ink_x0=CELL_W,ink_x1=-1,ink_y1=-1;
      const unsigned char *px=(const unsigned char*)bits;
      for(int y=0;y<rows;y++) for(int x=0;x<CELL_W;x++){
        const unsigned char *p=px+((size_t)y*CELL_W+x)*4;
        if(p[0]!=255||p[1]!=255||p[2]!=255){
          if(x<ink_x0) ink_x0=x;
          if(x>ink_x1) ink_x1=x;
          if(y>ink_y1) ink_y1=y;
        }
      }
      Glyph *g=&glyphs[i];
      g->off=running;
      if(ink_x1<0){
        g->w=advance>0?advance:1; g->h=spec->line_height; g->shift=advance; g->offset=0;
        g->rgb=calloc((size_t)g->w*g->h*3,1);
      }else{
        g->w=ink_x1-ink_x0+1; g->h=ink_y1+1; g->shift=advance; g->offset=ink_x0-PAD_X;
        g->rgb=malloc((size_t)g->w*g->h*3);
        for(int y=0;y<g->h;y++) for(int x=0;x<g->w;x++){
          const unsigned char *p=px+((size_t)y*CELL_W+ink_x0+x)*4;
          unsigned char *d=g->rgb+((size_t)y*g->w+x)*3;
          d[0]=(unsigned char)(255-p[2]);  /* R */
          d[1]=(unsigned char)(255-p[1]);  /* G */
          d[2]=(unsigned char)(255-p[0]);  /* B */
        }
      }
      running+=(unsigned)g->w*g->h*3;
    }

    const char *style=style_name(spec->bold,spec->italic);
    fprintf(out,"\nstatic const GmlClassicInfoGlyph gml_classic_info_%s_%d_glyphs[] = {\n",
            style,spec->half_points);
    for(int i=0;i<glyph_count;i++){
      if(i%4==0) fprintf(out,"  ");
      fprintf(out,"{%u,%d,%d,%d,%d},",glyphs[i].off,glyphs[i].w,glyphs[i].h,
              glyphs[i].shift,glyphs[i].offset);
      fprintf(out,(i%4==3||i==glyph_count-1)?"\n":" ");
    }
    fprintf(out,"};\n");
    fprintf(out,"\nstatic const uint8_t gml_classic_info_%s_%d_rgb_coverage[] = {\n",
            style,spec->half_points);
    unsigned total=0;
    for(int i=0;i<glyph_count;i++) total+=(unsigned)glyphs[i].w*glyphs[i].h*3;
    unsigned emitted=0;
    for(int i=0;i<glyph_count;i++){
      const Glyph *g=&glyphs[i];
      unsigned n=(unsigned)g->w*g->h*3;
      for(unsigned k=0;k<n;k++){
        if(emitted%24==0) fprintf(out,"  ");
        fprintf(out,"%u,",g->rgb[k]);
        emitted++;
        fprintf(out,(emitted%24==0||emitted==total)?"\n":"");
      }
    }
    fprintf(out,"};\n");
    for(int i=0;i<glyph_count;i++){ free(glyphs[i].rgb); glyphs[i].rgb=NULL; }
    SelectObject(hdc,previous);
    DeleteObject(font);
    fprintf(stderr,"face %s_%d: %u coverage bytes\n",style,spec->half_points,total);
  }

  fprintf(out,"\nstatic const GmlClassicInfoFontSource gml_classic_info_font_sources[] = {\n");
  for(int face=0;face<N_FACES;face++){
    const FaceSpec *spec=&FACES[face];
    const char *style=style_name(spec->bold,spec->italic);
    fprintf(out,"  {%d,%d,%d,%d,%d,gml_classic_info_%s_%d_glyphs,gml_classic_info_%s_%d_rgb_coverage},\n",
            spec->half_points,spec->bold,spec->italic,spec->line_height,spec->y_offset,
            style,spec->half_points,style,spec->half_points);
  }
  fprintf(out,"};\n#define GML_CLASSIC_INFO_FONT_SOURCE_COUNT \\\n"
              "  ((int)(sizeof(gml_classic_info_font_sources) / sizeof(gml_classic_info_font_sources[0])))\n\n"
              "#endif\n");
  fclose(out);
  free(glyphs);
  fprintf(stderr,"wrote %s\n",out_path);
  return 0;
}
