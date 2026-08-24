/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Test shared revision-800 masks, per-subimage classic precise masks, shaped
 * masks, automatic bounds, and sparse runtime sprite IDs. */
/* Earlier shaped sprites use the final subimage for shared automatic bounds;
 * revision-800 shared precise masks combine their subimages. */
#include "gml_render_internal.h"
#include "gmlc_package.h"
#include "gmlc_project.h"
#include "stdio_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"sprite masks failed: %s\n",label); \
    return 1; \
  } \
}while(0)

enum { SPRITE_SIZE=8 };

/* A row of pixels that is opaque up to (but not including) `columns`. */
static void fill_frame(uint8_t *rgba,int columns){
  for(int y=0;y<SPRITE_SIZE;y++)
    for(int x=0;x<SPRITE_SIZE;x++){
      uint8_t *pixel=rgba+((size_t)y*SPRITE_SIZE+(size_t)x)*4u;
      pixel[0]=255; pixel[1]=255; pixel[2]=255;
      pixel[3]=x<columns?255:0;
    }
}

static void fill_rectangle(uint8_t *rgba,int left,int top,int right,int bottom){
  memset(rgba,0,SPRITE_SIZE*SPRITE_SIZE*4u);
  for(int y=top;y<=bottom;y++)
    for(int x=left;x<=right;x++){
      uint8_t *pixel=rgba+((size_t)y*SPRITE_SIZE+(size_t)x)*4u;
      pixel[0]=255; pixel[1]=255; pixel[2]=255; pixel[3]=255;
    }
}

static int mask_bit(const GmlSprite *sprite,int x,int y){
  return (sprite->mask[(size_t)y*(size_t)sprite->mask_rowb+(size_t)x/8u]>>(7-(x&7)))&1;
}

