/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"

static int rounded_fixture(int coloured){
  GmlRender render={0}; GmlVM vm={0};
  uint32_t pixels[24*20],reference[24*20];
  int passed=0;
  render.color=0xFFFFFFu; render.alpha=1; render.alphablend=1;
  render.color_write_mask=15; render.blend_equation=render.blend_equation_alpha=1;
  render.circle_precision=64; vm.render=&render;
  gml_vm_software3d_reset(&vm);
#define ROUND_REQUIRE(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"rounded %s: %s\n",coloured?"colour":"geometry",label); goto done; } } while(0)
  for(int active=0;active<2;active++){
    gml_render_begin(&render,pixels,24,20,0,0);
    if(active){
      call_numbers(&vm,"d3d_start",NULL,0);
      call_numbers(&vm,"d3d_set_projection_ortho",(double[]){0,0,24,20,0},5);
      call_numbers(&vm,"d3d_set_hidden",software3d_disable,1);
    }
    for(int spelling=0;spelling<(coloured?2:1);spelling++){
      const char *name=coloured?(spelling?"draw_roundrect_colour_ext":"draw_roundrect_color_ext"):
        "draw_roundrect_ext";
      double args[]={2,2,18,14,4,4,coloured?255:0,16711680,0};
      int argc=coloured?9:7;
      memset(pixels,0,sizeof pixels);
      call_numbers(&vm,name,args,argc);
      ROUND_REQUIRE(pixels[8*24+10]>>24,"filled centre must be present in both projection modes");
      ROUND_REQUIRE(pixels[2*24+2]==0,"curved corner must not be a rectangle");
      if(coloured){
        uint32_t centre=pixels[8*24+10],edge=pixels[3*24+10];
        ROUND_REQUIRE(((centre>>16)&255)>(centre&255),"centre must favour the inner colour");
        ROUND_REQUIRE((edge&255)>((edge>>16)&255),"edge must favour the outer colour");
      } else {
        memcpy(reference,pixels,sizeof pixels);
        render.alpha=.5; memset(pixels,0,sizeof pixels);
        call_numbers(&vm,name,args,argc);
        uint32_t centre=pixels[8*24+10];
        /* The main framebuffer is XRGB; opacity is observable through blending. */
        ROUND_REQUIRE((centre&255)>0 && (centre&255)<255,"draw alpha must affect RGB");
        for(int i=0;i<24*20;i++)
          ROUND_REQUIRE((reference[i]==0 && pixels[i]==0) ||
                        (reference[i]!=0 && pixels[i]==centre),"fill must have no gaps or double-blended fan seams");
        render.alpha=1;
        render.circle_precision=4; memset(pixels,0,sizeof pixels);
        call_numbers(&vm,name,args,argc);
        ROUND_REQUIRE(!pixels[3*24+3] && reference[3*24+3],"circle precision must change the corner tessellation");
        render.circle_precision=64;
        if(!active) for(int mapping=0;mapping<3;mapping++){
          double mapped[]={2,2,18,14,4,4,0};
          memset(pixels,0,sizeof pixels);
          gml_render_begin(&render,pixels,24,20,mapping==0?3:0,mapping==0?5:0);
          if(mapping==0){ mapped[0]+=3; mapped[2]+=3; mapped[1]+=5; mapped[3]+=5; }
          else {
            for(int i=0;i<6;i++) mapped[i]*=.5;
            if(mapping==1){
              gml_render_gui_begin(&render,24,20);
              gml_render_gui_set_size(&render,12,10);
            }
            else gml_render_world_set_logical_extent(&render,12,10);
          }
          call_numbers(&vm,name,mapped,7);
          if(memcmp(pixels,reference,sizeof pixels)){
            fprintf(stderr,"rounded mapping scenario %d differs\n",mapping); goto done;
          }
          gml_render_gui_end(&render);
        }
        gml_render_begin(&render,pixels,24,20,0,0);
      }
      memset(pixels,0,sizeof pixels); args[coloured?8:6]=1;
      call_numbers(&vm,name,args,argc);
      ROUND_REQUIRE(!pixels[8*24+10],"outline must leave its centre untouched");
      ROUND_REQUIRE(pixels[2*24+10]>>24,"outline must retain the straight top edge");
    }
    call_numbers(&vm,"d3d_end",NULL,0);
  }
  if(!coloured){
    double args[]={2,2,18,14,4,4,0};
    memset(pixels,0,sizeof pixels);
    gml_render_begin(&render,pixels,24,20,0,0);
    call_numbers(&vm,"draw_roundrect_ext",args,7);
    memcpy(reference,pixels,sizeof pixels);
    for(int i=0;i<6;i++){
      double saved=args[i]; args[i]=NAN;
      call_numbers(&vm,"draw_roundrect_ext",args,7); args[i]=saved;
      ROUND_REQUIRE(!memcmp(pixels,reference,sizeof pixels),"nonfinite geometry must leave the target unchanged");
    }
    memset(pixels,0,sizeof pixels);
    call_numbers(&vm,"draw_roundrect_ext",(double[]){18,14,2,2,4,4,0},7);
    ROUND_REQUIRE(!memcmp(pixels,reference,sizeof pixels),"reversed bounds must retain the same geometry");
    memset(pixels,0,sizeof pixels);
    call_numbers(&vm,"draw_primitive_begin",(double[]){4},1);
    call_numbers(&vm,"draw_vertex",(double[]){20,16},2);
    call_numbers(&vm,"draw_vertex",(double[]){23,16},2);
    call_numbers(&vm,"draw_vertex",(double[]){20,19},2);
    call_numbers(&vm,"draw_roundrect_ext",args,7);
    call_numbers(&vm,"draw_primitive_end",NULL,0);
    ROUND_REQUIRE(pixels[16*24+20]>>24,"rounded drawing must not overwrite a pending primitive");
    memset(pixels,0,sizeof pixels);
    call_numbers(&vm,"draw_roundrect_ext",(double[]){2,2,18,14,0,0,0},7);
    ROUND_REQUIRE(pixels[2*24+2]>>24,"zero radii must produce square corners");
    memset(pixels,0,sizeof pixels);
    call_numbers(&vm,"draw_roundrect_ext",(double[]){-1000000,2,1000000,14,4,4,1},7);
    ROUND_REQUIRE(pixels[2*24+10]>>24,"long outlines must retain their visible samples");
  }
  passed=1;
done:
  gml_render_free(&render); vm.render=NULL; gml_vm_free(&vm);
  return passed;
#undef ROUND_REQUIRE
}

int rounded_geometry_fixture(void){ return rounded_fixture(0); }
int rounded_colour_fixture(void){ return rounded_fixture(1); }


