/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"


int software3d_operation_boundary_fixture(void){
  GmlWin win={0};
  GmlRender render;
  GmlSoftware3D *graphics=gml_software3d_create();
  GmlSoftware3DStatus status={0};
  GmlSoftware3DControl control={0};
  double identity[16],translation[16],scaling[16],actual[16];
  uint32_t pixels[16*16]={0};
  int format=-1,buffer=-1,model=-1,vertices=-1,batches=-1;
  int ok=0;
  if(!graphics || gml_render_init(&render,&win)!=0){
    fprintf(stderr,"software-3D operation boundary initialization failed\n");
    gml_software3d_destroy(graphics);
    return 0;
  }
  gml_software3d_reset(graphics);
  gml_render_bind_software3d(&render,graphics);
  gml_render_begin(&render,pixels,16,16,0,0);
  if(!gml_software3d_status_get(graphics,&status) || status.active){
    fprintf(stderr,"software-3D status snapshot mismatch\n");
    goto done;
  }
  control.hidden=1;
  control.zwrite=0;
  control.smooth=0;
  control.fog=1;
  control.fog_start=20;
  control.fog_end=5;
  control.fog_color=0x123456u;
  control.culling=1;
  control.lighting=1;
  control.ambient_color=0x654321u;
  control.fov=70;
  control.aspect=1.5;
  control.near_clip=.5;
  control.far_clip=500;
  control.draw_depth=7;
  gml_software3d_control_update(
    graphics,&control,
    GML_SOFTWARE3D_CONTROL_HIDDEN|GML_SOFTWARE3D_CONTROL_ZWRITE|
    GML_SOFTWARE3D_CONTROL_SMOOTH|GML_SOFTWARE3D_CONTROL_FOG|
    GML_SOFTWARE3D_CONTROL_CULLING|GML_SOFTWARE3D_CONTROL_LIGHTING|
    GML_SOFTWARE3D_CONTROL_AMBIENT|GML_SOFTWARE3D_CONTROL_FOV|
    GML_SOFTWARE3D_CONTROL_CLIP|GML_SOFTWARE3D_CONTROL_DEPTH);
  if(!gml_software3d_light_define_point(graphics,0,1,2,3,4,0x010203u) ||
     !gml_software3d_light_define_direction(graphics,1,4,5,6,0x040506u) ||
     !gml_software3d_light_enable(graphics,0,1) ||
     gml_software3d_light_enable(graphics,8,1)){
    fprintf(stderr,"software-3D opaque light operations mismatch\n");
    goto done;
  }
  gml_software3d_begin(&render,0,0,16,16,0);
  if(!gml_software3d_status_get(graphics,&status) || !status.active){
    fprintf(stderr,"software-3D begin status mismatch\n");
    goto done;
  }
  gml_software3d_end(graphics);
  if(!gml_software3d_status_get(graphics,&status) || status.active){
    fprintf(stderr,"software-3D end status mismatch\n");
    goto done;
  }
  gml_software3d_matrix_identity(identity);
  if(!gml_software3d_matrix_get(graphics,GML_SOFTWARE3D_MATRIX_TRANSFORM,actual) ||
     memcmp(actual,identity,sizeof(actual))){
    fprintf(stderr,"software-3D matrix value-copy mismatch\n");
    goto done;
  }
  gml_software3d_matrix_translation(translation,3,4,5);
  gml_software3d_matrix_scaling(scaling,2,3,4);
  if(!gml_software3d_matrix_set(&render,GML_SOFTWARE3D_MATRIX_TRANSFORM,translation) ||
     !gml_software3d_transform_stack(&render,GML_SOFTWARE3D_STACK_PUSH) ||
     !gml_software3d_matrix_set(&render,GML_SOFTWARE3D_MATRIX_TRANSFORM,scaling) ||
     !gml_software3d_transform_stack(&render,GML_SOFTWARE3D_STACK_TOP) ||
     !gml_software3d_matrix_get(graphics,GML_SOFTWARE3D_MATRIX_TRANSFORM,actual) ||
     memcmp(actual,translation,sizeof(actual)) ||
     !gml_software3d_transform_stack(&render,GML_SOFTWARE3D_STACK_DISCARD) ||
     !gml_software3d_transform_stack(&render,GML_SOFTWARE3D_STACK_EMPTY)){
    fprintf(stderr,"software-3D matrix-stack operation mismatch\n");
    goto done;
  }
  gml_software3d_vertex_format_begin(graphics);
  if(!gml_software3d_vertex_format_add(
       graphics,GML_SOFTWARE3D_VERTEX_POSITION3,0,0,3,12) ||
     (format=gml_software3d_vertex_format_finish(graphics))<0 ||
     (buffer=gml_software3d_vertex_buffer_create(graphics,12))<0 ||
     !gml_software3d_vertex_buffer_begin(graphics,buffer,format)){
    fprintf(stderr,"software-3D opaque vertex-resource setup mismatch\n");
    goto done;
  }
  const double position[4]={1,2,3,0};
  if(!gml_software3d_vertex_buffer_attribute(
       graphics,buffer,GML_SOFTWARE3D_VERTEX_POSITION3,position,3) ||
     gml_software3d_vertex_buffer_number(graphics,buffer)!=1 ||
     gml_software3d_vertex_buffer_size(graphics,buffer)!=12 ||
     !gml_software3d_vertex_buffer_freeze(graphics,buffer)){
    fprintf(stderr,"software-3D opaque vertex-resource query mismatch\n");
    goto done;
  }
  model=gml_software3d_model_create(graphics);
  GmlSoftware3DVertex supplied={
    .x=1,.y=2,.z=3,.r=17,.g=34,.b=51,.alpha=.5
  },copied={0};
  GmlSoftware3DBatch batch={0};
  if(model<0 || !gml_software3d_model_begin(graphics,model,4) ||
     !gml_software3d_model_append_vertex(graphics,model,supplied) ||
     !gml_software3d_model_end(graphics,model) ||
     !gml_software3d_model_info(graphics,model,&vertices,&batches) ||
     vertices!=1 || batches!=1 ||
     !gml_software3d_model_batch_get(graphics,model,0,&batch) ||
     batch.kind!=4 || batch.first!=0 || batch.count!=1 ||
     !gml_software3d_model_vertex_get(graphics,model,0,&copied) ||
     copied.x!=supplied.x || copied.y!=supplied.y || copied.z!=supplied.z ||
     copied.r!=supplied.r || copied.g!=supplied.g || copied.b!=supplied.b ||
     copied.alpha!=supplied.alpha ||
     !gml_software3d_model_clear(graphics,model) ||
     !gml_software3d_model_info(graphics,model,&vertices,&batches) ||
     vertices!=0 || batches!=0 ||
     !gml_software3d_model_destroy(graphics,model) ||
     gml_software3d_model_is_live(graphics,model)){
    fprintf(stderr,"software-3D opaque model operations mismatch\n");
    goto done;
  }
  ok=1;
done:
  if(buffer>=0) gml_software3d_vertex_buffer_delete(graphics,buffer);
  if(format>=0) gml_software3d_vertex_format_delete(graphics,format);
  if(model>=0 && gml_software3d_model_is_live(graphics,model))
    gml_software3d_model_destroy(graphics,model);
  gml_render_bind_software3d(&render,NULL);
  gml_render_free(&render);
  gml_software3d_destroy(graphics);
  return ok;
}