int main(void){
  uint8_t wide[SPRITE_SIZE*SPRITE_SIZE*4],narrow[SPRITE_SIZE*SPRITE_SIZE*4];
  uint8_t upper_left[SPRITE_SIZE*SPRITE_SIZE*4],lower_right[SPRITE_SIZE*SPRITE_SIZE*4];
  fill_frame(wide,SPRITE_SIZE);
  fill_frame(narrow,2);
  fill_rectangle(upper_left,1,1,3,4);
  fill_rectangle(lower_right,4,5,7,7);

  char directory[]="build/sprite-masks-XXXXXX";
  REQUIRE(mkdtemp(directory)!=NULL,"temporary root");
  char package_path[256];
  REQUIRE(snprintf(package_path,sizeof package_path,"%s/data.win",directory)<
          (int)sizeof package_path,"fixture path");

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);

  GmlcProject project={0};
  GmlcSprite sprites[2]={{0}};
  GmlcObject object={0};
  GmlcRoom room={0};
  int room_order=0;
  char *frames[4]={NULL,NULL,NULL,NULL};
  project.host=&services;
  project.name=(char *)"sprite-mask-fixture";
  project.classic_version=800;
  project.sprites=sprites;
  project.n_sprites=project.cap_sprites=2;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;

  frames[0]=gmlc_project_add_memory_file(&project,"wide",GMLC_MEMORY_RGBA,wide,sizeof wide,
                                         SPRITE_SIZE,SPRITE_SIZE);
  frames[1]=gmlc_project_add_memory_file(&project,"narrow",GMLC_MEMORY_RGBA,narrow,sizeof narrow,
                                         SPRITE_SIZE,SPRITE_SIZE);
  frames[2]=gmlc_project_add_memory_file(&project,"upper-left",GMLC_MEMORY_RGBA,
                                         upper_left,sizeof upper_left,SPRITE_SIZE,SPRITE_SIZE);
  frames[3]=gmlc_project_add_memory_file(&project,"lower-right",GMLC_MEMORY_RGBA,
                                         lower_right,sizeof lower_right,SPRITE_SIZE,SPRITE_SIZE);
  REQUIRE(frames[0] && frames[1] && frames[2] && frames[3],"subimage fixtures");

  sprites[0].id=sprites[0].name=(char *)"spr_shared";
  sprites[0].runtime_id=1;
  sprites[0].width=sprites[0].height=SPRITE_SIZE;
  sprites[0].bbox_right=sprites[0].bbox_bottom=SPRITE_SIZE-1;
  sprites[0].bbox_mode=2;
  sprites[0].n_frames=2;
  sprites[0].frame_paths=frames;
  sprites[0].sep_masks=0;

  sprites[1].id=sprites[1].name=(char *)"spr_separate_automatic";
  sprites[1].runtime_id=3;
  sprites[1].width=sprites[1].height=SPRITE_SIZE;
  sprites[1].bbox_right=sprites[1].bbox_bottom=SPRITE_SIZE-1;
  sprites[1].bbox_mode=0;
  sprites[1].col_kind=1;
  sprites[1].n_frames=2;
  sprites[1].frame_paths=frames+2;
  sprites[1].sep_masks=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=1;
  object.mask_id=object.parent_id=-1;
  room.id=room.name=(char *)"room_fixture";
  room.width=64;
  room.height=48;
  room.speed=60;

  char error[256]={0};
  int packaged=gmlc_package_write_structural(&project,package_path,error,sizeof error);
  if(!packaged) fprintf(stderr,"sprite mask package failed: %s\n",error);

  GmlWin win;
  GmlRender render;
  int loaded=packaged && anygm_stdio_load_win(&win,package_path)==0;
  int rendered=loaded && gml_render_init(&render,&win)==0;
  int failures=0;
  if(rendered){
    if(render.n_spr!=4 || render.spr[0].name || render.spr[2].name){
      fprintf(stderr,"sprite masks failed: sparse runtime sprite table was not retained\n");
      failures++;
    }
    const GmlSprite *packed=&render.spr[1];
    if(packed->mask_count!=1){
      fprintf(stderr,"sprite masks failed: shared sprite carries %d masks\n",packed->mask_count);
      failures++;
    }else{
      for(int y=0;y<SPRITE_SIZE;y++)
        for(int x=0;x<SPRITE_SIZE;x++){
          int expected=x<SPRITE_SIZE;
          if(mask_bit(packed,x,y)!=expected){
            fprintf(stderr,
                    "sprite masks failed: shared GM8 mask at %d,%d is %d, union says %d\n",
                    x,y,mask_bit(packed,x,y),expected);
            failures++;
            y=SPRITE_SIZE; break;
          }
        }
    }
    const GmlSprite *separate=&render.spr[3];
    const int bounds[2][4]={{1,1,3,4},{4,5,7,7}};
    if(separate->mask_count!=2){
      fprintf(stderr,"sprite masks failed: separate sprite carries %d masks\n",
              separate->mask_count);
      failures++;
    }else{
      for(int map=0;map<2;map++)
        for(int y=0;y<SPRITE_SIZE;y++)
          for(int x=0;x<SPRITE_SIZE;x++){
            int expected=x>=bounds[map][0] && y>=bounds[map][1] &&
                         x<=bounds[map][2] && y<=bounds[map][3];
            const uint8_t *mask=separate->mask+(size_t)map*(size_t)separate->mask_rowb*
                                (size_t)SPRITE_SIZE;
            int actual=(mask[(size_t)y*(size_t)separate->mask_rowb+(size_t)x/8u]>>
                        (7-(x&7)))&1;
            if(actual!=expected){
              fprintf(stderr,
                      "sprite masks failed: automatic mask %d at %d,%d is %d, expected %d\n",
                      map,x,y,actual,expected);
              failures++;
              y=SPRITE_SIZE; break;
            }
          }
    }
    gml_render_free(&render);
  }

  if(loaded) gml_win_free(&win);

  char legacy_path[256];
  REQUIRE(snprintf(legacy_path,sizeof legacy_path,"%s/legacy.win",directory)<
          (int)sizeof legacy_path,"legacy fixture path");
  project.classic_version=700;
  /* Check precise per-subimage and shaped shared masks in one package. */
  sprites[1].sep_masks=0;
  int legacy_packaged=gmlc_package_write_structural(&project,legacy_path,error,sizeof error);
  GmlWin legacy_win;
  GmlRender legacy_render;
  int legacy_loaded=legacy_packaged && anygm_stdio_load_win(&legacy_win,legacy_path)==0;
  int legacy_rendered=legacy_loaded && gml_render_init(&legacy_render,&legacy_win)==0;
  if(legacy_rendered){
    /* Earlier precise sprites use per-subimage masks without a separate-mask flag. */
    const GmlSprite *legacy=&legacy_render.spr[1];
    const int legacy_columns[2]={SPRITE_SIZE,2};
    if(legacy->mask_count!=2){
      fprintf(stderr,"sprite masks failed: GM7 sprite carries %d masks, one per subimage says 2\n",
              legacy->mask_count);
      failures++;
    }else{
      for(int map=0;map<2;map++)
        for(int y=0;y<SPRITE_SIZE;y++)
          for(int x=0;x<SPRITE_SIZE;x++){
            int expected=x<legacy_columns[map];
            const uint8_t *mask=legacy->mask+(size_t)map*(size_t)legacy->mask_rowb*
                                (size_t)SPRITE_SIZE;
            int actual=(mask[(size_t)y*(size_t)legacy->mask_rowb+(size_t)x/8u]>>(7-(x&7)))&1;
            if(actual!=expected){
              fprintf(stderr,
                      "sprite masks failed: GM7 mask %d at %d,%d is %d, subimage %d says %d\n",
                      map,x,y,actual,map,expected);
              failures++;
              y=SPRITE_SIZE; break;
            }
          }
    }
    const GmlSprite *shaped=&legacy_render.spr[3];
    if(shaped->mask_count!=1){
      fprintf(stderr,"sprite masks failed: shaped GM7 sprite carries %d masks, one says 1\n",
              shaped->mask_count);
      failures++;
    }else{
      for(int y=0;y<SPRITE_SIZE;y++)
        for(int x=0;x<SPRITE_SIZE;x++){
          int expected=x>=4 && y>=5;
          if(mask_bit(shaped,x,y)!=expected){
            fprintf(stderr,
                    "sprite masks failed: shaped GM7 mask at %d,%d is %d, last bounds say %d\n",
                    x,y,mask_bit(shaped,x,y),expected);
            failures++;
            y=SPRITE_SIZE; break;
          }
        }
    }
    gml_render_free(&legacy_render);
  }
  if(legacy_loaded) gml_win_free(&legacy_win);
  for(size_t i=0;i<sizeof frames/sizeof frames[0];i++) free(frames[i]);
  for(int i=0;i<project.n_memory_files;i++) free(project.memory_files[i].data);
  free(project.memory_files);
  remove(package_path);
  remove(legacy_path);
  rmdir(directory);

  REQUIRE(packaged,"structural package");
  REQUIRE(loaded,"packaged content did not load");
  REQUIRE(rendered,"renderer init");
  REQUIRE(legacy_packaged,"legacy structural package");
  REQUIRE(legacy_loaded,"legacy packaged content did not load");
  REQUIRE(legacy_rendered,"legacy renderer init");
  if(failures) return 1;
  puts("sprite masks: ok");
  return 0;
}
