/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_font_raster.h"
#include "gml_default_font_data.h"
#include "gml_image_codec.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_fonts(GmlcProject *project){
  for(int i = 0; i < project->n_fonts; ++i){
    free(project->fonts[i].id); free(project->fonts[i].name);
    free(project->fonts[i].png_path); free(project->fonts[i].glyphs);
  }
  free(project->fonts);
  project->fonts = NULL;
  project->n_fonts = project->cap_fonts = 0;
}

typedef struct {
  char *face;
  int point_size, bold, italic;
  int first, last, charset, antialias;
} ClassicFontSpec;

typedef struct {
  uint8_t *rgba;
  int width, height, line_height;
  GmlcFontGlyph *glyphs;
  int n_glyphs;
} ClassicFontRaster;

static void classic_font_spec_free(ClassicFontSpec *spec){
  if(!spec) return;
  free(spec->face);
  memset(spec,0,sizeof(*spec));
}

static void classic_font_raster_free(ClassicFontRaster *raster){
  if(!raster) return;
  free(raster->rgba);
  free(raster->glyphs);
  memset(raster,0,sizeof(*raster));
}

static int classic_font_parse(const GmlcClassicManifest *classic,
                              const GmlcClassicResourceSlot *slot,
                              ClassicFontSpec *spec, char *err, size_t errcap){
  memset(spec,0,sizeof(*spec));
  ImportReader r={slot->payload,slot->payload_size,0,err,errcap};
  if(!import_copy_string(&r,&spec->face,"font face")) return 0;
  uint32_t fields[5];
  for(int field=0;field<5;field++) if(!import_u32(&r,&fields[field],"font field")){
    classic_font_spec_free(spec);
    return 0;
  }
  if(r.pos!=r.size && !slot->executable_layout){
    if(err && errcap) snprintf(err,errcap,"classic import: trailing font payload");
    classic_font_spec_free(spec);
    return 0;
  }
  spec->point_size=(int32_t)fields[0];
  if(spec->point_size<1) spec->point_size=1;
  if(spec->point_size>256) spec->point_size=256;
  spec->bold=fields[1]!=0;
  spec->italic=fields[2]!=0;
  uint32_t first=fields[3];
  if(classic->inventory.header.version==GMLC_CLASSIC_GM81){
    spec->antialias=(int)(first>>24);
    spec->charset=(int)((first>>16)&0xffu);
    first&=0xffffu;
  }
  int range_first=(int)first, range_last=(int)fields[4];
  /* Classic strings use the format's single-byte character map. Controls have no printable
   * glyph, and FONT records store a 16-bit character id, so retain the meaningful byte range. */
  if(range_first<GML_DEFAULT_FONT_FIRST) range_first=GML_DEFAULT_FONT_FIRST;
  if(range_last>GML_DEFAULT_FONT_LAST) range_last=GML_DEFAULT_FONT_LAST;
  if(range_last<range_first){
    range_first=GML_DEFAULT_FONT_FIRST;
    range_last=GML_DEFAULT_FONT_LAST;
  }
  spec->first=range_first;
  spec->last=range_last;
  return 1;
}

/* A compiled classic resource carries the compiler's complete alpha atlas after the ordinary project
 * metadata. Importing it verbatim preserves the original glyph hints, metrics and host-font
 * choice. Return zero for a metadata-only resource, one for an imported atlas and -1 on error. */
