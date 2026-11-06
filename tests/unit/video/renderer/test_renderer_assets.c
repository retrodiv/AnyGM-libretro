/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_state.h"
#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "stdio_vfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"renderer assets failed: %s\n",label); \
    return 1; \
  } \
}while(0)

/* One pixel per subimage, two subimages, so the decoded frame count is unambiguous and cannot be
 * confused with a strip that a requested count would cut. */
static const uint8_t animated_two_frame_gif[]={
  0x47,0x49,0x46,0x38,0x39,0x61,0x01,0x00,0x01,0x00,0x80,0x00,0x00,
  0xFF,0x00,0x00,0x00,0x00,0xFF,
  0x21,0xF9,0x04,0x00,0x00,0x00,0x00,0x00,
  0x2C,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
  0x02,0x02,0x44,0x01,0x00,
  0x21,0xF9,0x04,0x00,0x00,0x00,0x00,0x00,
  0x2C,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
  0x02,0x02,0x4C,0x01,0x00,
  0x3B
};

/* A four-pixel row that a requested count of four cuts into four single-pixel subimages. */
static const uint8_t strip_gif[]={
  0x47,0x49,0x46,0x38,0x39,0x61,0x04,0x00,0x01,0x00,0x80,0x00,0x00,
  0xFF,0x00,0x00,0x00,0x00,0xFF,
  0x2C,0x00,0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x00,
  0x02,0x03,0x44,0x82,0x05,0x00,
  0x3B
};

static int write_file(const char *path,const uint8_t *data,size_t size){
  FILE *file=fopen(path,"wb");
  if(!file) return 0;
  size_t written=fwrite(data,1,size,file);
  fclose(file);
  return written==size;
}

static int runtime_sprite_names_survive_state(void){
  GmlWin win={0};
  GmlRender source,restored;
  REQUIRE(gml_render_init(&source,&win)==0 && gml_render_init(&restored,&win)==0,
          "sprite name state renderer init");
  source.spr=calloc(1,sizeof *source.spr);
  restored.spr=calloc(1,sizeof *restored.spr);
  REQUIRE(source.spr && restored.spr,"authored placeholder allocation");
  source.n_spr=source.spr_cap=source.base_n_spr=1;
  restored.n_spr=restored.spr_cap=restored.base_n_spr=1;
  char names[3][96];
  for(int i=0;i<3;i++){
    int extent=i==2?64:4;
    uint8_t *rgba=malloc((size_t)extent*extent*4);
    REQUIRE(rgba!=NULL,"sprite name state pixels");
    memset(rgba,255,(size_t)extent*extent*4);
    int id=gml_sprite_append_from_rgba(&source,rgba,extent,extent,0,0,
                                     i==1?"named_runtime_asset":NULL);
    GmlRenderSpriteMetrics metrics;
    REQUIRE(id==i+1 && gml_render_sprite_metrics(&source,id,&metrics) && metrics.name,
            "sprite name state source identity");
    snprintf(names[i],sizeof names[i],"%s",metrics.name);
  }
  size_t size=gml_render_state_size(&source,-1),written=0,used=0;
  uint8_t *state=malloc(size);
  REQUIRE(state && gml_render_state_save(&source,-1,state,size,&written) && written==size,
          "sprite name state save");
  for(int pass=0;pass<2;pass++){
    REQUIRE(gml_render_state_load(&restored,state,size,&used) && used==size,
            "sprite name state load");
    for(int i=0;i<3;i++){
      GmlRenderSpriteMetrics metrics;
      REQUIRE(gml_render_sprite_metrics(&restored,i+1,&metrics) && metrics.name &&
              !strcmp(metrics.name,names[i]) && gml_render_named_sprite(&restored,names[i])==i+1,
              "raw and compressed sprites must retain their addressable names after restore");
      if(pass==0){
        GmlSprite *sprite=&restored.spr[i+1];
        free(sprite->owned_name);
        sprite->owned_name=strdup("changed_live_identity");
        REQUIRE(sprite->owned_name!=NULL,"changed live sprite name");
        sprite->name=sprite->owned_name;
        restored.spr_name_gen++;
      }
    }
  }
  free(state);
  gml_render_free(&restored);
  gml_render_free(&source);
  return 0;
}

