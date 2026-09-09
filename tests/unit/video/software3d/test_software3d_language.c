/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"

static int matrix_array_matches(GmlVal value,const double expected[16]){
  if(value.t!=V_ARR || gml_val_array_length(value)<16) return 0;
  for(int i=0;i<16;i++){
    GmlVal item=gml_arr_get(value,i);
    if(item.t!=V_REAL || !isfinite(item.d) ||
       fabs(item.d-expected[i])>1e-12*fmax(1.0,fabs(expected[i]))) return 0;
  }
  return 1;
}

int matrix_inverse_fixture(void){
  GmlVM vm={0};
  GmlVal roots[4]={gml_arr_new(16,vreal(0)),vundef(),
                   gml_arr_new(18,vreal(23)),vundef()};
  const double transform[16]={2,0,0,0, 0,4,0,0, 0,0,8,0, 6,-12,16,1};
  const double inverse[16]={.5,0,0,0, 0,.25,0,0, 0,0,.125,0, -3,3,-2,1};
  const double permutation[16]={0,0,0,1, 0,0,1,0, 0,1,0,0, 1,0,0,0};
  int passed=0;
#define MATRIX_REQUIRE(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"matrix inverse: %s\n",label); goto done; } } while(0)
  for(int i=0;i<16;i++) gml_arr_set(roots[0],i,vreal(transform[i]));
  roots[1]=call_values(&vm,"matrix_inverse",roots,1);
  MATRIX_REQUIRE(matrix_array_matches(roots[1],inverse),"hand-derived affine inverse");
  MATRIX_REQUIRE(matrix_array_matches(roots[0],transform),"source is unchanged");
  GmlVal reuse_args[2]={roots[0],roots[2]};
  roots[3]=call_values(&vm,"matrix_inverse",reuse_args,2);
  MATRIX_REQUIRE(roots[3].t==V_ARR && roots[3].arr==roots[2].arr &&
    matrix_array_matches(roots[2],inverse) && gml_arr_get(roots[2],17).d==23,
    "reuse writes only the sixteen result elements");
  GmlVal alias_args[2]={roots[0],roots[0]};
  (void)call_values(&vm,"matrix_inverse",alias_args,2);
  MATRIX_REQUIRE(matrix_array_matches(roots[0],inverse),"input may alias output");
  for(int i=0;i<16;i++) gml_arr_set(roots[0],i,vreal(permutation[i]));
  (void)call_values(&vm,"matrix_inverse",alias_args,2);
  MATRIX_REQUIRE(matrix_array_matches(roots[0],permutation),"non-affine pivot swaps");
  for(int i=0;i<16;i++) gml_arr_set(roots[0],i,vreal(0));
  MATRIX_REQUIRE(call_values(&vm,"matrix_inverse",roots,1).t==V_UNDEF,
    "singular input has no inverse");
  MATRIX_REQUIRE(call_values(&vm,"matrix_inverse",reuse_args,2).t==V_UNDEF &&
    matrix_array_matches(roots[2],inverse),"singular reuse is transactional");
  for(int i=0;i<16;i++) gml_arr_set(roots[0],i,vreal(i%5==0?1:0));
  gml_arr_set(roots[0],0,vreal(1e-200));
  (void)call_values(&vm,"matrix_inverse",alias_args,2);
  MATRIX_REQUIRE(fabs(gml_arr_get(roots[0],0).d/1e200-1)<1e-12,
    "small nonzero pivots are not rejected by a fixed epsilon");
  const double invalid[]={NAN,INFINITY,1e-320};
  for(size_t k=0;k<sizeof invalid/sizeof invalid[0];k++){
    gml_arr_set(roots[0],0,vreal(invalid[k]));
    MATRIX_REQUIRE(call_values(&vm,"matrix_inverse",reuse_args,2).t==V_UNDEF &&
      matrix_array_matches(roots[2],inverse),"invalid or unrepresentable inverse is transactional");
  }
  MATRIX_REQUIRE(call_values(&vm,"matrix_inverse",NULL,0).t==V_UNDEF,
    "missing matrix is rejected");
  passed=1;
done:
  gml_values_release(roots,sizeof roots/sizeof roots[0]);
  gml_vm_free(&vm);
  return passed;
#undef MATRIX_REQUIRE
}