static int classic_font_build_compiled(const GmlcClassicResourceSlot *slot,
                                       const ClassicFontSpec *spec,
                                       ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  memset(raster,0,sizeof(*raster));
  if(!slot || !slot->executable_layout) return 0;
  ImportReader r={slot->payload,slot->payload_size,0,err,errcap};
  uint32_t ignored=0;
  if(!import_skip_string(&r,NULL,&ignored,"compiled font face") ||
     !import_skip_words(&r,5,"compiled font fields")) return -1;
  if(r.pos==r.size) return 0;

  uint32_t map[256u*6u];
  for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)
    if(!import_u32(&r,&map[i],"compiled font glyph map")) return -1;
  uint32_t width=0,height=0,stored_alpha_size=0;
  const uint8_t *stored_alpha=NULL,*alpha=NULL;
  if(!import_u32(&r,&width,"compiled font atlas width") ||
     !import_u32(&r,&height,"compiled font atlas height") ||
     !import_blob(&r,&stored_alpha,&stored_alpha_size,"compiled font atlas") || r.pos!=r.size ||
     !width || !height || width>4096 || height>4096){
    if(err && errcap && !err[0]) snprintf(err,errcap,"classic import: invalid compiled font atlas");
    return -1;
  }
  size_t alpha_size=(size_t)width*(size_t)height;
  uint8_t *owned_alpha=NULL;
  if(slot->legacy_layout){
    owned_alpha=(uint8_t*)malloc(alpha_size);
    size_t decoded_size=0;
    if(!owned_alpha ||
       !gml_deflate_decode_to_buffer(stored_alpha,stored_alpha_size,GML_DEFLATE_ZLIB,
                                     owned_alpha,alpha_size,&decoded_size) ||
       decoded_size!=alpha_size){
      free(owned_alpha);
      if(err && errcap) snprintf(err,errcap,"classic import: invalid compressed font atlas");
      return -1;
    }
    alpha=owned_alpha;
  } else {
    if(alpha_size!=stored_alpha_size){
      if(err && errcap) snprintf(err,errcap,"classic import: invalid compiled font atlas");
      return -1;
    }
    alpha=stored_alpha;
  }
  uint8_t *rgba=(uint8_t*)malloc((size_t)alpha_size*4u);
  int count=spec->last-spec->first+1;
  GmlcFontGlyph *glyphs=(GmlcFontGlyph*)calloc((size_t)count,sizeof(*glyphs));
  if(!rgba || !glyphs){
    free(owned_alpha); free(rgba); free(glyphs);
    if(err && errcap) snprintf(err,errcap,"classic import: out of memory loading compiled font");
    return -1;
  }
  for(size_t pixel=0;pixel<alpha_size;pixel++){
    rgba[pixel*4u]=rgba[pixel*4u+1u]=rgba[pixel*4u+2u]=255;
    rgba[pixel*4u+3u]=alpha[pixel];
  }
  free(owned_alpha);
  int line_height=0;
  for(int i=0;i<count;i++){
    int ch=spec->first+i;
    const uint32_t *entry=map+(size_t)ch*6u;
    uint32_t x=entry[0],y=entry[1],w=entry[2],h=entry[3];
    if(x>width || y>height || w>width-x || h>height-y ||
       x>INT32_MAX || y>INT32_MAX || w>INT32_MAX || h>INT32_MAX){
      free(rgba); free(glyphs);
      if(err && errcap) snprintf(err,errcap,"classic import: invalid compiled glyph bounds");
      return -1;
    }
    glyphs[i].ch=ch; glyphs[i].x=(int)x; glyphs[i].y=(int)y;
    glyphs[i].w=(int)w; glyphs[i].h=(int)h;
    glyphs[i].shift=(int32_t)entry[4]; glyphs[i].offset=(int32_t)entry[5];
    if((int)h>line_height) line_height=(int)h;
  }
  if(line_height<1) line_height=1;
  raster->rgba=rgba; raster->width=(int)width; raster->height=(int)height;
  raster->line_height=line_height; raster->glyphs=glyphs; raster->n_glyphs=count;
  return 1;
}

typedef struct {
  const char *regular, *bold, *italic, *bold_italic;
  int category, inherent_bold;
} ClassicFontFiles;

typedef struct {
  char *path;
  int bold, italic;
} ClassicFontFile;

enum { CLASSIC_FONT_SANS, CLASSIC_FONT_MONO, CLASSIC_FONT_SERIF };

static void classic_font_normalize_face(const char *face, char *normalized, size_t cap){
  size_t out=0;
  for(const unsigned char *p=(const unsigned char*)face; p && *p && out+1<cap; ++p){
    if(*p<128 && isalnum(*p)) normalized[out++]=(char)tolower(*p);
  }
  normalized[out]='\0';
}

static int classic_font_prefix(const char *name, const char *prefix){
  return !strncmp(name,prefix,strlen(prefix));
}