int software3d_case_particles(Software3dRasterFixture *fixture){
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  gml_part_reset_all(fixture->vm.particles);
  gml_effect_create(fixture->vm.particles,1,10,0,0,0,0xFFFF00);
  for(int i=0;i<5;i++) gml_part_update_all(fixture->vm.particles);
  gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
  int rain_pixels=0,rain_far_pixels=0,rain_right_pixels=0,rain_alpha_pixels=0;
  for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++) if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]&0x00FFFFFFu){
    rain_pixels++; if(x>8) rain_far_pixels++;
    if(x>56) rain_right_pixels++;
    if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]>>24) rain_alpha_pixels++;
  }
  if(rain_pixels<4 || rain_far_pixels<4 || rain_right_pixels<4 || rain_alpha_pixels!=rain_pixels){
    fprintf(stderr,"software rain effect distribution mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  {
    int system=gml_part_system_create(fixture->vm.particles);
    int type=gml_part_type_create(fixture->vm.particles);
    gml_part_type_shape(fixture->vm.particles,type,5);
    gml_part_type_size(fixture->vm.particles,type,.5,.5,0,0);
    gml_part_type_alpha(fixture->vm.particles,type,1,1,1,1);
    gml_part_particles_create_color(fixture->vm.particles,system,32,24,type,0x0000FF,1);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    int ring_pixels=0,ring_mass=0,minx=SOFTWARE3D_WIDTH,maxx=-1,miny=SOFTWARE3D_HEIGHT,maxy=-1;
    for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++){
      int red=(fixture->pixels[y*SOFTWARE3D_WIDTH+x]>>16)&0xFF;
      if(red){
        ring_pixels++; ring_mass+=red;
        if(x<minx) minx=x;
        if(x>maxx) maxx=x;
        if(y<miny) miny=y;
        if(y>maxy) maxy=y;
      }
    }
    if(ring_pixels!=264 || ring_mass!=25004 ||
       minx!=18 || maxx!=45 || miny!=10 || maxy!=37){
      fprintf(stderr,"classic filtered particle ring mismatch: pixels=%d mass=%d span=(%d,%d)-(%d,%d)\n",
              ring_pixels,ring_mass,minx,miny,maxx,maxy);
      return 0;
    }
  }
  gml_part_reset_all(fixture->vm.particles);

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  {
    int system=gml_part_system_create(fixture->vm.particles);
    int type=gml_part_type_create(fixture->vm.particles);
    gml_part_type_shape(fixture->vm.particles,type,3);
    gml_part_type_size(fixture->vm.particles,type,1,1,0,0);
    gml_part_type_orientation(fixture->vm.particles,type,0,0,0,0,0);
    gml_part_type_alpha(fixture->vm.particles,type,1,1,1,1);
    gml_part_particles_create_color(fixture->vm.particles,system,32,24,type,0x0000FF,1);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    int line_pixels=0,minx=SOFTWARE3D_WIDTH,maxx=-1,miny=SOFTWARE3D_HEIGHT,maxy=-1;
    for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++)
      if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]&0x00FFFFFFu){
        line_pixels++;
        if(x<minx) minx=x;
        if(x>maxx) maxx=x;
        if(y<miny) miny=y;
        if(y>maxy) maxy=y;
      }
    int edge_red=(fixture->pixels[24*SOFTWARE3D_WIDTH+4]>>16)&0xFF;
    int core_red=(fixture->pixels[24*SOFTWARE3D_WIDTH+8]>>16)&0xFF;
    int top_red=(fixture->pixels[19*SOFTWARE3D_WIDTH+32]>>16)&0xFF;
    if(line_pixels!=560 || minx!=4 || maxx!=59 || miny!=19 || maxy!=28 ||
       edge_red<50 || edge_red>52 || core_red!=255 || top_red<76 || top_red>78){
      fprintf(stderr,"classic particle line cell geometry mismatch\n");
      return 0;
    }
  }
  gml_part_reset_all(fixture->vm.particles);

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  {
    int system=gml_part_system_create(fixture->vm.particles);
    int type=gml_part_type_create(fixture->vm.particles);
    gml_part_type_shape(fixture->vm.particles,type,3);
    gml_part_type_size(fixture->vm.particles,type,.2,.2,0,0);
    gml_part_type_orientation(fixture->vm.particles,type,260,260,0,0,0);
    gml_part_type_alpha(fixture->vm.particles,type,1,1,1,1);
    gml_part_particles_create_color(fixture->vm.particles,system,32,24,type,0x0000FF,1);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    int line_pixels=0,minx=SOFTWARE3D_WIDTH,maxx=-1,miny=SOFTWARE3D_HEIGHT,maxy=-1;
    for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++)
      if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]&0x00FFFFFFu){
        line_pixels++;
        if(x<minx) minx=x;
        if(x>maxx) maxx=x;
        if(y<miny) miny=y;
        if(y>maxy) maxy=y;
      }
    if(line_pixels!=23 || minx!=31 || maxx!=34 || miny!=18 || maxy!=29){
      fprintf(stderr,"classic small rotated particle line geometry mismatch\n");
      return 0;
    }
  }
  gml_part_reset_all(fixture->vm.particles);

  {
    static const int shapes[3]={4,8,9};
    static const int min_pixels[3]={180,220,60};
    for(int k=0;k<3;k++){
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF202020u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      int system=gml_part_system_create(fixture->vm.particles);
      int type=gml_part_type_create(fixture->vm.particles);
      gml_part_type_shape(fixture->vm.particles,type,shapes[k]);
      gml_part_type_size(fixture->vm.particles,type,.5,.5,0,0);
      gml_part_type_orientation(fixture->vm.particles,type,0,0,0,0,0);
      gml_part_type_alpha(fixture->vm.particles,type,1,1,1,1);
      gml_part_particles_create_color(fixture->vm.particles,system,32,24,type,0x40A0FF,1);
      gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
      int changed=0,minx=SOFTWARE3D_WIDTH,maxx=-1,miny=SOFTWARE3D_HEIGHT,maxy=-1;
      for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++)
        if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]!=0xFF202020u){
          changed++;
          if(x<minx)minx=x;
          if(x>maxx)maxx=x;
          if(y<miny)miny=y;
          if(y>maxy)maxy=y;
        }
      if(changed<min_pixels[k] || maxx-minx<18 || maxy-miny<18){
        fprintf(stderr,"classic glint shape coverage mismatch: shape=%d pixels=%d span=%dx%d\n",
                shapes[k],changed,maxx-minx+1,maxy-miny+1);
        return 0;
      }
      gml_part_reset_all(fixture->vm.particles);
    }
  }

  {
    static const double scales[3]={.10,.15,.20};
    static const int min_pixels[3]={28,65,113};
    static const int max_pixels[3]={36,77,129};
    for(int k=0;k<3;k++){
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF202020u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      int system=gml_part_system_create(fixture->vm.particles);
      int type=gml_part_type_create(fixture->vm.particles);
      gml_part_type_shape(fixture->vm.particles,type,8);
      gml_part_type_size(fixture->vm.particles,type,scales[k],scales[k],0,0);
      gml_part_type_orientation(fixture->vm.particles,type,0,0,0,0,0);
      gml_part_type_alpha(fixture->vm.particles,type,1,1,1,1);
      gml_part_particles_create_color(fixture->vm.particles,system,32,24,type,0x40FFA0,1);
      gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
      int changed=0;
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) if(fixture->pixels[i]!=0xFF202020u) changed++;
      if(changed<min_pixels[k] || changed>max_pixels[k]){
        fprintf(stderr,"classic small flare footprint mismatch: scale=%.2f pixels=%d expected=%d..%d\n",
                scales[k],changed,min_pixels[k],max_pixels[k]);
        return 0;
      }
      gml_part_reset_all(fixture->vm.particles);
    }
  }

  {
    static const double scales[3]={.5,.5,.2};
    static const double angles[3]={0,30,0};
    static const int min_pixels[3]={680,670,100};
    static const int max_pixels[3]={730,720,120};
    static const int min_span_x[3]={28,30,10};
    static const int min_span_y[3]={30,28,10};
    for(int k=0;k<3;k++){
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF504030u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      int system=gml_part_system_create(fixture->vm.particles);
      int type=gml_part_type_create(fixture->vm.particles);
      gml_part_type_shape(fixture->vm.particles,type,13);
      gml_part_type_size(fixture->vm.particles,type,scales[k],scales[k],0,0);
      gml_part_type_orientation(fixture->vm.particles,type,angles[k],angles[k],0,0,0);
      gml_part_type_alpha(fixture->vm.particles,type,1,.6,.6,.6);
      gml_part_particles_create_color(fixture->vm.particles,system,32,24,type,0xFFFFFF,1);
      gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
      int changed=0,minx=SOFTWARE3D_WIDTH,maxx=-1,miny=SOFTWARE3D_HEIGHT,maxy=-1;
      for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++)
        if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]!=0xFF504030u){
          changed++;
          if(x<minx)minx=x;
          if(x>maxx)maxx=x;
          if(y<miny)miny=y;
          if(y>maxy)maxy=y;
        }
      if(changed<min_pixels[k] || changed>max_pixels[k] ||
         maxx-minx+1<min_span_x[k] || maxy-miny+1<min_span_y[k]){
        fprintf(stderr,"classic six-lobed particle footprint mismatch: scale=%.2f angle=%.0f pixels=%d span=%dx%d\n",
                scales[k],angles[k],changed,maxx-minx+1,maxy-miny+1);
        return 0;
      }
      gml_part_reset_all(fixture->vm.particles);
    }
  }

  gml_effect_create(fixture->vm.particles,1,3,32,24,0,0x40A0FF);
  if(gml_part_system_count(fixture->vm.particles,1)!=75){
    fprintf(stderr,"small firework particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,0,32,24,0,0x40A0FF);
  if(gml_part_system_count(fixture->vm.particles,1)!=21){
    fprintf(stderr,"small explosion particle count mismatch\n");
    return 0;
  }
  for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFFB0B0B0u;
  gml_part_update_all(fixture->vm.particles);
  gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
  int explosion_pixels=0,explosion_minx=SOFTWARE3D_WIDTH,explosion_maxx=-1;
  int explosion_miny=SOFTWARE3D_HEIGHT,explosion_maxy=-1;
  for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++)
    if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]!=0xFFB0B0B0u){
      explosion_pixels++;
      if(x<explosion_minx) explosion_minx=x;
      if(x>explosion_maxx) explosion_maxx=x;
      if(y<explosion_miny) explosion_miny=y;
      if(y>explosion_maxy) explosion_maxy=y;
    }
  if(explosion_pixels<80 || explosion_maxx-explosion_minx<10 || explosion_maxy-explosion_miny<10){
    fprintf(stderr,"classic explosion shape coverage mismatch: pixels=%d span=%dx%d\n",
            explosion_pixels,explosion_maxx-explosion_minx+1,explosion_maxy-explosion_miny+1);
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,1,32,24,0,0x40A0FF);
  gml_effect_create(fixture->vm.particles,1,2,32,24,0,0x40A0FF);
  if(gml_part_system_count(fixture->vm.particles,1)!=2){
    fprintf(stderr,"expanding wave particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,6,32,24,0,0x40A0FF);
  if(gml_part_system_count(fixture->vm.particles,1)!=1){
    fprintf(stderr,"shrinking star particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,7,32,24,0,0x40A0FF);
  gml_effect_create(fixture->vm.particles,1,8,32,24,0,0x40A0FF);
  if(gml_part_system_count(fixture->vm.particles,1)!=2){
    fprintf(stderr,"shrinking glint particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,4,32,24,0,0x808080);
  if(gml_part_system_count(fixture->vm.particles,1)!=6){
    fprintf(stderr,"small smoke particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,5,32,24,1,0x808080);
  if(gml_part_system_count(fixture->vm.particles,1)!=11){
    fprintf(stderr,"medium rising smoke particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,9,32,24,2,0xFFFFFF);
  if(gml_part_system_count(fixture->vm.particles,1)!=1){
    fprintf(stderr,"large cloud particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  gml_effect_create(fixture->vm.particles,1,11,32,24,2,0xFFFFFF);
  if(gml_part_system_count(fixture->vm.particles,1)!=7){
    fprintf(stderr,"large snowfall particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  {
    gml_vm_software3d_reset(&fixture->vm);
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    int system=gml_part_system_create(fixture->vm.particles);
    int type=gml_part_type_create(fixture->vm.particles);
    gml_part_type_size(fixture->vm.particles,type,1,1,0,0);
    gml_part_type_speed(fixture->vm.particles,type,8,8,0,0);
    gml_part_type_direction(fixture->vm.particles,type,0,0,0,90);
    gml_part_particles_create(fixture->vm.particles,system,20,20,type,1);
    size_t particle_state_size=gml_part_state_size(fixture->vm.particles),particle_written=0,particle_used=0;
    void *particle_state=malloc(particle_state_size);
    if(!particle_state || !gml_part_state_save(fixture->vm.particles,particle_state,particle_state_size,&particle_written) ||
       particle_written!=particle_state_size){
      fprintf(stderr,"particle wiggle phase state save failed\n");
      free(particle_state);
      return 0;
    }
    void *particle_repeat=malloc(particle_state_size);
    size_t particle_repeat_written=0;
    if(!particle_repeat ||
       !gml_part_state_save(fixture->vm.particles,particle_repeat,particle_state_size,
                            &particle_repeat_written) ||
       particle_repeat_written!=particle_written ||
       memcmp(particle_repeat,particle_state,particle_written)){
      fprintf(stderr,"repeated particle serialization was not canonical\n");
      free(particle_repeat);
      free(particle_state);
      return 0;
    }
    free(particle_repeat);
    gml_part_update_all(fixture->vm.particles);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    uint32_t expected_pixels[SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    memcpy(expected_pixels,fixture->pixels,sizeof(expected_pixels));
    if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<1 || fixture->pixels[20*SOFTWARE3D_WIDTH+28]!=0){
      int first=-1;
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) if(fixture->pixels[i]){ first=i; break; }
      fprintf(stderr,"particle direction wiggle phase mismatch: first=(%d,%d) count=%d straight=%08x\n",
              first<0?-1:first%SOFTWARE3D_WIDTH,first<0?-1:first/SOFTWARE3D_WIDTH,
              colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT),fixture->pixels[20*SOFTWARE3D_WIDTH+28]);
      free(particle_state);
      return 0;
    }
    if(!gml_part_state_load(fixture->vm.particles,particle_state,particle_written,&particle_used) ||
       particle_used!=particle_written){
      fprintf(stderr,"particle wiggle phase state load failed\n");
      free(particle_state);
      return 0;
    }
    free(particle_state);
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_part_update_all(fixture->vm.particles);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    if(memcmp(fixture->pixels,expected_pixels,sizeof(expected_pixels))){
      fprintf(stderr,"particle wiggle phase state roundtrip mismatch\n");
      return 0;
    }
    gml_part_reset_all(fixture->vm.particles);
  }

  {
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_part_reset_all(fixture->vm.particles);
    gml_effect_create(fixture->vm.particles,1,3,32,24,0,0x0080FF);
    for(int i=0;i<10;i++) gml_part_update_all(fixture->vm.particles);
    size_t particle_state_size=gml_part_state_size(fixture->vm.particles),particle_written=0,particle_used=0;
    void *particle_state=malloc(particle_state_size);
    if(!particle_state || !gml_part_state_save(fixture->vm.particles,particle_state,particle_state_size,&particle_written) ||
       particle_written!=particle_state_size){
      fprintf(stderr,"built-in effect identity state save failed\n");
      free(particle_state);
      return 0;
    }
    gml_effect_create(fixture->vm.particles,1,3,32,24,0,0xFF8000);
    for(int i=0;i<12;i++) gml_part_update_all(fixture->vm.particles);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    uint32_t expected_pixels[SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    memcpy(expected_pixels,fixture->pixels,sizeof(expected_pixels));
    if(!gml_part_state_load(fixture->vm.particles,particle_state,particle_written,&particle_used) ||
       particle_used!=particle_written){
      fprintf(stderr,"built-in effect identity state load failed\n");
      free(particle_state);
      return 0;
    }
    free(particle_state);
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_effect_create(fixture->vm.particles,1,3,32,24,0,0xFF8000);
    for(int i=0;i<12;i++) gml_part_update_all(fixture->vm.particles);
    gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
    if(memcmp(fixture->pixels,expected_pixels,sizeof(expected_pixels))){
      fprintf(stderr,"built-in effect identity state roundtrip mismatch\n");
      return 0;
    }
    gml_part_reset_all(fixture->vm.particles);
  }

  gml_vm_software3d_reset(&fixture->vm);
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  int motion_system=gml_part_system_create(fixture->vm.particles);
  int motion_type=gml_part_type_create(fixture->vm.particles);
  gml_part_type_size(fixture->vm.particles,motion_type,0,0,0,0);
  gml_part_type_speed(fixture->vm.particles,motion_type,1,1,2,0);
  gml_part_type_direction(fixture->vm.particles,motion_type,0,0,0,0);
  gml_part_particles_create(fixture->vm.particles,motion_system,10,10,motion_type,1);
  gml_part_update_all(fixture->vm.particles);
  gml_part_system_draw_all(fixture->vm.particles,&fixture->render);
  if((fixture->pixels[10*SOFTWARE3D_WIDTH+13]&0x00FFFFFFu)==0 || fixture->pixels[10*SOFTWARE3D_WIDTH+11]!=0){
    int first=-1;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) if(fixture->pixels[i]){ first=i; break; }
    fprintf(stderr,"particle speed increment phase mismatch: x13=%08x x11=%08x first=(%d,%d) count=%d\n",
      fixture->pixels[10*SOFTWARE3D_WIDTH+13],fixture->pixels[10*SOFTWARE3D_WIDTH+11],first<0?-1:first%SOFTWARE3D_WIDTH,
      first<0?-1:first/SOFTWARE3D_WIDTH,colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT));
    return 0;
  }
  gml_part_reset_all(fixture->vm.particles);

  {
    /* Studio matrices are column-major and matrix_multiply(a,b) returns b*a. This is the
     * composition used by 2D surface/vertex pipelines to translate, scale, then translate back. */
    gml_vm_software3d_reset(&fixture->vm);
    GmlVal translation=call_values(&fixture->vm,"matrix_build_identity",NULL,0);
    GmlVal scaling=call_values(&fixture->vm,"matrix_build_identity",NULL,0);
    gml_arr_set(translation,12,vreal(3)); gml_arr_set(translation,13,vreal(4));
    gml_arr_set(scaling,0,vreal(2)); gml_arr_set(scaling,5,vreal(3));
    GmlVal multiply_args[2]={translation,scaling};
    GmlVal composed=call_values(&fixture->vm,"matrix_multiply",multiply_args,2);
    GmlVal set_args[2]={vreal(2),composed};
    call_values(&fixture->vm,"matrix_set",set_args,2);
    GmlVal get_arg=vreal(2);
    GmlVal fetched=call_values(&fixture->vm,"matrix_get",&get_arg,1);
    int matrix_flags[GML_SOFTWARE3D_STATE_FLAG_COUNT];
    double matrix_values[GML_SOFTWARE3D_STATE_VALUE_COUNT];
    uint32_t matrix_colors[GML_SOFTWARE3D_STATE_COLOR_COUNT];
    gml_vm_software3d_state_get(&fixture->vm,matrix_flags,matrix_values,matrix_colors);
    if(composed.t!=V_ARR || fetched.t!=V_ARR ||
       fabs(gml_arr_get(fetched,0).d-2)>1e-12 || fabs(gml_arr_get(fetched,5).d-3)>1e-12 ||
       fabs(gml_arr_get(fetched,12).d-6)>1e-12 || fabs(gml_arr_get(fetched,13).d-12)>1e-12 ||
       fabs(matrix_values[56]-2)>1e-12 || fabs(matrix_values[61]-3)>1e-12 ||
       fabs(matrix_values[68]-6)>1e-12 || fabs(matrix_values[69]-12)>1e-12){
      fprintf(stderr,"Studio matrix composition mismatch\n");
      return 0;
    }
    /* A translation-only Studio world matrix affects ordinary portable 2D draws too, including
     * HUDs rendered into surfaces while legacy d3d mode is inactive. Represent it as a camera
     * delta for those software paths and restore the projection camera with the identity matrix. */
    gml_vm_software3d_reset(&fixture->vm);
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,7,9);
    GmlVal translation_set[2]={vreal(2),translation};
    call_values(&fixture->vm,"matrix_set",translation_set,2);
    if(fabs(fixture->render.cam_x-4)>1e-12 || fabs(fixture->render.cam_y-5)>1e-12){
      fprintf(stderr,"Studio software world-translation camera mismatch\n");
      return 0;
    }
    int translated_surface=gml_surface_create(&fixture->render,16,16);
    if(translated_surface<0 || !gml_surface_set_target(&fixture->render,translated_surface) ||
       fabs(fixture->render.cam_x+3)>1e-12 || fabs(fixture->render.cam_y+4)>1e-12){
      fprintf(stderr,"Studio surface world-translation camera mismatch\n");
      return 0;
    }
    GmlVal identity=call_values(&fixture->vm,"matrix_build_identity",NULL,0);
    GmlVal identity_set[2]={vreal(2),identity};
    call_values(&fixture->vm,"matrix_set",identity_set,2);
    if(fabs(fixture->render.cam_x)>1e-12 || fabs(fixture->render.cam_y)>1e-12){
      fprintf(stderr,"Studio surface identity-camera restore mismatch\n");
      return 0;
    }
    gml_surface_reset_target(&fixture->render);
    if(fabs(fixture->render.cam_x-7)>1e-12 || fabs(fixture->render.cam_y-9)>1e-12){
      fprintf(stderr,"Studio projection-camera restore mismatch\n");
      return 0;
    }
    gml_surface_free(&fixture->render,translated_surface);
  }

  gml_vm_software3d_reset(&fixture->vm);
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  return 1;
}


int software3d_case_projection(Software3dRasterFixture *fixture){
  GmlVal start_result=call_values(&fixture->vm,"d3d_start",NULL,0);
  int default_flags[GML_SOFTWARE3D_STATE_FLAG_COUNT];
  double default_values[GML_SOFTWARE3D_STATE_VALUE_COUNT];
  uint32_t default_colors[GML_SOFTWARE3D_STATE_COLOR_COUNT];
  gml_vm_software3d_state_get(&fixture->vm,default_flags,default_values,default_colors);
  if(start_result.t!=V_REAL || start_result.d!=1 || !default_flags[0] ||
     !default_flags[1] || !default_flags[21] || !default_flags[24] ||
     default_flags[20] || default_values[0]!=SOFTWARE3D_WIDTH*.5 ||
     default_values[1]!=SOFTWARE3D_HEIGHT*.5 || default_values[2]!=SOFTWARE3D_WIDTH ||
     default_values[9]!=0 || default_values[10]!=0 || default_values[11]!=-1 ||
     fabs(default_values[51]-1)>1e-12 || fabs(default_values[52]-32000)>1e-12){
    fprintf(stderr,"software D3 historical start defaults mismatch\n");
    return 0;
  }
  GmlVal end_result=call_values(&fixture->vm,"d3d_end",NULL,0);
  gml_vm_software3d_state_get(&fixture->vm,default_flags,default_values,default_colors);
  if(end_result.t!=V_REAL || end_result.d!=1 || default_flags[0]){
    fprintf(stderr,"software D3 historical end result mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  const double floor_args[]={8,6,0,24,18,0,-1,1,1};
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  call_numbers(&fixture->vm,"d3d_set_culling",software3d_enable,1);
  call_numbers(&fixture->vm,"d3d_draw_floor",floor_args,9);
  if((fixture->pixels[10*SOFTWARE3D_WIDTH+10]&0x00FFFFFFu)==0 || fixture->pixels[4*SOFTWARE3D_WIDTH+4]!=0 ||
     colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<150){
    fprintf(stderr,"software D3 orthographic raster mismatch\n");
    return 0;
  }

  /* A floor crossing the perspective near plane must remain a continuous projected polygon.
   * This is the common outdoor-camera shape; a bad clipped-fan depth interpolation used to leave
   * whole alternating scanline bands untouched. */
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  const double perspective[]={32,14,5,32,24,0,0,0,1};
  const double perspective_floor[]={0,0,0,64,48,0,-1,5,5};
  call_numbers(&fixture->vm,"d3d_set_projection",perspective,9);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  call_numbers(&fixture->vm,"d3d_draw_floor",perspective_floor,9);
  int first_row=-1,last_row=-1,empty_inside=0;
  for(int y=0;y<SOFTWARE3D_HEIGHT;y++){
    int row=colored_pixels(fixture->pixels+y*SOFTWARE3D_WIDTH,SOFTWARE3D_WIDTH);
    if(row){ if(first_row<0) first_row=y; last_row=y; }
  }
  if(first_row>=0) for(int y=first_row;y<=last_row;y++)
    if(colored_pixels(fixture->pixels+y*SOFTWARE3D_WIDTH,SOFTWARE3D_WIDTH)==0) empty_inside++;
  if(first_row<0 || empty_inside){
    fprintf(stderr,"software D3 perspective floor has %d empty interior rows (%d..%d)\n",
            empty_inside,first_row,last_row);
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);

  call_numbers(&fixture->vm,"d3d_transform_set_identity",NULL,0);
  call_numbers(&fixture->vm,"d3d_transform_stack_push",NULL,0);
  const double translation[]={16,8,0};
  call_numbers(&fixture->vm,"d3d_transform_add_translation",translation,3);
  int transform_flags[GML_SOFTWARE3D_STATE_FLAG_COUNT];
  double transform_values[GML_SOFTWARE3D_STATE_VALUE_COUNT];
  uint32_t transform_colors[GML_SOFTWARE3D_STATE_COLOR_COUNT];
  gml_vm_software3d_state_get(&fixture->vm,transform_flags,transform_values,transform_colors);
  if(transform_flags[25]!=1 || transform_values[68]!=16 || transform_values[69]!=8){
    fprintf(stderr,"software D3 transform stack push mismatch\n");
    return 0;
  }
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_draw_floor",floor_args,9);
  if((fixture->pixels[18*SOFTWARE3D_WIDTH+26]&0x00FFFFFFu)==0 || fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0){
    fprintf(stderr,"software D3 translated raster mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_transform_stack_pop",NULL,0);
  gml_vm_software3d_state_get(&fixture->vm,transform_flags,transform_values,transform_colors);
  if(transform_flags[25]!=0 || transform_values[68]!=0 || transform_values[69]!=0){
    fprintf(stderr,"software D3 transform stack pop mismatch\n");
    return 0;
  }

  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  const double triangle_kind[]={4};
  const double vertex_a[]={8,8,0,0x0000FF,1};
  const double vertex_b[]={56,8,0,0x00FF00,1};
  const double vertex_c[]={32,40,0,0xFF0000,1};
  call_numbers(&fixture->vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_color",vertex_a,5);
  call_numbers(&fixture->vm,"d3d_vertex_color",vertex_b,5);
  call_numbers(&fixture->vm,"d3d_vertex_color",vertex_c,5);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  uint32_t center=fixture->pixels[20*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu;
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<600 || ((center>>16)&255)<20 ||
     ((center>>8)&255)<20 || (center&255)<20){
    fprintf(stderr,"software D3 immediate triangle mismatch: center=%06x\n",center);
    return 0;
  }

  return 1;
}


int software3d_case_primitive_model(Software3dRasterFixture *fixture){
  const double far_depth[]={10},farther_depth[]={20},near_depth[]={-10};
  const double triangle_kind[]={4};
  uint32_t center=0;
  /* Exact cardinal rotations remain on the integer texel lattice.  Approximate libm zeros at
   * 90/180/270 degrees used to move every quadrant with a negative basis axis by one pixel. */
  {
    static const int angle[3]={90,180,270};
    static const int want[3][4]={{20,17,21,18},{27,27,28,28},{37,40,38,41}};
    for(int q=0;q<3;q++){
      memset(fixture->pixels,0,sizeof(fixture->pixels));
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      int anchor=20+q*10;
      gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,anchor,anchor,1,1,angle[q],0xFFFFFF,1);
      gml_render_flush_rotated_batch(&fixture->render);
      int count=0,minx=SOFTWARE3D_WIDTH,miny=SOFTWARE3D_HEIGHT,maxx=-1,maxy=-1;
      for(int py=0;py<SOFTWARE3D_HEIGHT;py++) for(int px=0;px<SOFTWARE3D_WIDTH;px++)
        if(fixture->pixels[py*SOFTWARE3D_WIDTH+px]&0x00FFFFFFu){
          count++;
          if(px<minx) minx=px;
          if(px>maxx) maxx=px;
          if(py<miny) miny=py;
          if(py>maxy) maxy=py;
        }
      if(count!=4 || minx!=want[q][0] || miny!=want[q][1] ||
         maxx!=want[q][2] || maxy!=want[q][3]){
        fprintf(stderr,"software modern cardinal rotation %d mismatch: pixels=%d span=(%d,%d)-(%d,%d)\n",
                angle[q],count,minx,miny,maxx,maxy);
        return 0;
      }
    }
  }
  {
    uint32_t phase[3][SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    fixture->render.classic=1;
    fixture->render.interp=0;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    memset(phase,0,sizeof(phase));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.classic_phase_y=phase[0];
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,24,20,3,3,37,0xFFFFFF,1);
    if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)==0 || colored_pixels(phase[0],SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)==0){
      fprintf(stderr,"software classic rotated sprite phase mismatch\n");
      return 0;
    }
    fixture->render.interp=1;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    memset(phase,0,sizeof(phase));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=phase[q];
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,24,20,3,3,37,0xFFFFFF,1);
    if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)==0 ||
       colored_pixels(phase[0],SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)==0 ||
       colored_pixels(phase[1],SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)==0 ||
       colored_pixels(phase[2],SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)==0){
      fprintf(stderr,"software classic rotated sprite interpolation phase mismatch\n");
      return 0;
    }
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=NULL;
    fixture->render.interp=0;
  }
  fixture->render.classic=1;
  fixture->render.tpag[0].tx=fixture->render.tpag[0].ty=1;
  fixture->render.tpag[0].bw=fixture->render.tpag[0].bh=4;
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  gml_draw_background_tiled(&fixture->render,0,0,0,1,1);
  if((fixture->pixels[1*SOFTWARE3D_WIDTH+1]&0x00FFFFFFu)==0 ||
     (fixture->pixels[2*SOFTWARE3D_WIDTH+2]&0x00FFFFFFu)==0 ||
     (fixture->pixels[1*SOFTWARE3D_WIDTH+5]&0x00FFFFFFu)==0 ||
     (fixture->pixels[2*SOFTWARE3D_WIDTH+6]&0x00FFFFFFu)==0 ||
     fixture->pixels[1*SOFTWARE3D_WIDTH+3]!=0 || fixture->pixels[1*SOFTWARE3D_WIDTH+4]!=0){
    fprintf(stderr,"software trimmed tiled background period mismatch\n");
    return 0;
  }
  fixture->render.classic=0;
  fixture->render.tpag[0].tx=fixture->render.tpag[0].ty=0;
  fixture->render.tpag[0].bw=fixture->render.tpag[0].bh=2;
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_enable,1);
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  gml_draw_background_ext(&fixture->render,0,40,10,8,8,0x0000FF,1);
  call_numbers(&fixture->vm,"d3d_set_depth",farther_depth,1);
  gml_draw_background_ext(&fixture->render,0,40,10,8,8,0x00FF00,1);
  if((fixture->pixels[16*SOFTWARE3D_WIDTH+46]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D background depth mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  gml_draw_sprite_part_ext(&fixture->render,fixture->depth_sprite,0,0,0,1,1,10,30,8,8,0x0000FF,1);
  call_numbers(&fixture->vm,"d3d_set_depth",farther_depth,1);
  gml_draw_sprite_part_ext(&fixture->render,fixture->depth_sprite,0,0,0,1,1,10,30,8,8,0x00FF00,1);
  if((fixture->pixels[34*SOFTWARE3D_WIDTH+14]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D sprite-part depth mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  gml_d3_draw_atlas_part_2d(&fixture->render,0,0,0,1,1,22,30,8,8,0x0000FF,1);
  call_numbers(&fixture->vm,"d3d_set_depth",farther_depth,1);
  gml_d3_draw_atlas_part_2d(&fixture->render,0,0,0,1,1,22,30,8,8,0x00FF00,1);
  if((fixture->pixels[34*SOFTWARE3D_WIDTH+26]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D atlas-part depth mismatch\n");
    return 0;
  }
  const double rectangle_2d[]={34,30,50,44,0};
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1); fixture->render.color=0x0000FFu;
  call_numbers(&fixture->vm,"draw_rectangle",rectangle_2d,5);
  call_numbers(&fixture->vm,"d3d_set_depth",farther_depth,1); fixture->render.color=0x00FF00u;
  call_numbers(&fixture->vm,"draw_rectangle",rectangle_2d,5);
  if((fixture->pixels[36*SOFTWARE3D_WIDTH+40]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D rectangle depth mismatch: pixel=%08x alpha=%.6f classic=%d\n",
      fixture->pixels[36*SOFTWARE3D_WIDTH+40],fixture->render.alpha,fixture->render.classic);
    return 0;
  }
  const double circle_2d[]={42,37,5,0};
  call_numbers(&fixture->vm,"d3d_set_depth",near_depth,1); fixture->render.color=0xFF0000u;
  call_numbers(&fixture->vm,"draw_circle",circle_2d,4);
  if((fixture->pixels[37*SOFTWARE3D_WIDTH+42]&0x00FFFFFFu)!=0x0000FFu){
    fprintf(stderr,"software D3 2D circle depth mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  gml_draw_surface_stretched(&fixture->render,fixture->surface,2,30,8,8,0x0000FF,1);
  call_numbers(&fixture->vm,"d3d_set_depth",farther_depth,1);
  gml_draw_surface_stretched(&fixture->render,fixture->surface,2,30,8,8,0x00FF00,1);
  if((fixture->pixels[34*SOFTWARE3D_WIDTH+6]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D surface depth mismatch\n");
    return 0;
  }
  const double draw_primitive_kind[]={4};
  const double draw_vertex_a[]={52,30,0x0000FF,1};
  const double draw_vertex_b[]={62,30,0x0000FF,1};
  const double draw_vertex_c[]={57,44,0x0000FF,1};
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  call_numbers(&fixture->vm,"draw_primitive_begin",draw_primitive_kind,1);
  call_numbers(&fixture->vm,"draw_vertex_color",draw_vertex_a,4);
  call_numbers(&fixture->vm,"draw_vertex_color",draw_vertex_b,4);
  call_numbers(&fixture->vm,"draw_vertex_color",draw_vertex_c,4);
  call_numbers(&fixture->vm,"draw_primitive_end",NULL,0);
  if((fixture->pixels[35*SOFTWARE3D_WIDTH+57]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D immediate primitive mismatch\n");
    return 0;
  }
  fixture->render.color=0xFFFFFFu;
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  const double point_kind[]={1};
  const double point_a[]={10,12,0,0x0000FF,1};
  const double point_b[]={20,14,0,0x00FF00,1};
  call_numbers(&fixture->vm,"d3d_primitive_begin",point_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_color",point_a,5);
  call_numbers(&fixture->vm,"d3d_vertex_color",point_b,5);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  if((fixture->pixels[12*SOFTWARE3D_WIDTH+10]&0x00FFFFFFu)!=0xFF0000u ||
     (fixture->pixels[14*SOFTWARE3D_WIDTH+20]&0x00FFFFFFu)!=0x00FF00u ||
     colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)!=2){
    fprintf(stderr,"software D3 immediate point-list mismatch: a=%06x b=%06x count=%d\n",
            fixture->pixels[12*SOFTWARE3D_WIDTH+10]&0x00FFFFFFu,fixture->pixels[14*SOFTWARE3D_WIDTH+20]&0x00FFFFFFu,
            colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT));
    return 0;
  }

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  const double line_kind[]={2};
  const double line_a[]={4,4,0,0x0000FF,1};
  const double line_b[]={60,40,0,0xFF0000,1};
  call_numbers(&fixture->vm,"d3d_primitive_begin",line_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_color",line_a,5);
  call_numbers(&fixture->vm,"d3d_vertex_color",line_b,5);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  center=fixture->pixels[22*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu;
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<50 || (center&255)<40 || ((center>>16)&255)<40){
    fprintf(stderr,"software D3 immediate line-list mismatch: center=%06x\n",center);
    return 0;
  }

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  const double strip_kind[]={3};
  const double strip_a[]={8,8,0,0xFFFFFF,1};
  const double strip_b[]={8,32,0,0xFFFFFF,1};
  const double strip_c[]={40,32,0,0xFFFFFF,1};
  call_numbers(&fixture->vm,"d3d_primitive_begin",strip_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_color",strip_a,5);
  call_numbers(&fixture->vm,"d3d_vertex_color",strip_b,5);
  call_numbers(&fixture->vm,"d3d_vertex_color",strip_c,5);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  if((fixture->pixels[20*SOFTWARE3D_WIDTH+8]&0x00FFFFFFu)!=0xFFFFFFu ||
     (fixture->pixels[32*SOFTWARE3D_WIDTH+24]&0x00FFFFFFu)!=0xFFFFFFu){
    fprintf(stderr,"software D3 immediate line-strip mismatch\n");
    return 0;
  }

  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  call_numbers(&fixture->vm,"d3d_set_lighting",software3d_enable,1);
  const double normal_a[]={8,8,0,0,0,1,0xFFFFFF,1};
  const double normal_b[]={56,8,0,0,0,1,0xFFFFFF,1};
  const double normal_c[]={32,40,0,0,0,1,0xFFFFFF,1};
  call_numbers(&fixture->vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_a,8);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_b,8);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_c,8);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  uint32_t dark=fixture->pixels[20*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu;
  const double directional[]={0,0,0,-1,0xFFFFFF};
  const double light_enable[]={0,1};
  call_numbers(&fixture->vm,"d3d_light_define_direction",directional,5);
  call_numbers(&fixture->vm,"d3d_light_enable",light_enable,2);
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_a,8);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_b,8);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_c,8);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  uint32_t bright=fixture->pixels[20*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu;
  int dark_sum=(dark&255)+((dark>>8)&255)+((dark>>16)&255);
  int bright_sum=(bright&255)+((bright>>8)&255)+((bright>>16)&255);
  if(dark_sum>=200 || bright_sum<700){
    fprintf(stderr,"software D3 normal lighting mismatch: dark=%06x bright=%06x\n",dark,bright);
    return 0;
  }
  const double light_disable[]={0,0};
  const double ambient_white[]={0xFFFFFF};
  call_numbers(&fixture->vm,"d3d_light_enable",light_disable,2);
  call_numbers(&fixture->vm,"d3d_light_define_ambient",ambient_white,1);
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_a,8);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_b,8);
  call_numbers(&fixture->vm,"d3d_vertex_normal_color",normal_c,8);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  uint32_t ambient=fixture->pixels[20*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu;
  int ambient_sum=(ambient&255)+((ambient>>8)&255)+((ambient>>16)&255);
  if(ambient_sum<700){
    fprintf(stderr,"software D3 ambient lighting mismatch: pixel=%06x\n",ambient);
    return 0;
  }

  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  GmlVal created=call_values(&fixture->vm,"d3d_model_create",NULL,0);
  if(created.t!=V_REAL || created.d!=0){
    fprintf(stderr,"software D3 model create mismatch\n");
    return 0;
  }
  const double model_begin[]={0,4};
  const double model_a[]={0,8,8,0,0x0000FF,1};
  const double model_b[]={0,56,8,0,0x00FF00,1};
  const double model_c[]={0,32,40,0,0xFF0000,1};
  const double model_id[]={0};
  const double model_draw[]={0,0,0,0,-1};
  call_numbers(&fixture->vm,"d3d_model_primitive_begin",model_begin,2);
  call_numbers(&fixture->vm,"d3d_model_vertex_color",model_a,6);
  call_numbers(&fixture->vm,"d3d_model_vertex_color",model_b,6);
  call_numbers(&fixture->vm,"d3d_model_vertex_color",model_c,6);
  call_numbers(&fixture->vm,"d3d_model_primitive_end",model_id,1);
  call_numbers(&fixture->vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<600){
    fprintf(stderr,"software D3 model draw mismatch\n");
    return 0;
  }
  const char *model_path="/tmp/anygm_d3_model_fixture.bin";
  GmlVal file_args[2]={vreal(0),vstr(model_path)};
  if(call_values(&fixture->vm,"d3d_model_save",file_args,2).d!=1){
    fprintf(stderr,"software D3 model save mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_model_clear",model_id,1);
  if(call_values(&fixture->vm,"d3d_model_load",file_args,2).d!=1){
    fprintf(stderr,"software D3 model load mismatch\n");
    anygm_vfs_remove(fixture->vm.host,model_path); return 0;
  }
  anygm_vfs_remove(fixture->vm.host,model_path);
  size_t model_state_size=gml_vm_state_size(&fixture->vm),model_written=0,model_used=0;
  void *model_state=malloc(model_state_size);
  if(!model_state || !gml_vm_state_save(&fixture->vm,model_state,model_state_size,&model_written)){
    fprintf(stderr,"software D3 model state save mismatch\n");
    free(model_state); return 0;
  }
  call_numbers(&fixture->vm,"d3d_model_destroy",model_id,1);
  if(!gml_vm_state_load(&fixture->vm,model_state,model_written,&model_used) || model_used!=model_written){
    fprintf(stderr,"software D3 model state load mismatch\n");
    free(model_state); return 0;
  }
  free(model_state); memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<600){
    fprintf(stderr,"software D3 restored model draw mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_model_clear",model_id,1);
  const double model_floor[]={0,8,6,0,24,18,0,1,1};
  call_numbers(&fixture->vm,"d3d_model_floor",model_floor,9);
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<150){
    fprintf(stderr,"software D3 generated model shape mismatch\n");
    return 0;
  }
  static const char legacy_model[]="100\n1\n15 8 6 0 24 18 0 1 1\n";
  if(!anygm_vfs_write_all(fixture->vm.host,model_path,legacy_model,sizeof legacy_model-1)){
    fprintf(stderr,"software D3 legacy model fixture write mismatch\n");
    return 0;
  }
  if(call_values(&fixture->vm,"d3d_model_load",file_args,2).d!=1){
    fprintf(stderr,"software D3 legacy model load mismatch\n");
    anygm_vfs_remove(fixture->vm.host,model_path); return 0;
  }
  anygm_vfs_remove(fixture->vm.host,model_path); memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<150){
    fprintf(stderr,"software D3 legacy model raster mismatch\n");
    return 0;
  }

  const double projection[]={0,-10,0, 0,0,0, 0,0,1};
  const double front_wall[]={-2,0,-2, 2,0,2, -1,1,1};
  const double back_wall[]={2,0,-2, -2,0,2, -1,1,1};
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection",projection,9);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  call_numbers(&fixture->vm,"d3d_set_culling",software3d_enable,1);
  call_numbers(&fixture->vm,"d3d_draw_wall",front_wall,9);
  int front_count=colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT);
  if(front_count<100){
    fprintf(stderr,"software D3 front face was culled\n");
    return 0;
  }

  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_draw_wall",back_wall,9);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)!=0){
    fprintf(stderr,"software D3 back face was not culled\n");
    return 0;
  }

  const double far_wall[]={-2,4,-2, 2,4,2, -1,1,1};
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection",projection,9);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_enable,1);
  call_numbers(&fixture->vm,"d3d_set_zwriteenable",software3d_disable,1);
  fixture->render.color=0x0000FFu;
  call_numbers(&fixture->vm,"d3d_draw_wall",front_wall,9);
  fixture->render.color=0x00FF00u;
  call_numbers(&fixture->vm,"d3d_draw_wall",far_wall,9);
  if((fixture->pixels[(SOFTWARE3D_HEIGHT/2)*SOFTWARE3D_WIDTH+SOFTWARE3D_WIDTH/2]&0x00FFFFFFu)!=0x00FF00u){
    fprintf(stderr,"software D3 disabled z-write mismatch\n");
    return 0;
  }
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection",projection,9);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_enable,1);
  fixture->render.color=0x0000FFu;
  call_numbers(&fixture->vm,"d3d_draw_wall",front_wall,9);
  fixture->render.color=0x00FF00u;
  call_numbers(&fixture->vm,"d3d_draw_wall",far_wall,9);
  if((fixture->pixels[(SOFTWARE3D_HEIGHT/2)*SOFTWARE3D_WIDTH+SOFTWARE3D_WIDTH/2]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 enabled z-write mismatch\n");
    return 0;
  }

  const double projection_ext[]={0,-10,0, 0,0,0, 0,0,1, 45, (double)SOFTWARE3D_WIDTH/SOFTWARE3D_HEIGHT, 1,12};
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ext",projection_ext,13);
  fixture->render.color=0xFFFFFFu;
  call_numbers(&fixture->vm,"d3d_draw_wall",far_wall,9);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)!=0){
    fprintf(stderr,"software D3 far clipping mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_draw_wall",front_wall,9);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<100){
    fprintf(stderr,"software D3 extended projection mismatch\n");
    return 0;
  }

  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection",projection,9);
  const double fog[]={1,0xFFFFFF,0,5};
  call_numbers(&fixture->vm,"d3d_set_fog",fog,4);
  fixture->render.color=0;
  call_numbers(&fixture->vm,"d3d_draw_wall",front_wall,9);
  if((fixture->pixels[(SOFTWARE3D_HEIGHT/2)*SOFTWARE3D_WIDTH+SOFTWARE3D_WIDTH/2]&0x00FFFFFFu)!=0xFFFFFFu){
    fprintf(stderr,"software D3 fog mismatch: pixel=%08x\n",fixture->pixels[(SOFTWARE3D_HEIGHT/2)*SOFTWARE3D_WIDTH+SOFTWARE3D_WIDTH/2]);
    return 0;
  }

  {
    uint32_t phase[3][SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    int phase_surface=gml_surface_create(&fixture->render,2,2);
    if(phase_surface<=0){
      fprintf(stderr,"software classic interpolation surface create mismatch\n");
      return 0;
    }
    GmlSurface *phase_surface_data=&fixture->render.surface[phase_surface-1];
    memset(fixture->pixels,0,sizeof(fixture->pixels)); memset(phase,0,sizeof(phase));
    phase_surface_data->px[0]=0xFF7F405Fu; phase_surface_data->px[1]=0xFFA060E0u;
    phase_surface_data->px[2]=0xFF20C0FFu; phase_surface_data->px[3]=0xFFFF0010u;
    phase_surface_data->dirty=1; phase_surface_data->opaque_known=0;
    phase_surface_data->all_opaque=0; phase_surface_data->all_transparent=0;
    gml_vm_software3d_reset(&fixture->vm); fixture->render.classic=1; fixture->render.interp=1;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=phase[q];
    gml_draw_surface_stretched(&fixture->render,phase_surface,8,8,2,2,0xFFFFFF,1);
    if(fixture->pixels[8*SOFTWARE3D_WIDTH+8]!=0xFF7F405Fu ||
       phase[0][8*SOFTWARE3D_WIDTH+7]!=0xFF7F405Fu || phase[0][8*SOFTWARE3D_WIDTH+8]!=0xFF8F509Fu ||
       phase[1][7*SOFTWARE3D_WIDTH+8]!=0xFF7F405Fu || phase[1][8*SOFTWARE3D_WIDTH+8]!=0xFF4F80AFu ||
       phase[2][7*SOFTWARE3D_WIDTH+7]!=0xFF7F405Fu || phase[2][8*SOFTWARE3D_WIDTH+8]!=0xFF8F5893u){
      fprintf(stderr,"software classic surface interpolation phase mismatch: %08x %08x %08x %08x %08x %08x %08x\n",
              fixture->pixels[8*SOFTWARE3D_WIDTH+8],phase[0][8*SOFTWARE3D_WIDTH+7],phase[0][8*SOFTWARE3D_WIDTH+8],
              phase[1][7*SOFTWARE3D_WIDTH+8],phase[1][8*SOFTWARE3D_WIDTH+8],
              phase[2][7*SOFTWARE3D_WIDTH+7],phase[2][8*SOFTWARE3D_WIDTH+8]);
      return 0;
    }
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=NULL;
    fixture->render.classic=0; fixture->render.interp=0;
  }

  {
    int multiply_surface=gml_surface_create(&fixture->render,2,2);
    if(multiply_surface<=0){
      fprintf(stderr,"software rotated multiply surface create mismatch\n");
      return 0;
    }
    GmlSurface *multiply_data=&fixture->render.surface[multiply_surface-1];
    for(int i=0;i<4;i++) multiply_data->px[i]=0xFF808080u;
    multiply_data->dirty=1; multiply_data->opaque_known=0;
    multiply_data->all_opaque=0; multiply_data->all_transparent=0;
    gml_vm_software3d_reset(&fixture->vm);
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFFC08040u;
    fixture->render.blendmode=3;
    GmlVal surface_ext_args[8]={vreal(multiply_surface),vreal(20),vreal(20),vreal(4),
      vreal(4),vreal(90),vreal(0xFFFFFF),vreal(1)};
    int surface_ext_id=gml_builtin_fast_id(&fixture->vm,"draw_surface_ext");
    (void)gml_builtin_call_fast_id(&fixture->vm,surface_ext_id,"draw_surface_ext",surface_ext_args,8);
    if(fixture->pixels[18*SOFTWARE3D_WIDTH+22]!=0xFF604020u ||
       fixture->pixels[22*SOFTWARE3D_WIDTH+22]!=0xFFC08040u || surface_ext_id<0){
      fprintf(stderr,"software rotated multiply surface mismatch: %08x %08x\n",
              fixture->pixels[18*SOFTWARE3D_WIDTH+22],fixture->pixels[22*SOFTWARE3D_WIDTH+22]);
      return 0;
    }
    fixture->render.blendmode=0;
  }

  {
    gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    call_numbers(&fixture->vm,"vertex_format_begin",NULL,0);
    call_numbers(&fixture->vm,"vertex_format_add_position",NULL,0);
    call_numbers(&fixture->vm,"vertex_format_add_colour",NULL,0);
    const double custom_format[]={2,2};
    call_numbers(&fixture->vm,"vertex_format_add_custom",custom_format,2);
    GmlVal format=call_values(&fixture->vm,"vertex_format_end",NULL,0);
    GmlVal buffer=call_values(&fixture->vm,"vertex_create_buffer",NULL,0);
    if(format.t!=V_REAL || format.d!=0 || buffer.t!=V_REAL || buffer.d!=0){
      fprintf(stderr,"software vertex resource allocation mismatch\n");
      return 0;
    }
    const double begin[]={0,0};
    const double position_a[]={0,0,0},position_b[]={0,48,0},position_c[]={0,24,32};
    const double color_red[]={0,0x0000FF,1};
    /* The low colour bits put red in the low byte and coverage in the top byte, so an
     * opaque red vertex colour uses 0xFF0000FF. */
    const double argb_red[]={0,4278190335.0};
    const double custom_a[]={0,-3.25,3.25};
    const double custom_b[]={0,-9.5,9.5};
    call_numbers(&fixture->vm,"vertex_begin",begin,2);
    call_numbers(&fixture->vm,"vertex_position",position_a,3); call_numbers(&fixture->vm,"vertex_argb",argb_red,2); call_numbers(&fixture->vm,"vertex_float2",custom_a,3);
    call_numbers(&fixture->vm,"vertex_position",position_b,3); call_numbers(&fixture->vm,"vertex_colour",color_red,3); call_numbers(&fixture->vm,"vertex_float2",custom_b,3);
    call_numbers(&fixture->vm,"vertex_position",position_c,3); call_numbers(&fixture->vm,"vertex_argb",argb_red,2); call_numbers(&fixture->vm,"vertex_float2",custom_a,3);
    const double buffer_id[]={0};
    call_numbers(&fixture->vm,"vertex_end",buffer_id,1);
    GmlVal vertex_count=call_values(&fixture->vm,"vertex_get_number",(GmlVal[]){vreal(0)},1);
    GmlVal vertex_bytes=call_values(&fixture->vm,"vertex_get_buffer_size",(GmlVal[]){vreal(0)},1);
    GmlVal world=call_values(&fixture->vm,"matrix_build_identity",NULL,0);
    gml_arr_set(world,12,vreal(8)); gml_arr_set(world,13,vreal(8));
    GmlVal world_set[2]={vreal(2),world};
    call_values(&fixture->vm,"matrix_set",world_set,2);
    const double submit[]={0,4,-1};
    call_numbers(&fixture->vm,"vertex_submit",submit,3);
    if(vertex_count.t!=V_REAL || vertex_count.d!=3 || vertex_bytes.t!=V_REAL || vertex_bytes.d!=60 ||
       (fixture->pixels[20*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu)!=0xFF0000u ||
       fixture->pixels[2*SOFTWARE3D_WIDTH+24]!=0){
      fprintf(stderr,"software vertex buffer triangle mismatch: count=%g bytes=%g pixel=%08x\n",
              vertex_count.d,vertex_bytes.d,fixture->pixels[20*SOFTWARE3D_WIDTH+32]);
      return 0;
    }
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_render_world_set_logical_extent(
      &fixture->render,SOFTWARE3D_WIDTH/2,SOFTWARE3D_HEIGHT/2);
    call_values(&fixture->vm,"matrix_set",world_set,2);
    call_numbers(&fixture->vm,"vertex_submit",submit,3);
    if((fixture->pixels[32*SOFTWARE3D_WIDTH+32]&0x00FFFFFFu)!=0xFF0000u ||
       (fixture->pixels[16*SOFTWARE3D_WIDTH+16]&0x00FFFFFFu)!=0xFF0000u ||
       fixture->pixels[8*SOFTWARE3D_WIDTH+8]!=0){
      int min_x=SOFTWARE3D_WIDTH,max_x=-1,min_y=SOFTWARE3D_HEIGHT,max_y=-1;
      for(int y=0;y<SOFTWARE3D_HEIGHT;y++) for(int x=0;x<SOFTWARE3D_WIDTH;x++)
        if(fixture->pixels[y*SOFTWARE3D_WIDTH+x]&0x00FFFFFFu){
          if(x<min_x) min_x=x;
          if(x>max_x) max_x=x;
          if(y<min_y) min_y=y;
          if(y>max_y) max_y=y;
        }
      int row_min=SOFTWARE3D_WIDTH,row_max=-1;
      for(int x=0;x<SOFTWARE3D_WIDTH;x++)
        if(fixture->pixels[32*SOFTWARE3D_WIDTH+x]&0x00FFFFFFu){
          if(x<row_min) row_min=x;
          if(x>row_max) row_max=x;
        }
      fprintf(stderr,"scaled software vertex buffer triangle mismatch: %08x %08x %08x span=(%d,%d)-(%d,%d) row32=%d..%d\n",
              fixture->pixels[32*SOFTWARE3D_WIDTH+32],
              fixture->pixels[16*SOFTWARE3D_WIDTH+16],
              fixture->pixels[8*SOFTWARE3D_WIDTH+8],
              min_x,min_y,max_x,max_y,row_min,row_max);
      return 0;
    }
    GmlVal identity=call_values(&fixture->vm,"matrix_build_identity",NULL,0);
    GmlVal identity_set[2]={vreal(2),identity};
    call_values(&fixture->vm,"matrix_set",identity_set,2);
    GmlVal frozen=call_values(&fixture->vm,"vertex_freeze",(GmlVal[]){vreal(0)},1);
    GmlVal rejected=call_values(&fixture->vm,"vertex_begin",(GmlVal[]){vreal(0),vreal(0)},2);
    if(frozen.t!=V_REAL || frozen.d!=0 || rejected.t!=V_REAL || rejected.d!=-1){
      fprintf(stderr,"software vertex buffer freeze mismatch\n");
      return 0;
    }
    call_numbers(&fixture->vm,"vertex_delete_buffer",buffer_id,1);
    call_numbers(&fixture->vm,"vertex_format_delete",buffer_id,1);
  }

  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection",projection,9);
  fixture->render.color=0xFFFFFFu;
  const double cone[]={-2,0,-2, 2,4,2, -1,1,1,1,12};
  call_numbers(&fixture->vm,"d3d_draw_cone",cone,11);
  if(colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)<50){
    fprintf(stderr,"software D3 cone raster mismatch\n");
    return 0;
  }
  return 1;
}