int software3d_case_language(Software3dRasterFixture *fixture){
  if(!pushref_function_fixture()) return 0;
  if(!variable_hash_reference_fixture()) return 0;
  if(!member_function_self_fixture()) return 0;
  if(!member_function_argument_fixture()) return 0;

  if(call_values(&fixture->vm,"gc_collect",NULL,0).t!=V_REAL ||
     call_values(&fixture->vm,"gc_is_enabled",NULL,0).d!=1){
    fprintf(stderr,"explicit garbage-collection API mismatch\n");
    return 0;
  }

  {
    GmlVal number=vreal(7), string=vstr("seven"), array=gml_arr_new(2,vreal(0));
    GmlVal undefined=vundef();
    GmlInstance *plain=gml_struct_new(&fixture->vm), *method=gml_struct_new(&fixture->vm);
    if(!plain || !method){
      fprintf(stderr,"typeof struct fixture allocation failed\n");
      return 0;
    }
    method->method_bound=1;
    *gml_varmap_put(&method->vars,"__fn")=vreal((double)(GML_FUNCVAL_TAG|3));
    GmlVal values_to_test[]={number,string,array,undefined,vreal((double)plain->id),
                             vreal((double)method->id)};
    const char *expected[]={"number","string","array","undefined","struct","method"};
    for(int i=0;i<6;i++){
      GmlVal actual=call_values(&fixture->vm,"typeof",&values_to_test[i],1);
      if(actual.t!=V_STR || strcmp(actual.s?actual.s:"",expected[i])){
        fprintf(stderr,"typeof fixture %d mismatch: got %s expected %s\n",i,
                actual.t==V_STR&&actual.s?actual.s:"<non-string>",expected[i]);
        return 0;
      }
    }
  }

  {
    GmlVM network_vm={0};
    AnygmHostServices services={0};
    services.struct_size=sizeof services;
    services.abi_version=ANYGM_HOST_SERVICES_VERSION;
    services.capability=fixture_capability;
    network_vm.host=&services;
    fixture_network_connected=0;
    GmlVal offline=call_values(&network_vm,"os_is_network_connected",NULL,0);
    fixture_network_connected=1;
    GmlVal online=call_values(&network_vm,"os_is_network_connected",NULL,0);
    gml_builtin_state_destroy(network_vm.builtins);
    gml_software3d_destroy(network_vm.software3d);
    if(offline.t!=V_REAL || offline.d!=0 || online.t!=V_REAL || online.d!=1){
      fprintf(stderr,"network connectivity override mismatch: offline=%g online=%g\n",offline.d,online.d);
      return 0;
    }
  }

  {
    fixture_key='A'; fixture_key_edge=0;
    if(!gml_keyboard_check(&fixture->vm,'A',0)){
      fprintf(stderr,"keyboard identity mapping mismatch\n");
      return 0;
    }
    gml_keyboard_set_map(&fixture->vm,'A','Z');
    if(gml_keyboard_get_map(&fixture->vm,'A')!='Z' ||
       !gml_keyboard_check(&fixture->vm,'Z',0) || gml_keyboard_check(&fixture->vm,'A',0)){
      fprintf(stderr,"keyboard remapping mismatch\n");
      return 0;
    }
    gml_keyboard_unset_map(&fixture->vm);
    if(gml_keyboard_get_map(&fixture->vm,'A')!='A' || !gml_keyboard_check(&fixture->vm,'A',0)){
      fprintf(stderr,"keyboard mapping reset mismatch\n");
      return 0;
    }
    fixture_key=fixture_key_edge=-1;
  }

  {
    /* json_decode is the legacy API: objects become ds_maps and arrays become
     * ds_lists (json_parse, by contrast, produces structs/arrays).  Save data
     * commonly round-trips those lists through ds_map_add_list + json_encode. */
    GmlVM jvm={0};
    GmlVal decode_arg=vstr("{\"items\":[1,2,{\"ok\":7}],\"version\":55}");
    GmlVal root=call_values(&jvm,"json_decode",&decode_arg,1);
    GmlVal find_items[2]={root,vstr("items")};
    GmlVal items=call_values(&jvm,"ds_map_find_value",find_items,2);
    GmlVal size_arg[1]={items};
    GmlVal index_args[2]={items,vreal(2)};
    GmlVal child=call_values(&jvm,"ds_list_find_value",index_args,2);
    GmlVal find_ok[2]={child,vstr("ok")};
    GmlVal okv=call_values(&jvm,"ds_map_find_value",find_ok,2);
    GmlVal encoded=call_values(&jvm,"json_encode",&root,1);
    GmlVal root_is_list[2]={root,vstr("items")};
    GmlVal items_is_map[2]={items,vreal(2)};
    if(root.t!=V_REAL || items.t!=V_REAL ||
       call_values(&jvm,"ds_list_size",size_arg,1).d!=3 ||
       okv.t!=V_REAL || okv.d!=7 || encoded.t!=V_STR ||
       call_values(&jvm,"ds_map_is_list",root_is_list,2).d!=1 ||
       call_values(&jvm,"ds_list_is_map",items_is_map,2).d!=1 ||
       strcmp(encoded.s?encoded.s:"","{\"items\":[1,2,{\"ok\":7}],\"version\":55}")){
      fprintf(stderr,"legacy JSON nested DS decode/encode mismatch: root=%g items=%g size=%g ok=%g json=%s\n",
              root.d,items.d,call_values(&jvm,"ds_list_size",size_arg,1).d,okv.d,
              encoded.t==V_STR&&encoded.s?encoded.s:"<non-string>");
      return 0;
    }

    GmlVal top_arg=vstr("[8,{\"n\":9}]");
    GmlVal top=call_values(&jvm,"json_decode",&top_arg,1);
    GmlVal find_default[2]={top,vstr("default")};
    GmlVal default_list=call_values(&jvm,"ds_map_find_value",find_default,2);
    GmlVal default_size[1]={default_list};
    if(top.t!=V_REAL || default_list.t!=V_REAL ||
       call_values(&jvm,"ds_map_is_list",find_default,2).d!=1 ||
       call_values(&jvm,"ds_list_size",default_size,1).d!=2){
      fprintf(stderr,"legacy JSON top-level array wrapper mismatch\n");
      return 0;
    }

    GmlVal trailing_arg=vstr("{\"items\":[1,2,],\"value\":7,}");
    GmlVal trailing=call_values(&jvm,"json_parse",&trailing_arg,1);
    GmlVal trailing_text=call_values(&jvm,"json_stringify",&trailing,1);
    const char *trailing_output=
      trailing_text.t==V_STR&&trailing_text.s?trailing_text.s:"";
    if(trailing.t!=V_REAL || !GML_IS_STRUCT_ID(trailing.d) ||
       trailing_text.t!=V_STR ||
       (strcmp(trailing_output,"{\"items\":[1,2],\"value\":7}") &&
        strcmp(trailing_output,"{\"value\":7,\"items\":[1,2]}"))){
      fprintf(stderr,"Studio JSON trailing-comma compatibility mismatch: %s\n",
              trailing_output[0]?trailing_output:"<non-string>");
      return 0;
    }

    GmlVal list=call_values(&jvm,"ds_list_create",NULL,0);
    GmlVal add_args[3]={list,vreal(4),vreal(5)};
    call_values(&jvm,"ds_list_add",add_args,3);
    GmlVal map=call_values(&jvm,"ds_map_create",NULL,0);
    GmlVal add_list_args[3]={map,vstr("data"),list};
    call_values(&jvm,"ds_map_add_list",add_list_args,3);
    GmlVal encoded_marked=call_values(&jvm,"json_encode",&map,1);
    if(encoded_marked.t!=V_STR || strcmp(encoded_marked.s?encoded_marked.s:"","{\"data\":[4,5]}")){
      fprintf(stderr,"ds_map_add_list JSON marker mismatch: %s\n",
              encoded_marked.t==V_STR&&encoded_marked.s?encoded_marked.s:"<non-string>");
      return 0;
    }

    GmlVal serial_map=call_values(&jvm,"ds_map_create",NULL,0);
    GmlVal serial_add[3]={serial_map,vstr("name"),vstr("ranger")};
    call_values(&jvm,"ds_map_add",serial_add,3);
    GmlVal serial_text=call_values(&jvm,"ds_map_write",&serial_map,1);
    GmlVal serial_copy=call_values(&jvm,"ds_map_create",NULL,0);
    GmlVal serial_read[2]={serial_copy,serial_text};
    GmlVal serial_find[2]={serial_copy,vstr("name")};
    GmlVal serial_value;
    if(call_values(&jvm,"ds_map_read",serial_read,2).d!=1 ||
       (serial_value=call_values(&jvm,"ds_map_find_value",serial_find,2)).t!=V_STR ||
       strcmp(serial_value.s?serial_value.s:"","ranger")){
      fprintf(stderr,"ds_map_write/read nested-list envelope mismatch\n");
      return 0;
    }

    GmlVal destroy[1];
    destroy[0]=root; call_values(&jvm,"ds_map_destroy",destroy,1);
    GmlVal exists_arg[1];
    exists_arg[0]=items;
    if(call_values(&jvm,"ds_exists",exists_arg,1).d!=0){
      fprintf(stderr,"destroying a decoded JSON root leaked a nested DS list\n");
      return 0;
    }
    exists_arg[0]=child;
    if(call_values(&jvm,"ds_exists",exists_arg,1).d!=0){
      fprintf(stderr,"destroying a decoded JSON root leaked a nested DS map\n");
      return 0;
    }
    destroy[0]=top; call_values(&jvm,"ds_map_destroy",destroy,1);
    destroy[0]=map; call_values(&jvm,"ds_map_destroy",destroy,1);
    exists_arg[0]=list;
    if(call_values(&jvm,"ds_exists",exists_arg,1).d!=0){
      fprintf(stderr,"destroying a marked parent map leaked its DS list\n");
      return 0;
    }
    destroy[0]=serial_map; call_values(&jvm,"ds_map_destroy",destroy,1);
    destroy[0]=serial_copy; call_values(&jvm,"ds_map_destroy",destroy,1);
    for(int pass=0;pass<300;pass++){
      GmlVal repeated=call_values(&jvm,"json_decode",&decode_arg,1);
      if(repeated.t!=V_REAL || repeated.d<0){
        fprintf(stderr,"repeated JSON decode exhausted nested DS slots at pass %d\n",pass);
        return 0;
      }
      destroy[0]=repeated;
      call_values(&jvm,"ds_map_destroy",destroy,1);
    }
    if(encoded.t==V_STR && encoded.d!=0) free((char*)encoded.s);
    if(encoded_marked.t==V_STR && encoded_marked.d!=0) free((char*)encoded_marked.s);
    if(serial_text.t==V_STR && serial_text.d!=0) free((char*)serial_text.s);
    if(trailing_text.t==V_STR && trailing_text.d!=0) free((char*)trailing_text.s);
  }

  {
    GmlWin settings={0};
    GmlRender initialized;
    settings.classic_version=800;
    settings.classic_interpolate=1;
    if(gml_render_init(&initialized,&settings)!=0 || !initialized.interp){
      fprintf(stderr,"classic texture interpolation setting was ignored\n");
      return 0;
    }
    gml_render_free(&initialized);
    settings.classic_version=0;
    if(gml_render_init(&initialized,&settings)!=0 || initialized.interp){
      fprintf(stderr,"classic texture interpolation leaked into modern content\n");
      return 0;
    }
    gml_render_free(&initialized);
  }

  {
    /* A two-column indexed palette: grayscale source values choose rows, while the normalized
     * column control chooses green/yellow. A coloured source texel bypasses lookup but is still
     * affected by the optional fragment colourise stage. */
    uint8_t source_rgba[]={0,0,0,255, 255,255,255,255, 20,40,80,255};
    uint8_t palette_rgba[]={255,0,0,255, 0,255,0,255,
                            0,0,255,255, 255,255,0,255};
    uint32_t palette_pixels[3]={0,0,0};
    GmlSprite sprites[2]; struct GmlShaderPal shader;
    GmlRender palette_render;
    memset(sprites,0,sizeof(sprites)); memset(&shader,0,sizeof(shader));
    memset(&palette_render,0,sizeof(palette_render));
    sprites[0].w=3; sprites[0].h=1; sprites[0].n_frames=1;
    sprites[0].runtime_rgba=source_rgba; sprites[0].runtime_opaque=1;
    sprites[1].w=2; sprites[1].h=2; sprites[1].n_frames=1;
    sprites[1].runtime_rgba=palette_rgba; sprites[1].runtime_opaque=1;
    shader.lut=shader.lut_indexed=1;
    shader.lut_row=0.75f; shader.lut_offset=255.0f; shader.lut_colors=2.0f;
    palette_render.spr=sprites; palette_render.n_spr=2;
    palette_render.shader_pal=&shader; palette_render.n_shader_pal=1;
    palette_render.lut_pal_sprite=1; palette_render.lut_pal_frame=0; palette_render.alpha=1.0;
    palette_render.alphablend=1; palette_render.color_write_mask=0x0F;
    palette_render.blend_equation=palette_render.blend_equation_alpha=1;
    gml_render_begin(&palette_render,palette_pixels,3,1,0,0);
    palette_render.active_shader=0; palette_render.lut_pal_sprite=1;
    palette_render.lut_pal_frame=0;
    gml_draw_sprite(&palette_render,0,0,0,0);
    if(palette_pixels[0]!=0xFF00FF00u || palette_pixels[1]!=0xFFFFFF00u ||
       palette_pixels[2]!=0xFF142850u){
      fprintf(stderr,"indexed palette mapping mismatch: %08x,%08x,%08x\n",
              palette_pixels[0],palette_pixels[1],palette_pixels[2]);
      return 0;
    }
    shader.lut_has_colorise=1;
    shader.lut_colorise[0]=1.0f; shader.lut_colorise[1]=0.0f;
    shader.lut_colorise[2]=0.0f; shader.lut_colorise[3]=1.0f;
    memset(palette_pixels,0,sizeof(palette_pixels));
    gml_render_begin(&palette_render,palette_pixels,3,1,0,0);
    palette_render.active_shader=0; palette_render.lut_pal_sprite=1;
    palette_render.lut_pal_frame=0;
    gml_draw_sprite(&palette_render,0,0,0,0);
    if(palette_pixels[0]!=0xFFFF0000u || palette_pixels[1]!=0xFFFF0000u ||
       palette_pixels[2]!=0xFFFF0000u){
      fprintf(stderr,"indexed palette colourise mismatch: %08x,%08x,%08x\n",
              palette_pixels[0],palette_pixels[1],palette_pixels[2]);
      return 0;
    }
    /* A constant -1 radial phase pulls both edge texels to the centre texel. This exercises the
     * texture-coordinate displacement on runtime sprites without relying on a packaged asset. */
    source_rgba[0]=255; source_rgba[1]=0; source_rgba[2]=0;
    source_rgba[4]=0; source_rgba[5]=255; source_rgba[6]=0;
    source_rgba[8]=0; source_rgba[9]=0; source_rgba[10]=255;
    memset(&shader,0,sizeof(shader)); shader.radial_wave=1;
    shader.radial_wave_value[0][0]=(float)(M_PI*0.5);
    shader.radial_wave_value[2][0]=3; shader.radial_wave_value[2][1]=1;
    shader.radial_wave_value[4][0]=1; shader.radial_wave_value[5][0]=1;
    memset(palette_pixels,0,sizeof(palette_pixels));
    gml_render_begin(&palette_render,palette_pixels,3,1,0,0);
    palette_render.active_shader=0;
    gml_draw_sprite(&palette_render,0,0,0,0);
    if(palette_pixels[0]!=0xFF00FF00u || palette_pixels[1]!=0xFF00FF00u ||
       palette_pixels[2]!=0xFF00FF00u){
      fprintf(stderr,"radial wave mapping mismatch: %08x,%08x,%08x\n",
              palette_pixels[0],palette_pixels[1],palette_pixels[2]);
      return 0;
    }
  }

  {
    enum { INFO_W=320, INFO_H=180 };
    static uint32_t information_pixels[INFO_W*INFO_H];
    uint8_t information[256];
    size_t information_size=classic_information_record(information,sizeof(information));
    GmlWin win={0}; GmlRender initialized;
    win.classic_version=800;
    if(!information_size || gml_render_init(&initialized,&win)!=0){
      fprintf(stderr,"classic information fixture initialization failed\n");
      return 0;
    }
    initialized.color=0x123456u; initialized.alpha=0.375;
    initialized.halign=2; initialized.valign=2; initialized.font=7;
    initialized.alphablend=0;
    memset(information_pixels,0,sizeof(information_pixels));
    gml_draw_classic_game_information(&initialized,information_pixels,INFO_W,INFO_H,
                                      information,information_size);
    int ink=0;
    for(int i=0;i<INFO_W*INFO_H;i++)
      if(information_pixels[i]!=0xFFFFFFu && information_pixels[i]!=0xABADB3u) ink++;
    if(information_pixels[0]!=0xABADB3u ||
       information_pixels[INFO_W*INFO_H-1]!=0xABADB3u ||
       information_pixels[INFO_W+1]!=0xFFFFFFu || ink<20 ||
       initialized.color!=0x123456u || initialized.alpha!=0.375 ||
       initialized.halign!=2 || initialized.valign!=2 || initialized.font!=7 ||
       initialized.alphablend!=0){
      fprintf(stderr,"classic information software render mismatch: edge=%06x,%06x inside=%06x ink=%d state=%06x,%.3f,%d,%d,%d,%d font=%d atlas=%d\n",
              information_pixels[0]&0xFFFFFFu,
              information_pixels[INFO_W*INFO_H-1]&0xFFFFFFu,
              information_pixels[INFO_W+1]&0xFFFFFFu,ink,
              initialized.color&0xFFFFFFu,initialized.alpha,initialized.halign,
              initialized.valign,initialized.font,initialized.alphablend,
              initialized.default_font.n_glyphs,initialized.n_atlas);
      gml_render_free(&initialized);
      return 0;
    }
    information_pixels[0]=0x123456u;
    gml_draw_classic_game_information(&initialized,information_pixels,INFO_W,INFO_H,
                                      information,11);
    if(information_pixels[0]!=0x123456u){
      fprintf(stderr,"invalid classic information record was rendered\n");
      gml_render_free(&initialized);
      return 0;
    }
    gml_render_free(&initialized);

    GmlVM modal={0};
    fixture_attach_input(&modal);
    gml_keyboard_unset_map(&modal);
    win.classic_game_information=information;
    win.classic_game_information_size=information_size;
    modal.win=&win;
    (void)call_values(&modal,"show_info",NULL,0);
    if(!modal.classic_info_active){
      fprintf(stderr,"classic information modal did not activate\n");
      return 0;
    }
    fixture_key='A'; fixture_key_edge=1;
    gml_vm_step(&modal);
    fixture_key=fixture_key_edge=-1;
    if(modal.classic_info_active){
      fprintf(stderr,"classic information modal did not dismiss\n");
      return 0;
    }
    (void)call_values(&modal,"action_show_info",NULL,0);
    if(!modal.classic_info_active){
      fprintf(stderr,"classic information action did not activate\n");
      return 0;
    }
    modal.classic_info_active=0; win.classic_version=0;
    (void)call_values(&modal,"show_info",NULL,0);
    if(modal.classic_info_active){
      fprintf(stderr,"classic information leaked into modern content\n");
      return 0;
    }
  }

  {
    GmlWin win={0};
    win.classic_version=800;
    fixture->vm.win=&win;
    fixture->render.presentation_w=640; fixture->render.presentation_h=480;
    if(call_values(&fixture->vm,"display_get_width",NULL,0).d!=1280 ||
       call_values(&fixture->vm,"display_get_height",NULL,0).d!=720 ||
       call_values(&fixture->vm,"window_get_width",NULL,0).d!=640 ||
       call_values(&fixture->vm,"window_get_height",NULL,0).d!=480){
      fprintf(stderr,"classic virtual display/window size mismatch\n");
      return 0;
    }
    fixture_mouse_x=320; fixture_mouse_y=240;
    if(call_values(&fixture->vm,"window_mouse_get_x",NULL,0).d!=320 ||
       call_values(&fixture->vm,"window_mouse_get_y",NULL,0).d!=240 ||
       call_values(&fixture->vm,"display_mouse_get_x",NULL,0).d!=640 ||
       call_values(&fixture->vm,"display_mouse_get_y",NULL,0).d!=360){
      fprintf(stderr,"classic display/window mouse coordinate mismatch\n");
      return 0;
    }
    { double p[2]={640,360}; call_numbers(&fixture->vm,"display_mouse_set",p,2); }
    if(fixture_mouse_set_x!=320 || fixture_mouse_set_y!=240){
      fprintf(stderr,"classic display mouse warp mismatch: %.3f,%.3f\n",
              fixture_mouse_set_x,fixture_mouse_set_y);
      return 0;
    }
    win.classic_version=0;
    if(call_values(&fixture->vm,"display_get_width",NULL,0).d!=640 ||
       call_values(&fixture->vm,"display_get_height",NULL,0).d!=480 ||
       call_values(&fixture->vm,"display_mouse_get_x",NULL,0).d!=320 ||
       call_values(&fixture->vm,"display_mouse_get_y",NULL,0).d!=240){
      fprintf(stderr,"modern presentation display size changed\n");
      return 0;
    }
    { double p[2]={123,45}; call_numbers(&fixture->vm,"display_mouse_set",p,2); }
    if(fixture_mouse_set_x!=123 || fixture_mouse_set_y!=45){
      fprintf(stderr,"modern display mouse coordinates changed\n");
      return 0;
    }
    /* Cursor confinement belongs to the libretro frontend. The legacy Studio calls remain
     * successful platform operations even when invoked every step. */
    { double rect[4]={0,0,640,480};
      call_numbers(&fixture->vm,"display_mouse_lock",rect,4);
      (void)call_values(&fixture->vm,"display_mouse_unlock",NULL,0);
    }
    fixture->render.presentation_w=fixture->render.presentation_h=0;
    fixture->vm.win=NULL;
  }

  {
    GmlWin win={0};
    uint32_t source[2]={0xFFFF0000u,0xFF0000FFu};
    uint32_t target[3]={0,0,0};
    fixture->render.app_surface=source; fixture->render.app_w=2; fixture->render.app_h=1;
    fixture->render.app_surface_opaque=1; fixture->render.classic=1; fixture->render.win=&win;
    win.classic_version=800; win.classic_scaling=-1;
    gml_render_begin(&fixture->render,target,3,1,0,0);
    gml_render_set_pending_underlay(&fixture->render,0,0,3,1);
    gml_render_flush_pending_underlay(&fixture->render);
    if(target[0]!=0xFFFF0000u || target[1]!=0xFF0000FFu || target[2]!=0xFF0000FFu){
      fprintf(stderr,"classic centre-sampled presentation mismatch\n");
      return 0;
    }
    {
      uint32_t fractional_source[3]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu};
      uint32_t fractional_target[4]={0,0,0,0};
      fixture->render.app_surface=fractional_source; fixture->render.app_w=3;
      gml_render_begin(&fixture->render,fractional_target,4,1,0,0);
      gml_render_set_pending_underlay(&fixture->render,0,0,4,1);
      gml_render_flush_pending_underlay(&fixture->render);
      if(fractional_target[0]!=0xFFFF0000u || fractional_target[1]!=0xFF00FF00u ||
         fractional_target[2]!=0xFF0000FFu || fractional_target[3]!=0xFF0000FFu){
        fprintf(stderr,"classic fractional presentation phase mismatch\n");
        return 0;
      }
      fixture->render.app_surface=source; fixture->render.app_w=2;
    }
    memset(target,0,sizeof(target)); fixture->render.classic=0;
    gml_render_begin(&fixture->render,target,3,1,0,0);
    gml_render_set_pending_underlay(&fixture->render,0,0,3,1);
    gml_render_flush_pending_underlay(&fixture->render);
    if(target[0]!=0xFFFF0000u || target[1]!=0xFFFF0000u || target[2]!=0xFF0000FFu){
      fprintf(stderr,"modern leading-edge presentation changed: %08x %08x %08x\n",
              target[0],target[1],target[2]);
      return 0;
    }
    memset(target,0,sizeof(target)); fixture->render.classic=1; win.classic_scaling=0;
    gml_render_begin(&fixture->render,target,3,1,0,0);
    gml_render_set_pending_underlay(&fixture->render,0,0,3,1);
    gml_render_flush_pending_underlay(&fixture->render);
    if(target[0]!=0xFFFF0000u || target[1]!=0xFFFF0000u || target[2]!=0xFF0000FFu){
      fprintf(stderr,"classic full-scale presentation changed\n");
      return 0;
    }
    fixture->render.app_surface=NULL; fixture->render.app_w=fixture->render.app_h=0; fixture->render.classic=0; fixture->render.win=NULL;
    win.classic_version=0; win.classic_scaling=0;
  }

  {
    GmlWin surface_win={0};
    uint32_t borrowed[6]={
      0xFF010203u,0xFF040506u,0xFF070809u,
      0xFF111213u,0xFF141516u,0xFF171819u
    };
    fixture->render.win=&surface_win;
    fixture->render.app_surface=borrowed; fixture->render.app_w=3; fixture->render.app_h=2;
    surface_win.bytecode=15;
    /* Explicit application-surface resize applies to this generation too. */
    { const double resize_args[3]={0,5,4}; call_numbers(&fixture->vm,"surface_resize",resize_args,3); }
    if(!fixture->render.app_surface_owned || fixture->render.app_surface!=fixture->render.app_surface_owned ||
       gml_surface_width(&fixture->render,0)!=5 || gml_surface_height(&fixture->render,0)!=4){
      fprintf(stderr,"Studio 1.x application-surface resize was ignored\n");
      return 0;
    }
    surface_win.bytecode=17;
    { const double resize_args[3]={0,5,4}; call_numbers(&fixture->vm,"surface_resize",resize_args,3); }
    if(!fixture->render.app_surface_owned || fixture->render.app_surface!=fixture->render.app_surface_owned ||
       gml_surface_width(&fixture->render,0)!=5 || gml_surface_height(&fixture->render,0)!=4 ||
       fixture->render.app_surface[0]!=borrowed[0] || fixture->render.app_surface[2]!=borrowed[2] ||
       fixture->render.app_surface[5]!=borrowed[3] || fixture->render.app_surface[7]!=borrowed[5] ||
       fixture->render.app_surface[4]!=0 || fixture->render.app_surface[19]!=0){
      fprintf(stderr,"explicit application-surface resize mismatch\n");
      return 0;
    }
    { const double resize_args[3]={0,2,1}; call_numbers(&fixture->vm,"surface_resize",resize_args,3); }
    if(gml_surface_width(&fixture->render,0)!=2 || gml_surface_height(&fixture->render,0)!=1 ||
       fixture->render.app_surface[0]!=borrowed[0] || fixture->render.app_surface[1]!=borrowed[1]){
      fprintf(stderr,"application-surface resize preservation mismatch\n");
      return 0;
    }
    free(fixture->render.app_surface_owned);
    fixture->render.app_surface_owned=NULL;
    fixture->render.app_surface=NULL; fixture->render.app_w=fixture->render.app_h=0;
    fixture->render.win=NULL;
  }

  {
    /* display_set_gui_size takes effect immediately inside Draw GUI. A temporary logical canvas
     * twice the target size must place both sprite-style coordinates and software primitives in
     * the same physical pixels, then restore the initial mapping later in the same event. */
    GmlWin gui_win={0}; gui_win.bytecode=17;
    fixture->vm.win=&gui_win; fixture->render.win=&gui_win;
    uint32_t target[8]={0};
    gml_render_begin(&fixture->render,target,4,2,0,0);
    gml_render_gui_begin(&fixture->render,4,2);
    const double gui_large[]={8,4};
    call_numbers(&fixture->vm,"display_set_gui_size",gui_large,2);
    if(fixture->vm.gui_w!=8 || fixture->vm.gui_h!=4 || fixture->render.gui_scale_x!=0.5 || fixture->render.gui_scale_y!=0.5){
      fprintf(stderr,"mid-pass GUI logical-size activation mismatch: gui=%d,%d scale=%g,%g active=%d target=%d stack=%d\n",
              fixture->vm.gui_w,fixture->vm.gui_h,fixture->render.gui_scale_x,fixture->render.gui_scale_y,
              fixture->render.gui_pass_active,fixture->render.target_id,fixture->render.target_sp);
      return 0;
    }
    fixture->render.color=0xFFFFFFu; fixture->render.alpha=1.0;
    const double right_half[]={4,0,8,4,0};
    call_numbers(&fixture->vm,"draw_rectangle",right_half,5);
    const double gui_base[]={4,2};
    call_numbers(&fixture->vm,"display_set_gui_size",gui_base,2);
    gml_render_gui_end(&fixture->render);
    for(int y=0;y<2;y++) for(int x=0;x<4;x++){
      int colored=(target[y*4+x]&0x00FFFFFFu)!=0;
      if(colored!=(x>=2)){
        fprintf(stderr,"mid-pass GUI logical-size transform mismatch at %d,%d: %08x\n",
                x,y,target[y*4+x]);
        return 0;
      }
    }
    if(fixture->render.gui_pass_active || fixture->render.gui_scale_x!=1.0 || fixture->render.gui_scale_y!=1.0 ||
       fixture->vm.gui_w!=4 || fixture->vm.gui_h!=2){
      fprintf(stderr,"mid-pass GUI logical-size restore mismatch\n");
      return 0;
    }
    fixture->vm.win=NULL; fixture->render.win=NULL;
  }

  {
    /* Screen-stage code may explicitly stretch application_surface to the complete window using
     * window-pixel dimensions.  Do not apply the logical GUI scale a second time. */
    uint32_t source[4]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu,0xFFFFFFFFu};
    uint32_t target[64]={0};
    gml_render_begin(&fixture->render,target,8,8,0,0);
    fixture->render.app_surface=source; fixture->render.app_w=2; fixture->render.app_h=2;
    fixture->render.app_surface_opaque=1;
    gml_render_gui_begin(&fixture->render,8,8);
    gml_render_gui_set_size(&fixture->render,2,2);
    gml_draw_surface_stretched(&fixture->render,0,0,0,8,8,0xFFFFFFu,1.0);
    gml_render_gui_end(&fixture->render);
    if(target[0]!=0xFFFF0000u || target[7]!=0xFF00FF00u ||
       target[7*8]!=0xFF0000FFu || target[63]!=0xFFFFFFFFu){
      fprintf(stderr,"explicit screen-stage surface raster mismatch: %08x %08x %08x %08x\n",
              target[0],target[7],target[7*8],target[63]);
      return 0;
    }
    memset(target,0,sizeof(target));
    gml_render_begin(&fixture->render,target,8,8,0,0);
    gml_render_gui_begin(&fixture->render,8,8);
    gml_render_gui_set_size(&fixture->render,16,16);
    gml_draw_surface_stretched(&fixture->render,0,0,0,8,8,0xFFFFFFu,1.0);
    gml_render_gui_end(&fixture->render);
    if(target[0]!=0xFFFF0000u || target[3]!=0xFF00FF00u ||
       target[3*8]!=0xFF0000FFu || target[3*8+3]!=0xFFFFFFFFu ||
       target[7]!=0 || target[7*8]!=0){
      fprintf(stderr,"oversized GUI surface transform mismatch: %08x %08x %08x %08x edges=%08x,%08x\n",
              target[0],target[3],target[3*8],target[3*8+3],target[7],target[7*8]);
      return 0;
    }
    memset(target,0,sizeof(target));
    GmlWin early_studio={0}; early_studio.bytecode=14;
    fixture->render.win=&early_studio;
    gml_render_begin(&fixture->render,target,8,8,0,0);
    gml_render_gui_begin(&fixture->render,8,8);
    gml_render_gui_set_size(&fixture->render,9,9);
    gml_draw_surface_stretched(&fixture->render,0,1,1,2,2,0xFFFFFFu,1.0);
    gml_render_gui_end(&fixture->render);
    fixture->render.win=NULL;
    if(target[1*8+1]!=0xFFFF0000u || target[1*8+2]!=0xFF00FF00u ||
       target[2*8+1]!=0xFF0000FFu || target[2*8+2]!=0xFFFFFFFFu ||
       target[0]!=0 || target[3*8+3]!=0){
      fprintf(stderr,"first-generation fractional GUI surface mismatch: %08x %08x %08x %08x edges=%08x,%08x\n",
              target[1*8+1],target[1*8+2],target[2*8+1],target[2*8+2],
              target[0],target[3*8+3]);
      return 0;
    }
    fixture->render.app_surface=NULL; fixture->render.app_w=fixture->render.app_h=0; fixture->render.app_surface_opaque=0;
  }

  {
    /* A screen-relative GUI transform overrides the size-derived fit, including its draw offset,
     * and resetting it restores the ordinary display_set_gui_size mapping immediately. */
    GmlWin gui_win={0}; gui_win.bytecode=17;
    fixture->vm.win=&gui_win; fixture->render.win=&gui_win;
    uint32_t target[64]={0};
    gml_render_begin(&fixture->render,target,8,8,0,0);
    gml_render_gui_begin(&fixture->render,8,8);
    const double gui_size[]={2,2};
    /* GUI maximise uses the dimensions returned by window queries: the presented raster.
     * Declare 16x24 over an 8x8 GUI target to exercise that conversion. */
    gml_render_presentation_effective_set(&fixture->render,16,24);
    const double maximise[]={2,3,2,6};
    call_numbers(&fixture->vm,"display_set_gui_size",gui_size,2);
    call_numbers(&fixture->vm,"display_set_gui_maximise",maximise,4);
    double x=2.0,y=2.0;
    gml_render_gui_map_point(&fixture->render,&x,&y);
    if(!fixture->vm.gui_maximise_active || fixture->vm.gui_maximise_xscale!=2.0 ||
       fixture->vm.gui_maximise_yscale!=3.0 || fixture->vm.gui_maximise_xoffset!=2.0 ||
       fixture->vm.gui_maximise_yoffset!=6.0 || fixture->render.gui_scale_x!=1.0 ||
       fixture->render.gui_scale_y!=1.0 || x!=3.0 || y!=4.0){
      fprintf(stderr,"GUI maximise transform mismatch: active=%d scale=%g,%g offset=%g,%g point=%g,%g\n",
              fixture->vm.gui_maximise_active,fixture->render.gui_scale_x,fixture->render.gui_scale_y,
              fixture->render.gui_maximise_xoffset,fixture->render.gui_maximise_yoffset,x,y);
      return 0;
    }
    const double reset[]={-1,-1};
    call_numbers(&fixture->vm,"display_set_gui_maximise",reset,2);
    if(fixture->vm.gui_maximise_active || fixture->render.gui_maximise_active ||
       fixture->render.gui_scale_x!=4.0 || fixture->render.gui_scale_y!=4.0 ||
       fixture->render.gui_maximise_xoffset!=0.0 || fixture->render.gui_maximise_yoffset!=0.0){
      fprintf(stderr,"GUI maximise reset mismatch\n");
      return 0;
    }
    call_numbers(&fixture->vm,"display_set_gui_maximise",NULL,0);
    if(!fixture->vm.gui_maximise_active || !fixture->render.gui_maximise_active ||
       fixture->render.gui_scale_x!=0.5 || fixture->render.gui_scale_y!=1.0/3.0){
      fprintf(stderr,"automatic GUI maximise mismatch\n");
      return 0;
    }
    call_numbers(&fixture->vm,"display_set_gui_size",gui_size,2);
    if(fixture->vm.gui_maximise_active || fixture->render.gui_maximise_active ||
       fixture->render.gui_scale_x!=4.0 || fixture->render.gui_scale_y!=4.0){
      fprintf(stderr,"GUI size did not override maximise\n");
      return 0;
    }
    call_numbers(&fixture->vm,"display_set_gui_maximise",reset,2);
    gml_render_gui_end(&fixture->render);
    gml_render_presentation_effective_set(&fixture->render,0,0);
    fixture->vm.win=NULL; fixture->render.win=NULL;
  }

  {
    GmlWin win={0};
    uint32_t source[4]={0xFFFF0000u,0xFF0000FFu,0xFFFF0000u,0xFF0000FFu};
    uint32_t target[16]={0};
    fixture->render.app_surface=source; fixture->render.app_w=2; fixture->render.app_h=2;
    fixture->render.app_surface_opaque=1; fixture->render.classic=1; fixture->render.interp=1; fixture->render.win=&win;
    win.classic_version=800; win.classic_scaling=0; win.classic_interpolate=1;
    gml_render_begin(&fixture->render,target,4,4,0,0);
    gml_render_set_pending_underlay(&fixture->render,0,0,4,4);
    gml_render_flush_pending_underlay(&fixture->render);
    for(int y=0;y<4;y++){
      if(target[y*4+0]!=0xFFFF0000u || target[y*4+1]!=0xFF800080u ||
         target[y*4+2]!=0xFF0000FFu || target[y*4+3]!=0xFF0000FFu){
        fprintf(stderr,"classic interpolated presentation mismatch\n");
        return 0;
      }
    }
    {
      uint32_t phase_x[4]={0xFF00FF00u,0xFF008800u,0xFF004400u,0xFF002200u};
      uint32_t phase_y[4]={0xFFFFFF00u,0xFF888800u,0xFF444400u,0xFF222200u};
      uint32_t phase_xy[4]={0xFF00FFFFu,0xFF008888u,0xFF004444u,0xFF002222u};
      const uint32_t expected[16]={
        source[0],phase_x[0],source[1],phase_x[1],
        phase_y[0],phase_xy[0],phase_y[1],phase_xy[1],
        source[2],phase_x[2],source[3],phase_x[3],
        phase_y[2],phase_xy[2],phase_y[3],phase_xy[3]
      };
      memset(target,0,sizeof(target));
      fixture->render.app_interp_phase[0]=phase_x;
      fixture->render.app_interp_phase[1]=phase_y;
      fixture->render.app_interp_phase[2]=phase_xy;
      gml_render_begin(&fixture->render,target,4,4,0,0);
      gml_render_set_pending_underlay(&fixture->render,0,0,4,4);
      gml_render_flush_pending_underlay(&fixture->render);
      if(memcmp(target,expected,sizeof(expected))!=0){
        fprintf(stderr,"classic interpolated phase-plane presentation mismatch\n");
        return 0;
      }
      memset(phase_x,0,sizeof(phase_x));
      memset(phase_y,0,sizeof(phase_y));
      memset(phase_xy,0,sizeof(phase_xy));
      gml_render_begin(&fixture->render,target,2,2,0,0);
      fixture->render.classic_interp_phase[0]=phase_x;
      fixture->render.classic_interp_phase[1]=phase_y;
      fixture->render.classic_interp_phase[2]=phase_xy;
      gml_render_set_pending_fill(&fixture->render,0xFF123456u);
      for(int i=0;i<4;i++) if(phase_x[i]!=0xFF123456u ||
                                phase_y[i]!=0xFF123456u ||
                                phase_xy[i]!=0xFF123456u){
        fprintf(stderr,"classic interpolated phase-plane clear mismatch\n");
        return 0;
      }
      fixture->render.classic_interp_phase[0]=NULL;
      fixture->render.classic_interp_phase[1]=NULL;
      fixture->render.classic_interp_phase[2]=NULL;
      fixture->render.app_interp_phase[0]=NULL;
      fixture->render.app_interp_phase[1]=NULL;
      fixture->render.app_interp_phase[2]=NULL;
    }
    fixture->render.app_surface=NULL; fixture->render.app_w=fixture->render.app_h=0;
    fixture->render.classic=0; fixture->render.interp=0; fixture->render.win=NULL;
  }

  {
    GmlWin win={0};
    int x=0,y=0;
    win.classic_version=800; win.classic_scaling=-1;
    gml_classic_present_adjust(&win,250,180,500,360,0,&x,&y);
    if(x!=-1 || y!=-1){
      fprintf(stderr,"classic doubled presentation origin mismatch\n");
      return 0;
    }
    x=0; y=0; win.classic_interpolate=1;
    gml_classic_present_adjust(&win,250,180,500,360,1,&x,&y);
    if(x!=0 || y!=0){
      fprintf(stderr,"classic interpolated presentation origin mismatch\n");
      return 0;
    }
    win.classic_interpolate=0;
    x=3; y=4;
    gml_classic_present_adjust(&win,250,180,512,362,0,&x,&y);
    if(x!=3 || y!=4){
      fprintf(stderr,"classic centred fractional presentation changed\n");
      return 0;
    }
    win.classic_scaling=0;
    gml_classic_present_adjust(&win,250,180,512,362,0,&x,&y);
    if(x!=2 || y!=3){
      fprintf(stderr,"classic fixed fractional presentation mismatch\n");
      return 0;
    }
    win.classic_version=701;
    gml_classic_present_adjust(&win,320,240,641,481,0,&x,&y);
    if(x!=1 || y!=2){
      fprintf(stderr,"legacy fractional presentation mismatch\n");
      return 0;
    }
    gml_classic_present_adjust(&win,320,240,320,240,0,&x,&y);
    if(x!=1 || y!=2){
      fprintf(stderr,"native classic presentation changed\n");
      return 0;
    }
  }

  {
    GmlWin win={0};
    int x=9,y=9,w=9,h=9;
    win.classic_version=701;
    if(!gml_classic_present_explicit_port(&win,1,480,432,0,0,160,144,
                                          &x,&y,&w,&h) ||
       x!=0 || y!=0 || w!=160 || h!=144){
      fprintf(stderr,"classic explicit-window port mismatch\n");
      return 0;
    }
    x=y=w=h=9;
    if(gml_classic_present_explicit_port(&win,0,480,432,0,0,160,144,
                                         &x,&y,&w,&h) ||
       x!=9 || y!=9 || w!=9 || h!=9){
      fprintf(stderr,"implicit classic window changed port\n");
      return 0;
    }
    win.classic_version=0;
    if(gml_classic_present_explicit_port(&win,1,480,432,5,7,160,144,
                                         &x,&y,&w,&h)){
      fprintf(stderr,"modern explicit window used classic port\n");
      return 0;
    }
  }

  {
    int visible[8]={0}, xport[8]={0}, yport[8]={0};
    int wport[8]={0}, hport[8]={0};
    int w=0,h=0;
    gml_classic_room_window_size(550,400,-1,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=550 || h!=400){
      fprintf(stderr,"viewless classic room window mismatch\n");
      return 0;
    }
    visible[0]=1; wport[0]=320; hport[0]=240;
    gml_classic_room_window_size(800,600,-1,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=320 || h!=240){
      fprintf(stderr,"single-port classic room window mismatch\n");
      return 0;
    }
    visible[1]=1; xport[1]=320; yport[1]=240; wport[1]=320; hport[1]=240;
    gml_classic_room_window_size(800,600,-1,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=640 || h!=480){
      fprintf(stderr,"multi-port classic room window mismatch\n");
      return 0;
    }
    gml_classic_room_window_size(320,240,200,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=640 || h!=480){
      fprintf(stderr,"fixed-scale classic window mismatch\n");
      return 0;
    }
  }

  return 1;
}