/* Classic projects name Windows families, while the serialized project contains no outlines.
 * These are family-to-filename conventions used by Windows itself, not resource-specific rules.
 * Script/charset suffixes share the same outlines and are intentionally matched by prefix. */
static ClassicFontFiles classic_font_files_for_face(const char *face){
  char name[128];
  classic_font_normalize_face(face,name,sizeof(name));
  if(classic_font_prefix(name,"arialblack"))
    return (ClassicFontFiles){"ariblk.ttf","ariblk.ttf","ariblk.ttf","ariblk.ttf",CLASSIC_FONT_SANS,1};
  if(classic_font_prefix(name,"arial"))
    return (ClassicFontFiles){"arial.ttf","arialbd.ttf","ariali.ttf","arialbi.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"couriernew") || !strcmp(name,"courier"))
    return (ClassicFontFiles){"cour.ttf","courbd.ttf","couri.ttf","courbi.ttf",CLASSIC_FONT_MONO,0};
  if(classic_font_prefix(name,"comicsans"))
    return (ClassicFontFiles){"comic.ttf","comicbd.ttf","comici.ttf","comicz.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"calibri"))
    return (ClassicFontFiles){"calibri.ttf","calibrib.ttf","calibrii.ttf","calibriz.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"georgia"))
    return (ClassicFontFiles){"georgia.ttf","georgiab.ttf","georgiai.ttf","georgiaz.ttf",CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"verdana"))
    return (ClassicFontFiles){"verdana.ttf","verdanab.ttf","verdanai.ttf","verdanaz.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"timesnewroman"))
    return (ClassicFontFiles){"times.ttf","timesbd.ttf","timesi.ttf","timesbi.ttf",CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"tahoma"))
    return (ClassicFontFiles){"tahoma.ttf","tahomabd.ttf",NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"centurygothic"))
    return (ClassicFontFiles){"GOTHIC.TTF","GOTHICB.TTF","GOTHICI.TTF","GOTHICBI.TTF",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"franklingothicmediumcond"))
    return (ClassicFontFiles){"FRAMDCN.TTF","framd.ttf","framdit.ttf",NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"berlinsans"))
    return (ClassicFontFiles){"BRLNSR.TTF","BRLNSB.TTF",NULL,"BRLNSDB.TTF",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"microsoftsansserif"))
    return (ClassicFontFiles){"micross.ttf",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"papyrus"))
    return (ClassicFontFiles){"PAPYRUS.TTF",NULL,NULL,NULL,CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"kristenitc"))
    return (ClassicFontFiles){"ITCKRIST.TTF",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"mvboli"))
    return (ClassicFontFiles){"mvboli.ttf",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"raavi"))
    return (ClassicFontFiles){"raavi.ttf","raavib.ttf",NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"meiryo"))
    return (ClassicFontFiles){"meiryo.ttc","meiryob.ttc",NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"dotum") || classic_font_prefix(name,"gulim"))
    return (ClassicFontFiles){"gulim.ttc",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"batang") || classic_font_prefix(name,"gungsuh"))
    return (ClassicFontFiles){"batang.ttc",NULL,NULL,NULL,CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"fixedsys") || classic_font_prefix(name,"smallfonts"))
    return (ClassicFontFiles){NULL,NULL,NULL,NULL,CLASSIC_FONT_MONO,0};
  return (ClassicFontFiles){NULL,NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
}

static char *classic_font_find_leaf(const AnygmHostServices *host,const char *family,
                                    const char *leaf,uint32_t style){
  if(!host || !host->font_resolve || !family || !*family) return NULL;
  char path[4096];
  path[0]=0;
  if(host->font_resolve(host->userdata,family,leaf,style,path,sizeof path)!=ANYGM_OK ||
     !path[0]) return NULL;
  AnygmFileInfo info;
  if(!anygm_vfs_stat(host,path,&info) || !(info.flags&ANYGM_FILE_INFO_REGULAR)) return NULL;
  return copy_string(path);
}

static const char *classic_font_style_leaf(const ClassicFontFiles *files,
                                           int bold, int italic,
                                           int *file_bold, int *file_italic){
  const char *leaf=NULL;
  *file_bold=*file_italic=0;
  if(bold && italic && files->bold_italic){ leaf=files->bold_italic; *file_bold=*file_italic=1; }
  else if(bold && files->bold){ leaf=files->bold; *file_bold=1; }
  else if(italic && files->italic){ leaf=files->italic; *file_italic=1; }
  else { leaf=files->regular; *file_bold=files->inherent_bold; }
  return leaf;
}