static int surface_subimages_retain_existing_frames(void){
  GmlWin win={0}; GmlRender render,restored; GmlVM vm={0};
  REQUIRE(gml_render_init(&render,&win)==0 && gml_render_init(&restored,&win)==0,
          "surface subimage renderer init");
  vm.render=&render;
  int surface=gml_surface_create(&render,4,4);
  uint32_t *pixels=surface_pixels(&render,surface,NULL,NULL);
  REQUIRE(surface>0 && pixels,"surface subimage source");
  for(int i=0;i<16;i++) pixels[i]=0xFFFF0000u;
  int sprite=gml_sprite_create_from_surface(&render,surface,0,0,2,2,0,0,1,1);
  REQUIRE(sprite>=0 && gml_sprite_collision_mask(&render,sprite,1,2,1,1,1,1,0,0),
          "surface subimage initial sprite and manual bounds");
  memset(pixels,0,16*sizeof *pixels);
  pixels[0]=0xFFFF0000u;pixels[1]=0xFF00FF00u;
  pixels[2]=0xFF0000FFu;pixels[3]=0xFFFFFFFFu;
  GmlVal args[8]={vreal(sprite),vreal(surface),vreal(0),vreal(0),
                 vreal(2),vreal(2),vreal(0),vreal(0)};
  GmlVal result=gml_builtin_call(&vm,"sprite_add_from_surface",args,8);
  REQUIRE(result.t==vundef().t && gml_sprite_frames(&render,sprite)==2,
          "surface append adds one subimage and returns undefined");
  REQUIRE(gml_sprite_collision(&render,sprite,0,1,1) &&
          gml_sprite_collision(&render,sprite,1,1,1),
          "surface append retains the original collision plane for the new image");
  args[4]=vreal(4);args[5]=vreal(1);
  (void)gml_builtin_call(&vm,"sprite_add_from_surface",args,8);
  GmlSprite *s=&render.spr[sprite];
  REQUIRE(s->w==2 && s->h==2 && s->originx==1 && s->originy==1 && s->n_frames==3 &&
          s->ml==1 && s->mt==1 && s->mr==1 && s->mb==1,
          "surface append retains geometry, origin and manual collision bounds");
  const uint8_t expected[48]={
    255,0,0,255, 255,0,0,255, 255,0,0,255, 255,0,0,255,
    255,0,0,255, 0,255,0,255, 0,0,0,0, 0,0,0,0,
    255,0,0,255, 0,0,255,255, 255,0,0,255, 0,0,255,255
  };
  REQUIRE(!memcmp(s->runtime_rgba,expected,sizeof expected),
          "surface append retains old frames and normalizes the new region by nearest sampling");
  args[4]=vreal(0);
  (void)gml_builtin_call(&vm,"sprite_add_from_surface",args,8);
  args[4]=vreal(2);args[1]=vreal(-1);
  (void)gml_builtin_call(&vm,"sprite_add_from_surface",args,8);
  REQUIRE(s->n_frames==3 && !memcmp(s->runtime_rgba,expected,sizeof expected),
          "invalid surface append leaves the sprite unchanged");
  size_t size=gml_render_state_size(&render,-1),written=0,used=0;
  uint8_t *state=malloc(size);
  REQUIRE(state && gml_render_state_save(&render,-1,state,size,&written) && written==size &&
          gml_render_state_load(&restored,state,size,&used) && used==size,
          "appended subimage state roundtrip");
  REQUIRE(restored.spr[sprite].n_frames==3 &&
          !memcmp(restored.spr[sprite].runtime_rgba,expected,sizeof expected) &&
          gml_sprite_collision(&restored,sprite,1,1,1),
          "restored subimages retain every source byte and the independent collision plane");
  REQUIRE(gml_sprite_collision_mask(&render,sprite,1,2,1,1,1,1,0,0) &&
          !gml_sprite_collision(&render,sprite,1,1,1),
          "explicit mask rebuilding adopts the appended frame alpha");
  free(state);
  gml_render_free(&restored);gml_render_free(&render);
  return 0;
}