int renderer_state_boundary_fixture(void){
  GmlWin win={0};
  GmlRender render;
  GmlRenderDrawState requested={0},actual={0},temporary={0};
  GmlRenderTargetMetrics target={0};
  GmlRenderPresentationMetrics presentation={0};
  uint32_t pixels[12]={0},pixel=0;
  if(gml_render_init(&render,&win)!=0){
    fprintf(stderr,"renderer state boundary initialization failed\n");
    return 0;
  }
  requested.color=0x123456u; requested.alpha=.375; requested.font=3;
  requested.horizontal_alignment=1; requested.vertical_alignment=2;
  requested.alpha_blend=0; requested.circle_precision=36; requested.interpolation=1;
  requested.blend_mode=2; requested.blend_equation=4; requested.blend_equation_alpha=5;
  requested.alpha_test_enable=1; requested.alpha_test_reference=123;
  requested.color_write_mask=5;
  gml_render_draw_state_update(&render,&requested,
    GML_RENDER_DRAW_STATE_COLOR|GML_RENDER_DRAW_STATE_ALPHA|
    GML_RENDER_DRAW_STATE_FONT|GML_RENDER_DRAW_STATE_HORIZONTAL_ALIGNMENT|
    GML_RENDER_DRAW_STATE_VERTICAL_ALIGNMENT|GML_RENDER_DRAW_STATE_ALPHA_BLEND|
    GML_RENDER_DRAW_STATE_CIRCLE_PRECISION|GML_RENDER_DRAW_STATE_INTERPOLATION|
    GML_RENDER_DRAW_STATE_BLEND_MODE|GML_RENDER_DRAW_STATE_BLEND_EQUATION|
    GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA|GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE|
    GML_RENDER_DRAW_STATE_ALPHA_TEST_REFERENCE|GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK);
  if(!gml_render_draw_state_get(&render,&actual) ||
     actual.color!=requested.color || actual.alpha!=requested.alpha ||
     actual.font!=requested.font ||
     actual.horizontal_alignment!=requested.horizontal_alignment ||
     actual.vertical_alignment!=requested.vertical_alignment ||
     actual.alpha_blend!=requested.alpha_blend ||
     actual.circle_precision!=requested.circle_precision ||
     actual.interpolation!=requested.interpolation ||
     actual.blend_mode!=requested.blend_mode ||
     actual.blend_equation!=requested.blend_equation ||
     actual.blend_equation_alpha!=requested.blend_equation_alpha ||
     actual.alpha_test_enable!=requested.alpha_test_enable ||
     actual.alpha_test_reference!=requested.alpha_test_reference ||
     actual.color_write_mask!=requested.color_write_mask){
    fprintf(stderr,"renderer draw-state value-copy mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  if(!gml_render_gpu_state_push(&render)){
    fprintf(stderr,"renderer GPU-state push failed\n");
    gml_render_free(&render);
    return 0;
  }
  temporary.alpha_blend=1; temporary.interpolation=0; temporary.blend_mode=0;
  temporary.alpha_test_enable=0; temporary.alpha_test_reference=7;
  temporary.color_write_mask=15;
  gml_render_draw_state_update(&render,&temporary,
    GML_RENDER_DRAW_STATE_ALPHA_BLEND|GML_RENDER_DRAW_STATE_INTERPOLATION|
    GML_RENDER_DRAW_STATE_BLEND_MODE|GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE|
    GML_RENDER_DRAW_STATE_ALPHA_TEST_REFERENCE|GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK);
  if(!gml_render_gpu_state_pop(&render) ||
     !gml_render_draw_state_get(&render,&actual) ||
     actual.alpha_blend!=requested.alpha_blend ||
     actual.interpolation!=requested.interpolation ||
     actual.blend_mode!=requested.blend_mode ||
     actual.alpha_test_enable!=requested.alpha_test_enable ||
     actual.alpha_test_reference!=requested.alpha_test_reference ||
     actual.color_write_mask!=requested.color_write_mask){
    fprintf(stderr,"renderer GPU-state stack roundtrip mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  requested.alpha=1.0; requested.alpha_blend=0;
  gml_render_draw_state_update(&render,&requested,
    GML_RENDER_DRAW_STATE_ALPHA|GML_RENDER_DRAW_STATE_ALPHA_BLEND);
  gml_render_begin(&render,pixels,4,3,2.5,-1.5);
  gml_render_clear(&render,0x445566u,1);
  gml_render_primitive_point(&render,1,2,0x112233u);
  if(!gml_render_target_metrics(&render,&target) ||
     target.width!=4 || target.height!=3 ||
     target.camera_x!=2.5 || target.camera_y!=-1.5 ||
     !gml_render_target_pixel(&render,1,2,&pixel) || pixel!=0xFF332211u ||
     !gml_render_target_pixel(&render,0,0,&pixel) || pixel!=0xFF665544u){
    fprintf(stderr,"renderer target snapshot or point operation mismatch: %08x\n",pixel);
    gml_render_free(&render);
    return 0;
  }
  if(!gml_render_presentation_metrics(&render,&presentation) ||
     presentation.gui_pass_active){
    fprintf(stderr,"renderer presentation snapshot mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  gml_render_shader_set_current(&render,7);
  gml_render_application_surface_set_draw_enabled(&render,0);
  if(gml_render_shader_current(&render)!=7){
    fprintf(stderr,"renderer shader control boundary mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  uint8_t *rgba=malloc(4);
  if(!rgba){
    fprintf(stderr,"renderer asset-boundary allocation failed\n");
    gml_render_free(&render);
    return 0;
  }
  rgba[0]=17; rgba[1]=34; rgba[2]=51; rgba[3]=255;
  int sprite_id=gml_sprite_append_from_rgba(&render,rgba,1,1,2,3,
                                            "neutral_runtime_sprite");
  GmlRenderSpriteMetrics sprite;
  if(sprite_id<0 ||
     !gml_render_sprite_metrics(&render,sprite_id,&sprite) ||
     sprite.width!=1 || sprite.height!=1 || sprite.origin_x!=2 ||
     sprite.origin_y!=3 || sprite.frame_count!=1 ||
     !sprite.name || strcmp(sprite.name,"neutral_runtime_sprite") ||
     gml_render_named_sprite(&render,"neutral_runtime_sprite")!=sprite_id ||
     !gml_render_sprite_set_playback(&render,sprite_id,2.5,0) ||
     !gml_render_sprite_metrics(&render,sprite_id,&sprite) ||
     !sprite.playback_speed_valid || sprite.playback_speed!=2.5 ||
     sprite.playback_speed_type!=0){
    fprintf(stderr,"renderer sprite metadata boundary mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  int font_id=gml_font_add_sprite(&render,sprite_id,32,1,2);
  GmlRenderFontMetrics font;
  if(font_id<0 || !gml_render_font_metrics(&render,font_id,&font) ||
     !font.sprite_backed || font.sprite!=sprite_id || font.first!=32 ||
     font.proportional!=1 || font.separation!=2 ||
     gml_render_background_texture_handle(&render,0)!=-1){
    fprintf(stderr,"renderer font or background metadata boundary mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  render.shader_pal=calloc(1,sizeof(*render.shader_pal));
  render.n_shader_pal=render.shader_pal?1:0;
  if(!render.shader_pal){
    fprintf(stderr,"renderer shader-boundary allocation failed\n");
    gml_render_free(&render);
    return 0;
  }
  struct GmlShaderPal *shader=&render.shader_pal[0];
  shader->lut=1;
  snprintf(shader->lut_row_uniform,sizeof(shader->lut_row_uniform),"u_control");
  int uniform=gml_render_shader_uniform_handle(&render,0,"u_control");
  double uniform_values[4]={.25,.5,.75,1.0};
  gml_render_shader_uniform_set(&render,uniform,uniform_values);
  GmlRenderShaderTextureBinding binding;
  GmlRenderTextureMetrics texture_metrics={0};
  int texture=gml_render_sprite_texture_handle(sprite_id,0);
  if(!gml_render_texture_metrics(&render,texture,&texture_metrics) ||
     texture_metrics.kind!=GML_RENDER_TEXTURE_SPRITE ||
     !texture_metrics.runtime || texture_metrics.logical_width!=1 ||
     texture_metrics.logical_height!=1 ||
     uniform!=1 || shader->lut_row!=.25f ||
     gml_render_shader_uniform_handle(&render,0,"u_ignored")!=63 ||
     !gml_render_shader_is_compiled(&render,0) ||
     !gml_render_shader_texture_stage_set(&render,uniform,texture,&binding) ||
     binding.kind!=GML_RENDER_SHADER_TEXTURE_PALETTE ||
     binding.sprite!=sprite_id || binding.frame!=0){
    fprintf(stderr,"renderer shader uniform or palette-stage boundary mismatch\n");
    gml_render_free(&render);
    return 0;
  }
  gml_render_free(&render);
  return 1;
}


static int state_matches(GmlVM *vm,const int expected_flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
                         const double expected_values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
                         const uint32_t expected_colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]){
  int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT];
  double values[GML_SOFTWARE3D_STATE_VALUE_COUNT];
  uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT];
  gml_vm_software3d_state_get(vm,flags,values,colors);
  return !memcmp(flags,expected_flags,sizeof(flags)) &&
         !memcmp(values,expected_values,sizeof(values)) &&
         !memcmp(colors,expected_colors,sizeof(colors));
}


static int software3d_state_roundtrip_exit_code(void){
  GmlWin win; GmlVM vm;
  memset(&win,0,sizeof(win)); memset(&vm,0,sizeof(vm)); vm.win=&win;
  int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT]={0};
  double values[GML_SOFTWARE3D_STATE_VALUE_COUNT]={0};
  uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]={0};
  flags[0]=1; flags[1]=1; flags[2]=1; flags[19]=1; flags[20]=1;
  for(int i=0;i<8;i++){
    flags[3+i*2]=1; flags[4+i*2]=(i&1)==0;
    colors[i]=0x010203u*(uint32_t)(i+1);
  }
  colors[8]=0x123456u;
  for(int i=0;i<GML_SOFTWARE3D_STATE_VALUE_COUNT;i++) values[i]=(double)(i+1)*1.25;
  values[44]=-13.5; values[45]=27.25; values[46]=640; values[47]=360; values[48]=33;
  gml_vm_software3d_state_set(&vm,flags,values,colors);
  if(!state_matches(&vm,flags,values,colors)){
    fprintf(stderr,"software D3 direct state mismatch\n");
    return 1;
  }
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles){
    fprintf(stderr,"software D3 particle state allocation failed\n");
    return 1;
  }
  vm.classic_info_active=1;
  vm.inst=calloc(1,sizeof(*vm.inst));
  if(!vm.inst){ fprintf(stderr,"software D3 instance allocation failed\n"); return 1; }
  vm.inst_cap=vm.inst_count=1;
  vm.inst[0].active=1; vm.inst[0].id=100000; vm.inst[0].obj=-1; vm.inst[0].room_owner=-1;
  vm.inst[0].mask_index=-1; vm.inst[0].image_speed=1;
  vm.inst[0].image_xscale=vm.inst[0].image_yscale=vm.inst[0].image_alpha=1;
  vm.inst[0].image_blend=16777215; vm.inst[0].visible=1; vm.inst[0].gravity_direction=270;
  vm.inst[0].path_index=-1; vm.inst[0].path_scale=1;
  vm.inst[0].timeline_index=-1; vm.inst[0].timeline_speed=1;
  vm.inst[0].draw_layer_order=7;
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  void *state=malloc(size);
  if(!state || !gml_vm_state_save(&vm,state,size,&written) || written!=size){
    fprintf(stderr,"software D3 state save failed\n");
    free(state); return 1;
  }
  void *repeated_state=malloc(size);
  size_t repeated_written=0;
  if(!repeated_state ||
     !gml_vm_state_save(&vm,repeated_state,size,&repeated_written) ||
     repeated_written!=written || memcmp(repeated_state,state,written)){
    fprintf(stderr,"repeated software D3 serialization was not canonical\n");
    free(repeated_state);
    free(state);
    return 1;
  }
  free(repeated_state);
  vm.classic_info_active=0;
  gml_vm_software3d_reset(&vm);
  if(state_matches(&vm,flags,values,colors)){
    fprintf(stderr,"software D3 reset did not clear state\n");
    free(state); return 1;
  }
  if(!gml_vm_state_load(&vm,state,written,&used) || used!=written ||
     !vm.classic_info_active ||
     vm.inst_count!=1 || vm.inst[0].draw_layer_order!=7 ||
     !state_matches(&vm,flags,values,colors)){
    fprintf(stderr,"software D3 savestate roundtrip mismatch\n");
    free(state); return 1;
  }
  free(state);
  gml_vm_free(&vm);
  return 0;
}


int software3d_state_roundtrip_case(void){
  return software3d_state_roundtrip_exit_code()==0;
}