static ClassicFontFile classic_font_resolve_file(const AnygmHostServices *host,
                                                 const ClassicFontSpec *spec){
  ClassicFontFiles files=classic_font_files_for_face(spec->face);
  ClassicFontFile result={0};
  int file_bold=0,file_italic=0;
  const char *leaf=classic_font_style_leaf(&files,spec->bold,spec->italic,&file_bold,&file_italic);
  uint32_t style=(spec->bold?ANYGM_FONT_STYLE_BOLD:0u)|
                 (spec->italic?ANYGM_FONT_STYLE_ITALIC:0u)|
                 (files.category==CLASSIC_FONT_MONO?ANYGM_FONT_STYLE_MONOSPACE:0u)|
                 (files.category==CLASSIC_FONT_SERIF?ANYGM_FONT_STYLE_SERIF:0u);
  result.path=classic_font_find_leaf(host,spec->face,leaf,style);
  if(!result.path && leaf!=files.regular){
    result.path=classic_font_find_leaf(host,spec->face,files.regular,style);
    file_bold=files.inherent_bold; file_italic=0;
  }
  if(!result.path && (files.regular || files.category!=CLASSIC_FONT_SANS)){
    ClassicFontFiles substitute;
    if(files.category==CLASSIC_FONT_MONO)
      substitute=(ClassicFontFiles){"LiberationMono-Regular.ttf","LiberationMono-Bold.ttf",
        "LiberationMono-Italic.ttf","LiberationMono-BoldItalic.ttf",CLASSIC_FONT_MONO,0};
    else if(files.category==CLASSIC_FONT_SERIF)
      substitute=(ClassicFontFiles){"LiberationSerif-Regular.ttf","LiberationSerif-Bold.ttf",
        "LiberationSerif-Italic.ttf","LiberationSerif-BoldItalic.ttf",CLASSIC_FONT_SERIF,0};
    else
      substitute=(ClassicFontFiles){"LiberationSans-Regular.ttf","LiberationSans-Bold.ttf",
        "LiberationSans-Italic.ttf","LiberationSans-BoldItalic.ttf",CLASSIC_FONT_SANS,0};
    leaf=classic_font_style_leaf(&substitute,spec->bold,spec->italic,&file_bold,&file_italic);
    const char *family=files.category==CLASSIC_FONT_MONO?"monospace":
                       files.category==CLASSIC_FONT_SERIF?"serif":"sans-serif";
    result.path=classic_font_find_leaf(host,family,leaf,style);
  }
  result.bold=file_bold;
  result.italic=file_italic;
  return result;
}

static int classic_font_codepoint(const ClassicFontSpec *spec, int ch){
  static const uint16_t windows_1252[32]={
    0x20ac,0,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
    0x02c6,0x2030,0x0160,0x2039,0x0152,0,0x017d,0,
    0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
    0x02dc,0x2122,0x0161,0x203a,0x0153,0,0x017e,0x0178
  };
  if(ch>=0x80 && ch<0xa0 && (spec->charset==0 || spec->charset==1)){
    int mapped=windows_1252[ch-0x80];
    if(mapped) return mapped;
  }
  return ch;
}