int main(void){
  REQUIRE(surface_subimages_retain_existing_frames()==0,"surface subimage regression");
  REQUIRE(runtime_sprite_names_survive_state()==0,"runtime sprite name state regression");
  char root[]="build/renderer-assets-XXXXXX";
  REQUIRE(mkdtemp(root)!=NULL,"temporary root");
  char animated[256],strip[256];
  REQUIRE(snprintf(animated,sizeof animated,"%s/animated.gif",root)<(int)sizeof animated &&
          snprintf(strip,sizeof strip,"%s/strip.gif",root)<(int)sizeof strip,"fixture paths");
  REQUIRE(write_file(animated,animated_two_frame_gif,sizeof animated_two_frame_gif),
          "animated fixture");
  REQUIRE(write_file(strip,strip_gif,sizeof strip_gif),"strip fixture");

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlWin win;
  memset(&win,0,sizeof win);
  win.host=&services;
  GmlRender render;
  REQUIRE(gml_render_init(&render,&win)==0,"renderer init");

  /* An animated image states its own subimage count. Applying a one-image request to it would
   * collapse the animation to its first frame. */
  int animated_sprite=gml_sprite_add_file(&render,animated,1,0,0,0);
  REQUIRE(animated_sprite>=0,"animated sprite was not added");
  REQUIRE(gml_sprite_frames(&render,animated_sprite)==2,
          "animated subimage count did not come from the file");

  int all_frames=gml_sprite_add_file(&render,animated,-1,0,0,0);
  REQUIRE(all_frames>=0 && gml_sprite_frames(&render,all_frames)==2,
          "an unrestricted request changed the animated subimage count");

  /* A single-image strip keeps taking its subimage count from the request, which is the only
   * information that says where to cut it. */
  int strip_sprite=gml_sprite_add_file(&render,strip,4,0,0,0);
  REQUIRE(strip_sprite>=0 && gml_sprite_frames(&render,strip_sprite)==4,
          "strip subimage count did not follow the request");
  int whole_strip=gml_sprite_add_file(&render,strip,1,0,0,0);
  REQUIRE(whole_strip>=0 && gml_sprite_frames(&render,whole_strip)==1,
          "an uncut strip gained subimages");

  /* Replacement follows the same rule, since content commonly swaps a placeholder for a file. */
  REQUIRE(gml_sprite_replace_from_file(&render,strip_sprite,animated,1,0,0,0,0),
          "animated replacement failed");
  REQUIRE(gml_sprite_frames(&render,strip_sprite)==2,
          "animated replacement did not take its subimage count from the file");

  /* Runtime sprite pixels and their precise collision plane are independent. Assignment and
   * duplication replace the whole asset, so both representations follow the source. */
  GmlRender runtime_sprites={0};
  runtime_sprites.n_spr=runtime_sprites.spr_cap=2;
  runtime_sprites.spr=calloc(2,sizeof *runtime_sprites.spr);
  REQUIRE(runtime_sprites.spr!=NULL,"runtime sprite fixture allocation");
  GmlSprite *authored=&runtime_sprites.spr[0];
  authored->w=4; authored->h=2; authored->n_frames=1;
  authored->runtime_rgba=calloc(8,4);
  REQUIRE(authored->runtime_rgba!=NULL,"runtime sprite pixel allocation");
  authored->runtime_owned=1;
  authored->ml=0; authored->mt=0; authored->mr=3; authored->mb=1;
  authored->runtime_mask=malloc(2);
  REQUIRE(authored->runtime_mask!=NULL,"runtime sprite collision allocation");
  authored->runtime_mask[0]=0x00;
  authored->runtime_mask[1]=0x80;
  authored->mask=authored->runtime_mask;
  authored->mask_rowb=1;
  authored->mask_count=1;
  REQUIRE(gml_sprite_collision(&runtime_sprites,0,0,0,1),
          "runtime pixels replaced the independent collision plane");
  REQUIRE(gml_sprite_assign(&runtime_sprites,1,0),
          "assigning the runtime sprite failed");
  REQUIRE(runtime_sprites.spr[1].mask_count==1 && runtime_sprites.spr[1].mask_rowb==1,
          "sprite assignment did not retain the source collision plane");
  REQUIRE(gml_sprite_collision(&runtime_sprites,1,0,0,1),
          "sprite assignment replaced the source collision with copied-frame alpha");
  int duplicate=gml_sprite_duplicate(&runtime_sprites,0);
  REQUIRE(duplicate==2,"duplicating the runtime sprite failed");
  runtime_sprites.spr[0].runtime_mask[1]=0x00;
  REQUIRE(!gml_sprite_collision(&runtime_sprites,0,0,0,1),
          "runtime sprite collision mutation was not visible to its owner");
  REQUIRE(gml_sprite_collision(&runtime_sprites,1,0,0,1),
          "sprite assignment retained an alias to the source collision plane");
  REQUIRE(runtime_sprites.spr[duplicate].mask_count==1 &&
          gml_sprite_collision(&runtime_sprites,duplicate,0,0,1),
          "sprite duplication replaced the source collision with copied-frame alpha");
  gml_render_free(&runtime_sprites);

  gml_render_free(&render);
  puts("renderer assets: ok");
  return 0;
}