static int classic_font_build_truetype(const AnygmHostServices *host,const ClassicFontSpec *spec,
                                       const ClassicFontFile *file,
                                       ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  memset(raster,0,sizeof(*raster));
  uint8_t *font_data=NULL;
  size_t file_size=0;
  if(!file->path || !anygm_vfs_read_all(host,file->path,&font_data,&file_size,32u*1024u*1024u))
    return 0;
  const char *match=spec->face;
  while(*match=='@') match++;
  GmlFontRasterFace *face=NULL;
  if(!gml_font_raster_face_open(font_data,file_size,match,&face)){
    free(font_data);
    return 0;
  }
  free(font_data);
  float em_pixels=(float)(spec->point_size*96.0/72.0);
  if(em_pixels<4.0f) em_pixels=4.0f;
  if(em_pixels>384.0f) em_pixels=384.0f;
  /* A Windows point size maps the font's em square to points at 96 dpi. ScaleForPixelHeight
   * instead maps ascender-to-descender to that value and noticeably shrinks faces with large
   * Windows line metrics. */
  float scale=gml_font_raster_scale_for_em(face,em_pixels);
  GmlFontRasterMetrics font_metrics;
  if(!(scale>0.0f) || !gml_font_raster_metrics(face,&font_metrics)){
    gml_font_raster_face_close(face);
    return 0;
  }
  int ascent=(int)lround(font_metrics.ascent*scale);
  int line_height=(int)lround((font_metrics.ascent-font_metrics.descent+
                               font_metrics.line_gap)*scale);
  if(line_height<4) line_height=4;
  int synth_bold=spec->bold && !file->bold ? (line_height>=24?2:1) : 0;
  int synth_italic=spec->italic && !file->italic;
  int italic_px=synth_italic?(int)ceil(line_height*0.22):0;
  int count=spec->last-spec->first+1;
  int *widths=(int*)calloc((size_t)count,sizeof(*widths));
  int *xmins=(int*)calloc((size_t)count,sizeof(*xmins));
  GmlcFontGlyph *glyphs=(GmlcFontGlyph*)calloc((size_t)count,sizeof(*glyphs));
  if(!widths || !xmins || !glyphs) goto oom;
  int max_width=0;
  for(int i=0;i<count;i++){
    int codepoint=classic_font_codepoint(spec,spec->first+i);
    GmlFontRasterGlyphMetrics metrics;
    if(!gml_font_raster_glyph_metrics(face,codepoint,scale,&metrics)) goto oom;
    int w=metrics.x1-metrics.x0;
    if(w<1) w=1;
    widths[i]=w+synth_bold+italic_px;
    xmins[i]=metrics.x0;
    if(widths[i]>max_width) max_width=widths[i];
  }
  int atlas_width=512;
  while(atlas_width<max_width+1 && atlas_width<4096) atlas_width*=2;
  if(atlas_width<max_width+1){
    if(err && errcap) snprintf(err,errcap,"classic import: TrueType glyph is too wide");
    free(widths); free(xmins); free(glyphs); gml_font_raster_face_close(face);
    return 0;
  }
  int x=0,y=0,row_height=line_height,used_height=0;
  for(;;){
    x=0; y=0;
    for(int i=0;i<count;i++){
      if(x+widths[i]+1>atlas_width){ x=0; y+=row_height+1; }
      x+=widths[i]+1;
    }
    used_height=y+row_height;
    if(used_height<=4096 || atlas_width>=4096) break;
    atlas_width*=2;
  }
  int atlas_height=32;
  while(atlas_height<used_height && atlas_height<4096) atlas_height*=2;
  if(atlas_height<used_height){
    if(err && errcap) snprintf(err,errcap,"classic import: TrueType atlas is too tall");
    free(widths); free(xmins); free(glyphs); gml_font_raster_face_close(face);
    return 0;
  }
  uint8_t *rgba=(uint8_t*)calloc((size_t)atlas_width*atlas_height,4);
  if(!rgba) goto oom;
  x=0; y=0;
  for(int i=0;i<count;i++){
    int ch=spec->first+i, w=widths[i];
    int codepoint=classic_font_codepoint(spec,ch);
    if(x+w+1>atlas_width){ x=0; y+=row_height+1; }
    GmlFontRasterGlyphMetrics metrics;
    if(!gml_font_raster_glyph_metrics(face,codepoint,scale,&metrics)){
      free(rgba);
      goto oom;
    }
    int bitmap_width=metrics.x1-metrics.x0;
    int bitmap_height=metrics.y1-metrics.y0;
    GmlcFontGlyph *glyph=&glyphs[i];
    glyph->ch=ch; glyph->x=x; glyph->y=y; glyph->w=w; glyph->h=line_height;
    glyph->shift=(int)lround(metrics.advance*scale)+synth_bold;
    if(glyph->shift<1) glyph->shift=1;
    glyph->offset=xmins[i];
    if(bitmap_width>0 && bitmap_height>0){
      uint8_t *bitmap=(uint8_t*)malloc((size_t)bitmap_width*bitmap_height);
      if(!bitmap){ free(rgba); goto oom; }
      (void)gml_font_raster_render_glyph(face,codepoint,scale,bitmap,
                                          (size_t)bitmap_width*bitmap_height,
                                          bitmap_width,bitmap_height,bitmap_width);
      int origin_y=ascent+metrics.y0;
      for(int py=0;py<bitmap_height;py++){
        int dest_y=origin_y+py;
        if(dest_y<0 || dest_y>=line_height) continue;
        int shear=synth_italic?(int)lround((line_height-1-dest_y)*0.22):0;
        for(int px=0;px<bitmap_width;px++){
          uint8_t alpha=bitmap[py*bitmap_width+px];
          for(int bx=0;bx<=synth_bold;bx++){
            int dest_x=px+shear+bx;
            if(dest_x<0 || dest_x>=w) continue;
            uint8_t *pixel=rgba+((size_t)(y+dest_y)*atlas_width+x+dest_x)*4u;
            if(alpha>pixel[3]){
              pixel[0]=pixel[1]=pixel[2]=255;
              pixel[3]=alpha;
            }
          }
        }
      }
      free(bitmap);
    }
    x+=w+1;
  }
  free(widths); free(xmins); gml_font_raster_face_close(face);
  raster->rgba=rgba; raster->width=atlas_width; raster->height=atlas_height;
  raster->line_height=line_height; raster->glyphs=glyphs; raster->n_glyphs=count;
  return 1;
oom:
  if(err && errcap) snprintf(err,errcap,"classic import: out of memory rasterizing TrueType font");
  free(widths); free(xmins); free(glyphs); gml_font_raster_face_close(face);
  return 0;
}

static uint8_t classic_font_sample(const GmlDefaultGlyph *glyph, double x, double y){
  if(x<0.0 || y<0.0 || x>(double)glyph->width-1.0 || y>(double)glyph->height-1.0) return 0;
  int x0=(int)floor(x), y0=(int)floor(y);
  int x1=x0+1<glyph->width?x0+1:x0;
  int y1=y0+1<glyph->height?y0+1:y0;
  double fx=x-x0, fy=y-y0;
  const uint8_t *alpha=gml_default_font_alpha+glyph->off;
  double top=alpha[y0*glyph->width+x0]*(1.0-fx)+alpha[y0*glyph->width+x1]*fx;
  double bottom=alpha[y1*glyph->width+x0]*(1.0-fx)+alpha[y1*glyph->width+x1]*fx;
  int value=(int)lround(top*(1.0-fy)+bottom*fy);
  return (uint8_t)(value<0?0:value>255?255:value);
}

/* The bundled coverage is the portable fallback when the source typeface is unavailable.
 * It represents a 12-point regular face; scale its real per-glyph bounds (not the common line
 * height) and synthesize requested styles so classic font metadata is never silently ignored. */
static int classic_font_build_fallback(const ClassicFontSpec *spec,
                                       ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  memset(raster,0,sizeof(*raster));
  const double scale=(double)spec->point_size/12.0;
  int line_height=(int)lround(GML_DEFAULT_FONT_LINE_HEIGHT*scale);
  if(line_height<4) line_height=4;
  int bold_px=spec->bold?(line_height>=24?2:1):0;
  int italic_px=spec->italic?(int)ceil(line_height*0.22):0;
  int count=spec->last-spec->first+1;
  int *widths=(int*)calloc((size_t)count,sizeof(*widths));
  int *heights=(int*)calloc((size_t)count,sizeof(*heights));
  GmlcFontGlyph *glyphs=(GmlcFontGlyph*)calloc((size_t)count,sizeof(*glyphs));
  if(!widths || !heights || !glyphs) goto oom;
  int max_width=0;
  for(int i=0;i<count;i++){
    const GmlDefaultGlyph *source=&gml_default_glyphs[spec->first+i-GML_DEFAULT_FONT_FIRST];
    int w=(int)ceil(source->width*scale)+bold_px+italic_px;
    int h=(int)ceil(source->height*scale);
    if(w<1) w=1;
    if(h<1) h=1;
    widths[i]=w; heights[i]=h;
    if(w>max_width) max_width=w;
  }
  int atlas_width=512;
  while(atlas_width<max_width+1 && atlas_width<4096) atlas_width*=2;
  if(atlas_width<max_width+1){
    if(err && errcap) snprintf(err,errcap,"classic import: font fallback glyph is too wide");
    free(widths); free(heights); free(glyphs);
    return 0;
  }
  int x=0,y=0,row_height=0,used_height=0;
  for(;;){
    x=0; y=0; row_height=0;
    for(int i=0;i<count;i++){
      int w=widths[i],h=heights[i];
      if(x+w+1>atlas_width){ x=0; y+=row_height+1; row_height=0; }
      if(h>row_height) row_height=h;
      x+=w+1;
    }
    used_height=y+row_height;
    if(used_height<=4096 || atlas_width>=4096) break;
    atlas_width*=2;
  }
  int atlas_height=32;
  while(atlas_height<used_height && atlas_height<4096) atlas_height*=2;
  if(atlas_height<used_height){
    if(err && errcap) snprintf(err,errcap,"classic import: font fallback atlas is too tall");
    free(widths); free(heights); free(glyphs);
    return 0;
  }
  uint8_t *rgba=(uint8_t*)calloc((size_t)atlas_width*atlas_height,4);
  if(!rgba) goto oom;
  x=0; y=0; row_height=0;
  for(int i=0;i<count;i++){
    const GmlDefaultGlyph *source=&gml_default_glyphs[spec->first+i-GML_DEFAULT_FONT_FIRST];
    int w=widths[i], h=heights[i];
    if(x+w+1>atlas_width){ x=0; y+=row_height+1; row_height=0; }
    if(h>row_height) row_height=h;
    GmlcFontGlyph *glyph=&glyphs[i];
    glyph->ch=spec->first+i; glyph->x=x; glyph->y=y; glyph->w=w; glyph->h=h;
    glyph->shift=(int)lround(source->shift*scale)+bold_px;
    if(glyph->shift<1) glyph->shift=1;
    glyph->offset=(int)lround(source->offset*scale);
    for(int py=0;py<h;py++){
      int shear=spec->italic?(int)lround((h-1-py)*0.22):0;
      for(int px=0;px<w-italic_px;px++){
        double sx=((double)px+0.5)/scale-0.5;
        double sy=((double)py+0.5)/scale-0.5;
        uint8_t alpha=classic_font_sample(source,sx,sy);
        for(int bx=0;bx<=bold_px;bx++){
          int dx=x+px+shear+bx;
          if(dx>=x+w) continue;
          uint8_t *pixel=rgba+((size_t)(y+py)*atlas_width+dx)*4u;
          if(alpha>pixel[3]){
            pixel[0]=pixel[1]=pixel[2]=255;
            pixel[3]=alpha;
          }
        }
      }
    }
    x+=w+1;
  }
  free(widths); free(heights);
  raster->rgba=rgba; raster->width=atlas_width; raster->height=atlas_height;
  raster->line_height=line_height; raster->glyphs=glyphs; raster->n_glyphs=count;
  return 1;
oom:
  if(err && errcap) snprintf(err,errcap,"classic import: out of memory building font fallback");
  free(widths); free(heights); free(glyphs);
  return 0;
}

static char *classic_font_store_raster(GmlcProject *project, const char *cache_dir,
                                       uint32_t slot, const ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  char leaf[64];
  snprintf(leaf,sizeof(leaf),"classic_font_%u.png",slot);
  if(project->prefer_memory_files){
    char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_RGBA,raster->rgba,
                                            (size_t)raster->width*raster->height*4u,
                                            raster->width,raster->height);
    if(!path && err && errcap) snprintf(err,errcap,"classic import: out of memory retaining %s",leaf);
    return path;
  }
  char *path=cache_path(cache_dir,leaf);
  if(!path) return NULL;
  GmlMediaBuffer png={0};
  int wrote=gml_image_encode_png(raster->rgba,raster->width,raster->height,4,
                                 raster->width*4,&png) &&
            anygm_vfs_write_all(project->host,path,png.data,png.size);
  gml_media_buffer_release(&png);
  if(!wrote){
    if(err && errcap) snprintf(err,errcap,"classic import: cannot write %s",path);
    free(path);
    return NULL;
  }
  return path;
}

int gmlc_classic_import_fonts(const GmlcClassicManifest *classic,
                              GmlcProject *project, const char *cache_dir,
                              char *err, size_t errcap){
  if(err && errcap) err[0]='\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->fonts || project->n_fonts){
    if(err && errcap) snprintf(err,errcap,"classic import: invalid font-import arguments");
    return 0;
  }
  uint32_t slot_count=classic->inventory.resource_slots[GMLC_CLASSIC_FONT];
  const GmlcClassicResourceSlot *slots=classic->slots[GMLC_CLASSIC_FONT];
  if(slot_count && !slots){
    if(err && errcap) snprintf(err,errcap,"classic import: missing font slots");
    return 0;
  }
  uint32_t count=0;
  for(uint32_t i=0;i<slot_count;i++) if(slots[i].exists) count++;
  if(count>INT32_MAX){ if(err && errcap) snprintf(err,errcap,"classic import: too many fonts"); return 0; }
  /* Empty and deleted classic slots are not resources. In particular, do not create an atlas or
   * reserve runtime font ids for them: sparse projects can have thousands of slots and no fonts. */
  if(!count) return 1;
  project->fonts=(GmlcFont*)calloc(count,sizeof(*project->fonts));
  if(!project->fonts){ if(err && errcap) snprintf(err,errcap,"classic import: out of memory allocating fonts"); return 0; }
  project->cap_fonts=(int)count;
  for(uint32_t i=0;i<slot_count;i++){
    if(!slots[i].exists) continue;
    char fallback[64];
    snprintf(fallback,sizeof(fallback),"__classic_font_slot_%u",i);
    const char *name=slots[i].name?slots[i].name:fallback;
    /* Dense FONT records are required by data.win. The compiler subsequently binds this resource
     * name to its dense index; the original sparse slot number is deliberately not emitted. */
    GmlcFont *font=&project->fonts[project->n_fonts++];
    font->id=copy_string(name); font->name=copy_string(name);
    ClassicFontSpec spec;
    ClassicFontRaster raster;
    if(!classic_font_parse(classic,&slots[i],&spec,err,errcap)){
      classic_font_spec_free(&spec); free_imported_fonts(project); return 0;
    }
    ClassicFontFile file={0};
    int compiled=classic_font_build_compiled(&slots[i],&spec,&raster,err,errcap);
    if(compiled<0){ classic_font_spec_free(&spec); free_imported_fonts(project); return 0; }
    int rasterized=0;
    if(!compiled){
      file=classic_font_resolve_file(project->host,&spec);
      rasterized=file.path && classic_font_build_truetype(project->host,&spec,&file,&raster,err,errcap);
    }
    if(!compiled && !rasterized){
      if(err && errcap) err[0]='\0';
      if(!classic_font_build_fallback(&spec,&raster,err,errcap)){
        free(file.path); classic_font_spec_free(&spec); free_imported_fonts(project); return 0;
      }
    }
    if(anygm_host_development_setting(project->host,"GML_LOG_FONT"))
      anygm_host_logf(project ? project->host : NULL,ANYGM_LOG_DEBUG,"[font] classic face=%s pt=%d bold=%d italic=%d source=%s line=%d glyphs=%d\n",
              spec.face?spec.face:"",spec.point_size,spec.bold,spec.italic,
              compiled?"compiled atlas":rasterized?file.path:"embedded fallback",
              raster.line_height,raster.n_glyphs);
    font->png_path=classic_font_store_raster(project,cache_dir,i,&raster,err,errcap);
    font->width=raster.width; font->height=raster.height; font->em_size=raster.line_height;
    font->glyphs=raster.glyphs; font->n_glyphs=font->cap_glyphs=raster.n_glyphs;
    raster.glyphs=NULL;
    classic_font_raster_free(&raster);
    free(file.path);
    classic_font_spec_free(&spec);
    if(!font->id || !font->name || !font->png_path || !font->glyphs){
      free_imported_fonts(project); return 0;
    }
  }
  return 1;
}
