/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_render.h"
#include "gmlc/gmlc_package.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fixture_joystick_button3;
int gml_input_key(int key,int edge){ return key==65 && edge==0; }
int gml_input_gamepad(int button,int edge){
  return fixture_joystick_button3 && button==32771 && edge==0;
}
void gml_input_mouse(double *rx,double *ry,double *gx,double *gy,double *wx,double *wy,
                     int *held,int *pressed,int *released,int *wheel){
  if(rx)*rx=0; if(ry)*ry=0; if(gx)*gx=0; if(gy)*gy=0; if(wx)*wx=0; if(wy)*wy=0;
  if(held)*held=1; if(pressed)*pressed=0; if(released)*released=0; if(wheel)*wheel=0;
}
GmlVal gml_builtin_call(GmlVM *vm,const char *name,GmlVal *args,int count);

static int fixture_write_text(const char *path,const char *text){
  FILE *f=fopen(path,"wb");
  int ok=f && fwrite(text,1,strlen(text),f)==strlen(text);
  if(f && fclose(f)) ok=0;
  return ok;
}

static int fixture_write_large_ini(const char *path){
  FILE *f=fopen(path,"wb");
  if(!f) return 0;
  int ok=fprintf(f,"[localization]\n")>0;
  for(int i=0;ok && i<600;i++)
    ok=fprintf(f,i==599?"key_%03d=\"value_%03d\"\n":"key_%03d=value_%03d\n",i,i)>0;
  if(fclose(f)) ok=0;
  return ok;
}

static int fixture_read_text(const char *path,char *out,size_t cap){
  FILE *f=fopen(path,"rb");
  if(!f || !cap){ if(f) fclose(f); return 0; }
  size_t n=fread(out,1,cap-1,f); out[n]=0;
  int ok=!ferror(f);
  fclose(f);
  return ok;
}

static int expect_file_sandbox(GmlVM *vm,const char *save_dir){
  char overlay[80],written[80],ini[80],large_ini[80];
  snprintf(overlay,sizeof overlay,"gml-overlay-%ld.txt",(long)getpid());
  snprintf(written,sizeof written,"gml-written-%ld.txt",(long)getpid());
  snprintf(ini,sizeof ini,"gml-default-%ld.ini",(long)getpid());
  snprintf(large_ini,sizeof large_ini,"gml-large-%ld.ini",(long)getpid());
  char content_overlay[640],save_overlay[640],content_written[640],save_written[640];
  char content_ini[640],save_ini[640],content_large_ini[640],save_large_ini[640];
  snprintf(content_overlay,sizeof content_overlay,"%s/%s",vm->win->content_dir,overlay);
  snprintf(save_overlay,sizeof save_overlay,"%s/%s",save_dir,overlay);
  snprintf(content_written,sizeof content_written,"%s/%s",vm->win->content_dir,written);
  snprintf(save_written,sizeof save_written,"%s/%s",save_dir,written);
  snprintf(content_ini,sizeof content_ini,"%s/%s",vm->win->content_dir,ini);
  snprintf(save_ini,sizeof save_ini,"%s/%s",save_dir,ini);
  snprintf(content_large_ini,sizeof content_large_ini,"%s/%s",vm->win->content_dir,large_ini);
  snprintf(save_large_ini,sizeof save_large_ini,"%s/%s",save_dir,large_ini);
  unlink(content_overlay); unlink(save_overlay); unlink(content_written); unlink(save_written);
  unlink(content_ini); unlink(save_ini); unlink(content_large_ini); unlink(save_large_ini);
  if(!fixture_write_text(content_overlay,"program") || !fixture_write_text(save_overlay,"save") ||
     !fixture_write_text(content_ini,"[fixture]\nvalue=7\n") ||
     !fixture_write_large_ini(content_large_ini)) return 0;

  GmlVal name=vstr(overlay);
  GmlVal handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  GmlVal line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  int ok=handle.t==V_REAL && handle.d>0 && line.t==V_STR && line.s && !strcmp(line.s,"save");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);
  unlink(save_overlay);
  handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  ok=ok && line.t==V_STR && line.s && !strcmp(line.s,"program");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);

  /* working_directory concatenation produces an absolute bundle path. It must retain the same
   * save-over-bundle read overlay rather than bypassing the sandbox. */
  if(!fixture_write_text(save_overlay,"absolute-save")) ok=0;
  name=vstr(content_overlay);
  handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  ok=ok && line.t==V_STR && line.s && !strcmp(line.s,"absolute-save");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);
  unlink(save_overlay);

  name=vstr(written);
  handle=gml_builtin_call(vm,"file_text_open_write",&name,1);
  GmlVal write_args[2]={handle,vstr("sandbox")};
  (void)gml_builtin_call(vm,"file_text_write_string",write_args,2);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  char observed[128]={0};
  ok=ok && !fixture_read_text(content_written,observed,sizeof observed) &&
     fixture_read_text(save_written,observed,sizeof observed) && !strcmp(observed,"sandbox");
  unlink(save_written);
  name=vstr(content_written);
  handle=gml_builtin_call(vm,"file_text_open_write",&name,1);
  GmlVal absolute_write_args[2]={handle,vstr("absolute-sandbox")};
  (void)gml_builtin_call(vm,"file_text_write_string",absolute_write_args,2);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  observed[0]=0;
  ok=ok && !fixture_read_text(content_written,observed,sizeof observed) &&
     fixture_read_text(save_written,observed,sizeof observed) && !strcmp(observed,"absolute-sandbox");

  name=vstr(ini);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  GmlVal read_args[3]={vstr("fixture"),vstr("value"),vreal(-1)};
  GmlVal initial=gml_builtin_call(vm,"ini_read_real",read_args,3);
  GmlVal ini_write_args[3]={vstr("fixture"),vstr("value"),vreal(11)};
  (void)gml_builtin_call(vm,"ini_write_real",ini_write_args,3);
  (void)gml_builtin_call(vm,"ini_close",NULL,0);
  char installed[128]={0},saved[128]={0};
  ok=ok && initial.t==V_REAL && initial.d==7 &&
     fixture_read_text(content_ini,installed,sizeof installed) && strstr(installed,"value=7") &&
     fixture_read_text(save_ini,saved,sizeof saved) && strstr(saved,"value=11");

  /* Localization INIs routinely exceed the old 256-entry settings limit and quote
   * string values.  A late value must be unquoted and survive the save overlay. */
  name=vstr(large_ini);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  GmlVal late_args[3]={vstr("localization"),vstr("key_599"),vstr("missing")};
  GmlVal late=gml_builtin_call(vm,"ini_read_string",late_args,3);
  ok=ok && late.t==V_STR && late.s && !strcmp(late.s,"value_599") && vm->ini_n==600;
  if(late.t==V_STR && late.d!=0) free((void*)late.s);
  (void)gml_builtin_call(vm,"ini_close",NULL,0);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  late=gml_builtin_call(vm,"ini_read_string",late_args,3);
  ok=ok && late.t==V_STR && late.s && !strcmp(late.s,"value_599");
  if(late.t==V_STR && late.d!=0) free((void*)late.s);
  (void)gml_builtin_call(vm,"ini_close",NULL,0);

  unlink(content_overlay); unlink(save_overlay); unlink(content_written); unlink(save_written);
  unlink(content_ini); unlink(save_ini); unlink(content_large_ini); unlink(save_large_ini);
  if(!ok) fprintf(stderr,"read overlay / writable sandbox fixture failed\n");
  return ok;
}

static GmlInstance *find_slot(GmlVM *vm,uint32_t id){
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].id==id) return &vm->inst[i];
  return NULL;
}

static uint32_t fixture_u32(const uint8_t *data,size_t offset){
  return (uint32_t)data[offset] | (uint32_t)data[offset+1]<<8 |
    (uint32_t)data[offset+2]<<16 | (uint32_t)data[offset+3]<<24;
}

static void fixture_w32(uint8_t *data,size_t offset,uint32_t value){
  data[offset]=(uint8_t)value; data[offset+1]=(uint8_t)(value>>8);
  data[offset+2]=(uint8_t)(value>>16); data[offset+3]=(uint8_t)(value>>24);
}

/* Re-express the compiler fixture's tagged TMLC records in the independently parsed native
 * [name,count,(step,event-pointer)*count] shape. CODE-name resolution is intentional here: native
 * packages use event graphs while compiler fixtures use direct code indices, and the runtime must
 * not require either representation when the stable timeline CODE identity is available. */
static int expect_native_timeline_import(const char *path){
  GmlWin source;
  if(gml_win_load(&source,path)) return 0;
  uint8_t *data=malloc(source.size?source.size:1);
  if(!data){ gml_win_free(&source); return 0; }
  memcpy(data,source.data,source.size);
  const GmlChunk *chunk=gml_chunk(&source,"TMLN");
  size_t end=chunk?(size_t)chunk->off+chunk->size:0;
  uint32_t count=chunk&&chunk->size>=4?fixture_u32(data,chunk->off):0;
  int valid=chunk && count>0 && (size_t)chunk->off+4+(size_t)count*4<=end;
  for(uint32_t i=0;valid && i<count;i++){
    uint32_t entry=fixture_u32(data,chunk->off+4+i*4);
    if((size_t)entry+12>end || fixture_u32(data,entry+4)!=0x434C4D54u){ valid=0; break; }
    uint32_t moments=fixture_u32(data,entry+8);
    if(moments>100000 || (size_t)entry+12+(size_t)moments*8>end){ valid=0; break; }
    fixture_w32(data,entry+4,moments);
    for(uint32_t m=0;m<moments;m++){
      fixture_w32(data,entry+8+m*8,fixture_u32(data,entry+12+m*8));
      fixture_w32(data,entry+12+m*8,0); /* CODE name, not this synthetic pointer, owns identity. */
    }
  }
  size_t size=source.size;
  gml_win_free(&source);
  if(!valid){ free(data); return 0; }
  GmlWin win;
  if(gml_win_from_mem(&win,data,size,1)) { free(data); return 0; }
  GmlVM vm;
  if(gml_vm_init(&vm,&win)){ gml_win_free(&win); return 0; }
  int ok=vm.n_timelines==1 && vm.timelines[0].name &&
    !strcmp(vm.timelines[0].name,"fixture_timeline") && vm.timelines[0].n==3 &&
    vm.timelines[0].moments[0].step==0 && vm.timelines[0].moments[0].code>=0 &&
    vm.timelines[0].moments[1].step==2 && vm.timelines[0].moments[1].code>=0 &&
    vm.timelines[0].moments[2].step==4 && vm.timelines[0].moments[2].code>=0;
  if(ok){
    gml_room_enter(&vm,0);
    GmlInstance *probe=find_slot(&vm,100000);
    if(!probe) ok=0;
    else {
      *gml_varmap_put(&vm.globals,"timeline_order")=vreal(0);
      probe->timeline_index=0; probe->timeline_position=0; probe->timeline_speed=1;
      probe->timeline_running=1; probe->timeline_loop=0;
      gml_vm_step(&vm); gml_vm_step(&vm); gml_vm_step(&vm);
      GmlVal *order=gml_varmap_get(&vm.globals,"timeline_order");
      ok=order && order->t==V_REAL && order->d==12;
    }
  }
  if(!ok) fprintf(stderr,"native timeline import fixture failed\n");
  gml_vm_free(&vm); gml_win_free(&win);
  return ok;
}


static double global_array_value(GmlVM *vm,const char *name,int index){
  GmlVal *value=gml_varmap_get(&vm->globals,name);
  GmlArr *array=value&&value->t==V_ARR?(GmlArr*)value->arr:NULL;
  return array&&index>=0&&index<array->len&&array->data[index].t==V_REAL?array->data[index].d:0;
}

int main(void){
  {
    GmlSprite sprite={0}; GmlRender render={0};
    render.spr=&sprite; render.n_spr=1;
    sprite.playback_speed_valid=1; sprite.playback_speed=15.0f; sprite.playback_speed_type=0;
    double per_second=gml_sprite_animation_delta(&render,0,1.0,60.0);
    sprite.playback_speed=0.5f; sprite.playback_speed_type=1;
    double per_frame=gml_sprite_animation_delta(&render,0,1.0,60.0);
    sprite.playback_speed_valid=0;
    double legacy=gml_sprite_animation_delta(&render,0,0.75,60.0);
    if(fabs(per_second-0.25)>1e-12 || fabs(per_frame-0.5)>1e-12 || fabs(legacy-0.75)>1e-12){
      fprintf(stderr,"sprite playback cadence mismatch: fps=%.6f frame=%.6f legacy=%.6f\n",
        per_second,per_frame,legacy); return 1;
    }
  }
  {
    GmlRender render={0};
    render.n_fonts=1; render.n_atlas=1;
    render.atlas=calloc(1,sizeof(*render.atlas));
    if(!render.atlas) return 1;
    render.fonts[0].real=1; render.fonts[0].runtime_owned=1; render.fonts[0].atlas=0;
    render.fonts[0].glyphs=calloc(1,sizeof(*render.fonts[0].glyphs));
    render.atlas[0].px=calloc(4,4);
    if(!render.fonts[0].glyphs || !render.atlas[0].px) return 1;
    gml_font_delete(&render,0);
    if(render.fonts[0].glyphs || render.fonts[0].sprite!=-1 || render.fonts[0].atlas!=-1 ||
       render.atlas[0].px){
      fprintf(stderr,"runtime font deletion retained owned storage\n"); return 1;
    }
    free(render.atlas);
  }
  {
    /* Vertical centring uses the complete glyph-cell extent while line_height remains the
     * authored line advance. A font can legitimately have descenders taller than its nominal
     * em; centring only the advance moves every visible glyph down. */
    GmlRender render={0}; GmlGlyph glyph={0}; GmlAtlas atlas={0};
    uint32_t framebuffer[16*16]={0}; uint8_t pixels[8*4];
    memset(pixels,255,sizeof(pixels));
    render.fbw=render.fbh=16; render.fb=render.base_fb=framebuffer;
    render.n_fonts=1; render.n_atlas=1; render.atlas=&atlas;
    render.font=0; render.valign=1; render.color=0xFFFFFF; render.alpha=1;
    render.alphablend=1; render.software_overlay=1;
    atlas.px=pixels; atlas.w=1; atlas.h=8; atlas.decode_attempted=1;
    memset(render.fonts[0].glyph_by_char,0xFF,sizeof(render.fonts[0].glyph_by_char));
    render.fonts[0].real=1; render.fonts[0].atlas=0;
    render.fonts[0].line_height=4; render.fonts[0].align_height=8;
    render.fonts[0].glyphs=&glyph; render.fonts[0].n_glyphs=1;
    render.fonts[0].glyphs_sorted=1; render.fonts[0].glyph_by_char['A']=0;
    glyph.ch='A'; glyph.w=1; glyph.h=8; glyph.shift=1;
    gml_draw_text(&render,8,10,"A");
    int min_y=16,max_y=-1;
    for(int y=0;y<16;y++) for(int x=0;x<16;x++) if(framebuffer[y*16+x]&0xFFFFFFu){
      if(y<min_y) min_y=y; if(y>max_y) max_y=y;
    }
    if(min_y!=6 || max_y!=13){
      fprintf(stderr,"real-font centred extent mismatch: y=%d..%d\n",min_y,max_y); return 1;
    }
  }
  GmlcProject project; GmlcObject objects[3]; GmlcRoom rooms[2];
  GmlcScript scripts[2]; char *script_order[2];
  GmlcPath fixture_path; GmlcPathPoint fixture_path_points[2];
  GmlcTimeline timeline; GmlcTimelineMoment timeline_moments[3];
  GmlcRoomInstance placed_instance;
  int room_order[2]={0,1};
  GmlcObjectEvent object_events[25];
  GmlcProjectTrigger trigger;
  GmlcProjectIncludedFile included;
  unsigned char included_data[]={1,3,5,7};
  char included_name[64];
  GmlcProjectConstant constant={(char*)"fixture_constant",(char*)"6*7"};
  memset(&project,0,sizeof(project)); memset(objects,0,sizeof(objects)); memset(rooms,0,sizeof(rooms));
  memset(scripts,0,sizeof(scripts));
  memset(&fixture_path,0,sizeof(fixture_path)); memset(fixture_path_points,0,sizeof(fixture_path_points));
  memset(&timeline,0,sizeof(timeline)); memset(timeline_moments,0,sizeof(timeline_moments));
  memset(&placed_instance,0,sizeof(placed_instance));
  memset(object_events,0,sizeof(object_events)); memset(&trigger,0,sizeof(trigger)); memset(&included,0,sizeof(included));
  project.name="persistent-room-fixture"; project.objects=objects; project.n_objects=3;
  project.classic_version=800;
  scripts[0].id=scripts[0].name=(char*)"script_implicit_result";
  scripts[1].id=scripts[1].name=(char*)"crear";
  script_order[0]=scripts[0].id;
  script_order[1]=scripts[1].id;
  project.scripts=scripts; project.n_scripts=project.cap_scripts=2;
  project.script_order_ids=script_order; project.n_script_order=2;
  fixture_path.id=fixture_path.name=(char*)"fixture_path";
  fixture_path.precision=4; fixture_path.points=fixture_path_points; fixture_path.n_points=2;
  fixture_path_points[0].speed=fixture_path_points[1].speed=100;
  fixture_path_points[1].x=100;
  project.paths=&fixture_path; project.n_paths=project.cap_paths=1;
  timeline.id=timeline.name=(char*)"fixture_timeline";
  timeline.moments=timeline_moments; timeline.n_moments=timeline.cap_moments=3;
  timeline_moments[0].step=0; timeline_moments[1].step=2; timeline_moments[2].step=4;
  project.timelines=&timeline; project.n_timelines=project.cap_timelines=1;
  project.constants=&constant; project.n_constants=project.cap_constants=1;
  project.triggers=&trigger; project.n_triggers=project.cap_triggers=1;
  snprintf(included_name,sizeof(included_name),"gml-included-%ld.dat",(long)getpid());
  included.file_name=included_name; included.data=included_data; included.data_size=sizeof(included_data);
  included.export_mode=2; included.overwrite_file=1;
  project.included_files=&included; project.n_included_files=project.cap_included_files=1;
  project.rooms=rooms; project.n_rooms=2; project.room_order=room_order; project.n_room_order=2;
  objects[0].name="obj_fixture"; objects[0].sprite_id=-1; objects[0].mask_id=-1; objects[0].parent_id=-1; objects[0].visible=1;
  objects[0].events=object_events; objects[0].n_events=objects[0].cap_events=10;
  objects[1].name="obj_changed"; objects[1].sprite_id=-1; objects[1].mask_id=-1; objects[1].parent_id=-1; objects[1].visible=1;
  objects[1].events=&object_events[10]; objects[1].n_events=objects[1].cap_events=11;
  objects[2].name="obj_create_order"; objects[2].sprite_id=-1; objects[2].mask_id=-1; objects[2].parent_id=-1; objects[2].visible=0;
  objects[2].events=&object_events[21]; objects[2].n_events=objects[2].cap_events=4;
  object_events[0].event_type=11; object_events[0].event_number=0;
  object_events[1].event_type=3; object_events[1].event_number=0;
  object_events[2].event_type=2; object_events[2].event_number=0;
  object_events[3].event_type=2; object_events[3].event_number=1;
  object_events[4].event_type=5; object_events[4].event_number=65;
  object_events[5].event_type=6; object_events[5].event_number=50;
  object_events[6].event_type=7; object_events[6].event_number=0;
  object_events[7].event_type=7; object_events[7].event_number=1;
  object_events[8].event_type=7; object_events[8].event_number=41;
  object_events[9].event_type=7; object_events[9].event_number=51;
  object_events[10].event_type=3; object_events[10].event_number=2;
  object_events[11].event_type=3; object_events[11].event_number=0;
  object_events[12].event_type=2; object_events[12].event_number=0;
  object_events[13].event_type=2; object_events[13].event_number=1;
  object_events[14].event_type=5; object_events[14].event_number=65;
  object_events[15].event_type=6; object_events[15].event_number=50;
  object_events[16].event_type=7; object_events[16].event_number=0;
  object_events[17].event_type=7; object_events[17].event_number=1;
  object_events[18].event_type=7; object_events[18].event_number=41;
  object_events[19].event_type=7; object_events[19].event_number=51;
  object_events[20].event_type=11; object_events[20].event_number=0;
  object_events[21].event_type=0; object_events[21].event_number=0;
  object_events[22].event_type=6; object_events[22].event_number=23;
  object_events[23].event_type=4; object_events[23].event_number=1;
  object_events[23].collision_object_id=1;
  object_events[24].event_type=1; object_events[24].event_number=0;
  trigger.name=(char*)"fixture_trigger"; trigger.moment=1; trigger.runtime_id=0;
  for(int i=0;i<2;i++){
    rooms[i].name=i?"room_b":"room_a"; rooms[i].width=320; rooms[i].height=240; rooms[i].speed=30;
    rooms[i].view_enabled=1; rooms[i].n_views=2;
    for(int v=0;v<2;v++){
      rooms[i].views[v].wview=rooms[i].views[v].wport=v?100:320;
      rooms[i].views[v].hview=rooms[i].views[v].hport=v?100:240;
      rooms[i].views[v].hspeed=rooms[i].views[v].vspeed=-1;
      rooms[i].views[v].object_id=-1;
    }
  }
  rooms[1].view_enabled=0;
  rooms[0].persistent=1;
  placed_instance.id=(char*)"placed_create_order";
  placed_instance.name=(char*)"placed_create_order";
  placed_instance.object_id=2; placed_instance.instance_id=100000;
  rooms[0].instances=&placed_instance; rooms[0].n_instances=rooms[0].cap_instances=1;
  char startup[]="/tmp/gml-startup-XXXXXX"; int startup_fd=mkstemp(startup); if(startup_fd<0)return 1;
  FILE *startup_file=fdopen(startup_fd,"wb");
  const char startup_source[]=
    "global.startup_value = fixture_constant; "
    "global.date_minute_span_fixture = date_minute_span(100, 99.5); "
    "global.view_fixture_scalar = 7; global.background_fixture_scalar = 9; "
    "view_enabled[0] = true; global.view_enabled_alias = view_enabled[1]; "
    "global.delta_fixture = delta_time; "
    "global.working_fixture = working_directory; global.program_fixture = program_directory; "
    "global.fixture_orange = c_orange; global.fixture_rain = ef_rain;\n";
  if(!startup_file || fwrite(startup_source,1,sizeof(startup_source)-1,startup_file)!=sizeof(startup_source)-1 ||
     fclose(startup_file)!=0){ unlink(startup); return 1; }
  project.startup_code_path=startup;
  char implicit_script[]="/tmp/gml-implicit-script-XXXXXX";
  int implicit_script_fd=mkstemp(implicit_script); if(implicit_script_fd<0)return 1;
  FILE *implicit_script_file=fdopen(implicit_script_fd,"wb");
  const char implicit_script_source[]=
    "if (argument0 == -4) return (1 == 1.000001); "
    "transition_kind=12; transition_steps=40; "
    "if (argument0 >= 0) instance_exists(argument0); "
    "else if (argument0 == -2) return all.fixture_all_scope; "
    "else if (argument0 == -3) { all.fixture_all_scope=77; return 77; } "
    "else global.implicit_assignment = 42;\n";
  if(!implicit_script_file ||
     fwrite(implicit_script_source,1,sizeof(implicit_script_source)-1,implicit_script_file)!=sizeof(implicit_script_source)-1 ||
     fclose(implicit_script_file)!=0)return 1;
  scripts[0].source_path=implicit_script;
  char shadowed_alias_script[]="/tmp/gml-shadowed-alias-script-XXXXXX";
  int shadowed_alias_fd=mkstemp(shadowed_alias_script); if(shadowed_alias_fd<0)return 1;
  FILE *shadowed_alias_file=fdopen(shadowed_alias_fd,"wb");
  const char shadowed_alias_source[]="global.user_crear_hits += argument0;\n";
  if(!shadowed_alias_file ||
     fwrite(shadowed_alias_source,1,sizeof(shadowed_alias_source)-1,shadowed_alias_file)!=sizeof(shadowed_alias_source)-1 ||
     fclose(shadowed_alias_file)!=0)return 1;
  scripts[1].source_path=shadowed_alias_script;
  char condition[]="/tmp/gml-trigger-condition-XXXXXX"; int condition_fd=mkstemp(condition); if(condition_fd<0)return 1;
  FILE *condition_file=fdopen(condition_fd,"wb"); const char condition_source[]="return (global.startup_value == 42);\n";
  if(!condition_file || fwrite(condition_source,1,sizeof(condition_source)-1,condition_file)!=sizeof(condition_source)-1 || fclose(condition_file)!=0)return 1;
  trigger.condition_path=condition;
  char event[]="/tmp/gml-trigger-event-XXXXXX"; int event_fd=mkstemp(event); if(event_fd<0)return 1;
  FILE *event_file=fdopen(event_fd,"wb");
  const char event_source[]="global.trigger_hits += 1; global.trigger_order=global.trigger_order*10+1;\n";
  if(!event_file || fwrite(event_source,1,sizeof(event_source)-1,event_file)!=sizeof(event_source)-1 || fclose(event_file)!=0)return 1;
  object_events[0].source_path=event;
  char changed_trigger[]="/tmp/gml-trigger-changed-XXXXXX"; int changed_trigger_fd=mkstemp(changed_trigger); if(changed_trigger_fd<0)return 1;
  FILE *changed_trigger_file=fdopen(changed_trigger_fd,"wb");
  const char changed_trigger_source[]="global.trigger_hits += 1; global.trigger_order=global.trigger_order*10+2;\n";
  if(!changed_trigger_file || fwrite(changed_trigger_source,1,sizeof(changed_trigger_source)-1,changed_trigger_file)!=sizeof(changed_trigger_source)-1 ||
     fclose(changed_trigger_file)!=0)return 1;
  object_events[20].source_path=changed_trigger;
  char create_order[]="/tmp/gml-create-order-XXXXXX"; int create_order_fd=mkstemp(create_order); if(create_order_fd<0)return 1;
  FILE *create_order_file=fdopen(create_order_fd,"wb");
  const char create_order_source[]=
    "global.create_order=global.create_order*10+2; "
    "crear(5); "
    "image_speed=0.5; image_single[0]=7; global.image_single_indexed=image_single[13];\n";
  if(!create_order_file || fwrite(create_order_source,1,sizeof(create_order_source)-1,create_order_file)!=sizeof(create_order_source)-1 ||
     fclose(create_order_file)!=0)return 1;
  object_events[21].source_path=create_order;
  char joystick_event[]="/tmp/gml-joystick-event-XXXXXX"; int joystick_event_fd=mkstemp(joystick_event); if(joystick_event_fd<0)return 1;
  FILE *joystick_event_file=fdopen(joystick_event_fd,"wb");
  const char joystick_event_source[]="global.joystick_event_hits += 1;\n";
  if(!joystick_event_file || fwrite(joystick_event_source,1,sizeof(joystick_event_source)-1,joystick_event_file)!=sizeof(joystick_event_source)-1 ||
     fclose(joystick_event_file)!=0)return 1;
  object_events[22].source_path=joystick_event;
  char solid_collision[]="/tmp/gml-solid-collision-XXXXXX";
  int solid_collision_fd=mkstemp(solid_collision); if(solid_collision_fd<0)return 1;
  FILE *solid_collision_file=fdopen(solid_collision_fd,"wb");
  const char solid_collision_source[]="y -= 1; global.studio_solid_hits += 1;\n";
  if(!solid_collision_file ||
     fwrite(solid_collision_source,1,sizeof(solid_collision_source)-1,solid_collision_file)!=sizeof(solid_collision_source)-1 ||
     fclose(solid_collision_file)!=0)return 1;
  object_events[23].source_path=solid_collision;
  char destroy_reentry[]="/tmp/gml-destroy-reentry-XXXXXX";
  int destroy_reentry_fd=mkstemp(destroy_reentry); if(destroy_reentry_fd<0)return 1;
  FILE *destroy_reentry_file=fdopen(destroy_reentry_fd,"wb");
  const char destroy_reentry_source[]=
    "global.destroy_reentry_hits += 1; instance_destroy();\n";
  if(!destroy_reentry_file ||
     fwrite(destroy_reentry_source,1,sizeof(destroy_reentry_source)-1,destroy_reentry_file)!=sizeof(destroy_reentry_source)-1 ||
     fclose(destroy_reentry_file)!=0)return 1;
  object_events[24].source_path=destroy_reentry;
  char instance_order[]="/tmp/gml-instance-order-XXXXXX"; int instance_order_fd=mkstemp(instance_order); if(instance_order_fd<0)return 1;
  FILE *instance_order_file=fdopen(instance_order_fd,"wb");
  const char instance_order_source[]="global.create_order=global.create_order*10+1;\n";
  if(!instance_order_file || fwrite(instance_order_source,1,sizeof(instance_order_source)-1,instance_order_file)!=sizeof(instance_order_source)-1 ||
     fclose(instance_order_file)!=0)return 1;
  placed_instance.creation_code_path=instance_order;
  char step[]="/tmp/gml-step-event-XXXXXX"; int step_fd=mkstemp(step); if(step_fd<0)return 1;
  FILE *step_file=fdopen(step_fd,"wb");
  const char step_source[]=
    "global.step_order=global.step_order*10+1; "
    "persistent_array[0]=7; "
    "var persistent_alias=persistent_array; "
    "global.persistent_alias_len=array_length_1d(persistent_alias); "
    "global.persistent_alias_value=persistent_alias[0]; "
    "if (!global.spawned_once) { global.spawned_once=1; "
    "with(instance_create(50,50,obj_changed)){ hspeed=3; } } "
    "if (global.churn_pool) { with(instance_create(70,70,obj_changed)){ instance_destroy(); } } "
    "if (hspeed > 0) hspeed -= 1; if (hspeed < 0) hspeed += 1; "
    "hspeed = round(hspeed); x += 5;\n";
  if(!step_file || fwrite(step_source,1,sizeof(step_source)-1,step_file)!=sizeof(step_source)-1 || fclose(step_file)!=0)return 1;
  object_events[1].source_path=step;
  char end_step[]="/tmp/gml-end-step-event-XXXXXX"; int end_step_fd=mkstemp(end_step); if(end_step_fd<0)return 1;
  FILE *end_step_file=fdopen(end_step_fd,"wb"); const char end_step_source[]="x += 7;\n";
  if(!end_step_file || fwrite(end_step_source,1,sizeof(end_step_source)-1,end_step_file)!=sizeof(end_step_source)-1 || fclose(end_step_file)!=0)return 1;
  object_events[10].source_path=end_step;
  char changed_step[]="/tmp/gml-changed-step-event-XXXXXX"; int changed_step_fd=mkstemp(changed_step); if(changed_step_fd<0)return 1;
  FILE *changed_step_file=fdopen(changed_step_fd,"wb");
  const char changed_step_source[]=
    "global.step_order=global.step_order*10+2; global.changed_step_hits += 1; "
    "global.engine_event_other_is_self=(other.id==id); "
    "global.nested_empty_with=0; "
    "with(other.id){ global.nested_empty_with=1; "
    "with(noone){ global.nested_empty_with=-1; } "
    "global.nested_empty_with=12; }\n";
  if(!changed_step_file || fwrite(changed_step_source,1,sizeof(changed_step_source)-1,changed_step_file)!=sizeof(changed_step_source)-1 || fclose(changed_step_file)!=0)return 1;
  object_events[11].source_path=changed_step;
  char alarm_files[4][40];
  const char *alarm_sources[4]={
    "global.alarm_order=global.alarm_order*10+1;\n",
    "global.alarm_order=global.alarm_order*10+3;\n",
    "global.alarm_order=global.alarm_order*10+2;\n",
    "global.alarm_order=global.alarm_order*10+4;\n"
  };
  const int alarm_events[4]={2,3,12,13};
  for(int i=0;i<4;i++){
    snprintf(alarm_files[i],sizeof(alarm_files[i]),"/tmp/gml-alarm-order-%d-XXXXXX",i);
    int alarm_fd=mkstemp(alarm_files[i]); if(alarm_fd<0)return 1;
    FILE *alarm_file=fdopen(alarm_fd,"wb"); size_t alarm_len=strlen(alarm_sources[i]);
    if(!alarm_file || fwrite(alarm_sources[i],1,alarm_len,alarm_file)!=alarm_len || fclose(alarm_file)!=0)return 1;
    object_events[alarm_events[i]].source_path=alarm_files[i];
  }
  char key_files[2][40]; const int key_events[2]={4,14};
  for(int i=0;i<2;i++){
    snprintf(key_files[i],sizeof(key_files[i]),"/tmp/gml-key-order-%d-XXXXXX",i);
    int key_fd=mkstemp(key_files[i]); if(key_fd<0)return 1;
    FILE *key_file=fdopen(key_fd,"wb");
    char key_source[64]; int key_len=snprintf(key_source,sizeof(key_source),
      "global.key_order=global.key_order*10+%d;\n",i+1);
    if(!key_file || fwrite(key_source,1,(size_t)key_len,key_file)!=(size_t)key_len || fclose(key_file)!=0)return 1;
    object_events[key_events[i]].source_path=key_files[i];
  }
  char mouse_files[2][40]; const int mouse_events[2]={5,15};
  for(int i=0;i<2;i++){
    snprintf(mouse_files[i],sizeof(mouse_files[i]),"/tmp/gml-mouse-order-%d-XXXXXX",i);
    int mouse_fd=mkstemp(mouse_files[i]); if(mouse_fd<0)return 1;
    FILE *mouse_file=fdopen(mouse_fd,"wb");
    char mouse_source[72]; int mouse_len=snprintf(mouse_source,sizeof(mouse_source),
      "global.mouse_order=global.mouse_order*10+%d;\n",i+1);
    if(!mouse_file || fwrite(mouse_source,1,(size_t)mouse_len,mouse_file)!=(size_t)mouse_len || fclose(mouse_file)!=0)return 1;
    object_events[mouse_events[i]].source_path=mouse_files[i];
  }
  char boundary_files[4][44]; const int boundary_events[4]={6,16,7,17};
  for(int i=0;i<4;i++){
    snprintf(boundary_files[i],sizeof(boundary_files[i]),"/tmp/gml-boundary-order-%d-XXXXXX",i);
    int boundary_fd=mkstemp(boundary_files[i]); if(boundary_fd<0)return 1;
    FILE *boundary_file=fdopen(boundary_fd,"wb");
    char boundary_source[80]; int boundary_len=snprintf(boundary_source,sizeof(boundary_source),
      "global.boundary_order=global.boundary_order*10+%d;\n",i+1);
    if(!boundary_file || fwrite(boundary_source,1,(size_t)boundary_len,boundary_file)!=(size_t)boundary_len ||
       fclose(boundary_file)!=0)return 1;
    object_events[boundary_events[i]].source_path=boundary_files[i];
  }
  char view_boundary_files[4][48]; const int view_boundary_events[4]={8,18,9,19};
  for(int i=0;i<4;i++){
    snprintf(view_boundary_files[i],sizeof(view_boundary_files[i]),"/tmp/gml-view-boundary-%d-XXXXXX",i);
    int view_boundary_fd=mkstemp(view_boundary_files[i]); if(view_boundary_fd<0)return 1;
    FILE *view_boundary_file=fdopen(view_boundary_fd,"wb");
    char view_boundary_source[90]; int view_boundary_len=snprintf(view_boundary_source,sizeof(view_boundary_source),
      "global.secondary_boundary_order=global.secondary_boundary_order*10+%d;\n",i+1);
    if(!view_boundary_file || fwrite(view_boundary_source,1,(size_t)view_boundary_len,view_boundary_file)!=(size_t)view_boundary_len ||
       fclose(view_boundary_file)!=0)return 1;
    object_events[view_boundary_events[i]].source_path=view_boundary_files[i];
  }
  char timeline_files[3][44];
  const char *timeline_sources[3]={
    "global.timeline_order=global.timeline_order*10+1; global.dynamic_timeline=timeline_add();\n",
    "global.timeline_order=global.timeline_order*10+2;\n",
    "global.timeline_order=global.timeline_order*10+3;\n"
  };
  for(int i=0;i<3;i++){
    snprintf(timeline_files[i],sizeof(timeline_files[i]),"/tmp/gml-timeline-moment-%d-XXXXXX",i);
    int timeline_fd=mkstemp(timeline_files[i]); if(timeline_fd<0)return 1;
    FILE *timeline_file=fdopen(timeline_fd,"wb"); size_t timeline_len=strlen(timeline_sources[i]);
    if(!timeline_file || fwrite(timeline_sources[i],1,timeline_len,timeline_file)!=timeline_len ||
       fclose(timeline_file)!=0)return 1;
    timeline_moments[i].source_path=timeline_files[i];
  }
  char path[]="/tmp/gml-persistent-room-XXXXXX"; int fd=mkstemp(path); if(fd<0)return 1; close(fd);
  char err[256]={0};
  if(!gmlc_package_write_structural(&project,path,err,sizeof(err))){ fprintf(stderr,"package: %s\n",err); unlink(path); unlink(startup); return 1; }
  if(!expect_native_timeline_import(path)) return 1;
  char included_path[96]; snprintf(included_path,sizeof(included_path),"/tmp/%s",included_name);
  FILE *included_file=fopen(included_path,"rb"); unsigned char observed[sizeof(included_data)]={0};
  int included_ok=included_file && fread(observed,1,sizeof(observed),included_file)==sizeof(observed) &&
                  !memcmp(observed,included_data,sizeof(observed));
  if(included_file) fclose(included_file);
  if(!included_ok){ fprintf(stderr,"included file was not exported\n"); return 1; }
  GmlWin win; if(gml_win_load(&win,path)){ unlink(path); return 1; }
  char save_root[]="/tmp/gml-save-root-XXXXXX";
  if(!mkdtemp(save_root)){ gml_win_free(&win); unlink(path); return 1; }
  snprintf(win.save_dir,sizeof win.save_dir,"%s",save_root);
  GmlVM vm; if(gml_vm_init(&vm,&win)){ gml_win_free(&win); unlink(path); return 1; }
  {
    GmlVal *transition_kind=gml_varmap_get(&vm.globals,"transition_kind");
    GmlVal *transition_steps=gml_varmap_get(&vm.globals,"transition_steps");
    if(!transition_kind || transition_kind->t!=V_REAL || transition_kind->d!=0 ||
       !transition_steps || transition_steps->t!=V_REAL || transition_steps->d!=80){
      fprintf(stderr,"classic transition defaults mismatch: kind=%.0f steps=%.0f\n",
        transition_kind&&transition_kind->t==V_REAL?transition_kind->d:-1.0,
        transition_steps&&transition_steps->t==V_REAL?transition_steps->d:-1.0);
      return 1;
    }
  }
  GmlVal *startup_value=gml_varmap_get(&vm.globals,"startup_value");
  GmlVal *view_fixture_scalar=gml_varmap_get(&vm.globals,"view_fixture_scalar");
  GmlVal *background_fixture_scalar=gml_varmap_get(&vm.globals,"background_fixture_scalar");
  GmlVal *view_enabled_alias=gml_varmap_get(&vm.globals,"view_enabled_alias");
  GmlVal *delta_fixture=gml_varmap_get(&vm.globals,"delta_fixture");
  GmlVal *working_fixture=gml_varmap_get(&vm.globals,"working_fixture");
  GmlVal *program_fixture=gml_varmap_get(&vm.globals,"program_fixture");
  GmlVal *fixture_orange=gml_varmap_get(&vm.globals,"fixture_orange");
  GmlVal *fixture_rain=gml_varmap_get(&vm.globals,"fixture_rain");
  GmlVal *date_minute_span_fixture=gml_varmap_get(&vm.globals,"date_minute_span_fixture");
  char expected_working[640],expected_program[640];
  /* This compiler fixture is a bytecode-15 Studio package: working_directory remains the
   * installed content root, while writes are still redirected through the file sandbox. */
  snprintf(expected_working,sizeof expected_working,"%s/",win.content_dir);
  snprintf(expected_program,sizeof expected_program,"%s/",win.content_dir);
  if(!startup_value || startup_value->t!=V_REAL || startup_value->d!=42 ||
     !view_fixture_scalar || view_fixture_scalar->t!=V_REAL || view_fixture_scalar->d!=7 ||
     !background_fixture_scalar || background_fixture_scalar->t!=V_REAL || background_fixture_scalar->d!=9 ||
     !view_enabled_alias || view_enabled_alias->t!=V_REAL || view_enabled_alias->d!=1 ||
     !delta_fixture || delta_fixture->t!=V_REAL || fabs(delta_fixture->d-1000000.0/60.0)>1e-6 ||
     !working_fixture || working_fixture->t!=V_STR || strcmp(working_fixture->s,expected_working) ||
     !program_fixture || program_fixture->t!=V_STR || strcmp(program_fixture->s,expected_program) ||
     !fixture_orange || fixture_orange->t!=V_REAL || fixture_orange->d!=0x40A0FF ||
     !fixture_rain || fixture_rain->t!=V_REAL || fixture_rain->d!=10 ||
     !date_minute_span_fixture || date_minute_span_fixture->t!=V_REAL ||
       date_minute_span_fixture->d!=720){
    fprintf(stderr,"startup code or project constant did not run: startup=%.9g view=%.9g background=%.9g delta=%.17g orange=%.9g rain=%.9g\n",
      startup_value&&startup_value->t==V_REAL?startup_value->d:-1.0,
      view_fixture_scalar&&view_fixture_scalar->t==V_REAL?view_fixture_scalar->d:-1.0,
      background_fixture_scalar&&background_fixture_scalar->t==V_REAL?background_fixture_scalar->d:-1.0,
      delta_fixture&&delta_fixture->t==V_REAL?delta_fixture->d:-1.0,
      fixture_orange&&fixture_orange->t==V_REAL?fixture_orange->d:-1.0,
      fixture_rain&&fixture_rain->t==V_REAL?fixture_rain->d:-1.0); return 1;
  }
  if(!expect_file_sandbox(&vm,save_root)) return 1;
  gml_room_enter(&vm,0);
  GmlVal *room_view_enabled=gml_varmap_get(&vm.globals,"view_enabled");
  if(!room_view_enabled || room_view_enabled->t!=V_REAL || room_view_enabled->d!=1){
    fprintf(stderr,"room flags did not enable the legacy view system: %.0f\n",
      room_view_enabled&&room_view_enabled->t==V_REAL?room_view_enabled->d:-1.0);
    return 1;
  }
  GmlInstance *timeline_probe=find_slot(&vm,100000); if(!timeline_probe)return 1;
  GmlVal *image_single_indexed=gml_varmap_get(&vm.globals,"image_single_indexed");
  GmlVal *user_crear_hits=gml_varmap_get(&vm.globals,"user_crear_hits");
  if(timeline_probe->image_index!=7 || timeline_probe->image_speed!=0 ||
     !image_single_indexed || image_single_indexed->t!=V_REAL || image_single_indexed->d!=7 ||
     !user_crear_hits || user_crear_hits->t!=V_REAL || user_crear_hits->d!=5){
    fprintf(stderr,"classic indexed image_single or project-script precedence mismatch: index=%.1f speed=%.1f read=%.1f script=%.1f\n",
      timeline_probe->image_index,timeline_probe->image_speed,
      image_single_indexed&&image_single_indexed->t==V_REAL?image_single_indexed->d:-1.0,
      user_crear_hits&&user_crear_hits->t==V_REAL?user_crear_hits->d:-1.0); return 1;
  }
  uint32_t path_next_id=vm.next_id;
  GmlInstance *path_probe=gml_instance_create(&vm,100,100,2); if(!path_probe)return 1;
  GmlInstance *path_control=gml_instance_create(&vm,100,100,2); if(!path_control)return 1;
  GmlInstance *engine_other_probe=gml_instance_create(&vm,110,100,1); if(!engine_other_probe)return 1;
  gml_run_event(&vm,engine_other_probe,"Step_0");
  GmlVal *engine_event_other_is_self=gml_varmap_get(&vm.globals,"engine_event_other_is_self");
  if(!engine_event_other_is_self || engine_event_other_is_self->t!=V_REAL ||
     engine_event_other_is_self->d!=1){
    fprintf(stderr,"engine event did not expose self as other: %.0f\n",
      engine_event_other_is_self&&engine_event_other_is_self->t==V_REAL?
      engine_event_other_is_self->d:-1.0);
    return 1;
  }
  GmlVal *nested_empty_with=gml_varmap_get(&vm.globals,"nested_empty_with");
  if(!nested_empty_with || nested_empty_with->t!=V_REAL || nested_empty_with->d!=12){
    fprintf(stderr,"empty nested with closed its parent scope: %.0f\n",
      nested_empty_with&&nested_empty_with->t==V_REAL?nested_empty_with->d:-1.0);
    return 1;
  }
  gml_instance_destroy(&vm,engine_other_probe);
  gml_set_global_scalar(&vm,"step_order",0);
  gml_set_global_scalar(&vm,"create_order",12);
  uint32_t path_probe_id=path_probe->id, path_control_id=path_control->id;
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control)return 1;
  gml_path_start(&vm,path_probe,0,10,0,0);
  gml_path_start(&vm,path_control,0,10,0,0);
  path_probe->hspeed=7; path_probe->vspeed=0; path_probe->speed=7; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control)return 1;
  if(fabs(path_probe->x-path_control->x)>1e-6 || fabs(path_probe->y-path_control->y)>1e-6 ||
     path_probe->hspeed!=0 || path_probe->vspeed!=0 || path_probe->speed!=0 ||
     fabs(path_probe->direction)>1e-9){
    fprintf(stderr,"path motion retained stale ordinary velocity: probe=(%.6f,%.6f) control=(%.6f,%.6f) direction=%.6f velocity=(%.6f,%.6f) speed=%.6f\n",
      path_probe->x,path_probe->y,path_control->x,path_control->y,path_probe->direction,
      path_probe->hspeed,path_probe->vspeed,path_probe->speed);
    return 1;
  }
  double paused_start=path_probe->x;
  path_probe->path_speed=0; path_probe->hspeed=2; path_probe->speed=2; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control)return 1;
  if(fabs(path_probe->x-(paused_start+2))>1e-6 || path_probe->hspeed!=2 || path_probe->speed!=2){
    fprintf(stderr,"paused path suppressed ordinary velocity: start=%.6f expected=%.6f x=%.6f hspeed=%.6f speed=%.6f\n",
      paused_start,paused_start+2,path_probe->x,path_probe->hspeed,path_probe->speed); return 1;
  }
  gml_instance_destroy(&vm,path_probe); gml_instance_destroy(&vm,path_control);

  /* Bytecode 15 keeps ordinary velocity observable while a path is active.  A paused path is
   * still evaluated after automatic movement, restoring the instance to the current path point. */
  win.classic_version=0; win.bytecode=15;
  path_probe=gml_instance_create(&vm,100,100,2); if(!path_probe)return 1;
  path_control=gml_instance_create(&vm,100,100,2); if(!path_control)return 1;
  path_probe_id=path_probe->id; path_control_id=path_control->id;
  gml_path_start(&vm,path_probe,0,10,0,0);
  gml_path_start(&vm,path_control,0,10,0,0);
  path_probe->hspeed=7; path_probe->vspeed=0; path_probe->speed=7; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control || fabs(path_probe->x-path_control->x)>1e-6 ||
     fabs(path_probe->y-path_control->y)>1e-6 || path_probe->hspeed!=7 || path_probe->speed!=7){
    fprintf(stderr,"Studio 1.x path did not retain ordinary velocity: probe=(%.6f,%.6f) control=(%.6f,%.6f) velocity=(%.6f,%.6f) speed=%.6f\n",
      path_probe?path_probe->x:-1.0,path_probe?path_probe->y:-1.0,
      path_control?path_control->x:-1.0,path_control?path_control->y:-1.0,
      path_probe?path_probe->hspeed:-1.0,path_probe?path_probe->vspeed:-1.0,
      path_probe?path_probe->speed:-1.0); return 1;
  }
  paused_start=path_probe->x;
  path_probe->path_speed=0; path_probe->hspeed=2; path_probe->speed=2; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id);
  if(!path_probe || fabs(path_probe->x-paused_start)>1e-6 ||
     path_probe->hspeed!=2 || path_probe->speed!=2){
    fprintf(stderr,"Studio 1.x paused path did not override ordinary displacement: start=%.6f x=%.6f hspeed=%.6f speed=%.6f\n",
      paused_start,path_probe?path_probe->x:-1.0,path_probe?path_probe->hspeed:-1.0,
      path_probe?path_probe->speed:-1.0); return 1;
  }
  gml_instance_destroy(&vm,path_probe); gml_instance_destroy(&vm,path_control);
  gml_set_global_scalar(&vm,"create_order",12);
  win.classic_version=800; win.bytecode=15;
  GmlVal *destroy_reentry_hits=gml_varmap_get(&vm.globals,"destroy_reentry_hits");
  if(!path_probe->marked || !path_control->marked || !destroy_reentry_hits ||
     destroy_reentry_hits->t!=V_REAL || destroy_reentry_hits->d!=4){
    fprintf(stderr,"recursive Destroy event was not single-shot: marked=(%d,%d) hits=%.0f\n",
      path_probe->marked,path_control->marked,
      destroy_reentry_hits&&destroy_reentry_hits->t==V_REAL?destroy_reentry_hits->d:-1.0);
    return 1;
  }
  vm.next_id=path_next_id;
  *gml_varmap_put(&vm.globals,"timeline_order")=vreal(0);
  timeline_probe->timeline_index=0; timeline_probe->timeline_position=0;
  timeline_probe->timeline_speed=-1; timeline_probe->timeline_running=1; timeline_probe->timeline_loop=0;
  gml_vm_step(&vm);
  GmlVal *timeline_order=gml_varmap_get(&vm.globals,"timeline_order");
  if(!timeline_order || timeline_order->t!=V_REAL || timeline_order->d!=1 ||
     timeline_probe->timeline_position!=0 || timeline_probe->timeline_running!=0){
    fprintf(stderr,"reverse timeline did not fire its initial moment: order=%.0f pos=%.1f running=%.0f\n",
      timeline_order&&timeline_order->t==V_REAL?timeline_order->d:-1.0,
      timeline_probe->timeline_position,timeline_probe->timeline_running); return 1;
  }
  timeline_order->d=0; timeline_probe->timeline_position=0; timeline_probe->timeline_speed=1;
  timeline_probe->timeline_running=1; timeline_probe->timeline_loop=0;
  gml_vm_step(&vm); gml_vm_step(&vm); gml_vm_step(&vm);
  if(timeline_order->d!=12 || timeline_probe->timeline_position!=3 || !timeline_probe->timeline_running){
    fprintf(stderr,"forward timeline endpoint ownership mismatch: order=%.0f pos=%.1f running=%.0f\n",
      timeline_order->d,timeline_probe->timeline_position,timeline_probe->timeline_running); return 1;
  }
  timeline_order->d=0; timeline_probe->timeline_position=4; timeline_probe->timeline_speed=2;
  timeline_probe->timeline_running=1; timeline_probe->timeline_loop=1;
  gml_vm_step(&vm);
  if(timeline_order->d!=31 || timeline_probe->timeline_position!=1){
    fprintf(stderr,"forward timeline wrap mismatch: order=%.0f pos=%.1f\n",
      timeline_order->d,timeline_probe->timeline_position); return 1;
  }
  timeline_order->d=0; timeline_probe->timeline_position=0; timeline_probe->timeline_speed=-2;
  timeline_probe->timeline_running=1; timeline_probe->timeline_loop=1;
  gml_vm_step(&vm);
  if(timeline_order->d!=13 || timeline_probe->timeline_position!=3){
    fprintf(stderr,"reverse timeline wrap mismatch: order=%.0f pos=%.1f\n",
      timeline_order->d,timeline_probe->timeline_position); return 1;
  }
  timeline_probe->timeline_running=0; timeline_probe->timeline_loop=0;
  if(vm.n_timelines<2 || !vm.timelines[vm.n_timelines-1].name){
    fprintf(stderr,"timeline_add from a running moment did not append an asset\n"); return 1;
  }
  int timeline_count=vm.n_timelines;
  GmlVal added_timeline=gml_builtin_call(&vm,"timeline_add",NULL,0);
  GmlVal added_arg=added_timeline;
  GmlVal added_exists=gml_builtin_call(&vm,"timeline_exists",&added_arg,1);
  if(added_timeline.t!=V_REAL || added_timeline.d!=timeline_count ||
     added_exists.t!=V_REAL || added_exists.d!=1 || vm.n_timelines!=timeline_count+1){
    fprintf(stderr,"runtime timeline allocation mismatch: index=%.0f count=%d exists=%.0f\n",
      added_timeline.d,vm.n_timelines,added_exists.d); return 1;
  }
  GmlVal imported_timeline=vreal(0);
  (void)gml_builtin_call(&vm,"timeline_clear",&imported_timeline,1);
  GmlVal imported_exists=gml_builtin_call(&vm,"timeline_exists",&imported_timeline,1);
  if(vm.timelines[0].n!=0 || vm.timelines[0].last_step!=-1 ||
     imported_exists.t!=V_REAL || imported_exists.d!=1){
    fprintf(stderr,"timeline_clear removed the asset or retained its moments\n"); return 1;
  }
  int implicit_code=gml_code_index_by_name(&win,"gml_Script_script_implicit_result");
  GmlVal implicit_arg=vreal(2);
  GmlVal implicit_result=implicit_code>=0?
    gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1):vreal(0);
  if(implicit_code<0 || implicit_result.t!=V_REAL || implicit_result.d!=1){
    fprintf(stderr,"classic script implicit result mismatch: code=%d result=%.0f\n",
      implicit_code,implicit_result.t==V_REAL?implicit_result.d:-1.0); return 1;
  }
  {
    int saved_classic=win.classic_version, saved_bytecode=win.bytecode;
    double saved_epsilon=vm.math_epsilon;
    implicit_arg=vreal(-4);
    win.classic_version=0; win.bytecode=15; vm.math_epsilon=1e-5;
    GmlVal studio1_compare=gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1);
    win.bytecode=17;
    GmlVal studio2_compare=gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1);
    win.classic_version=saved_classic; win.bytecode=saved_bytecode;
    vm.math_epsilon=saved_epsilon;
    if(studio1_compare.t!=V_REAL || studio1_compare.d!=0 ||
       studio2_compare.t!=V_REAL || studio2_compare.d!=1){
      fprintf(stderr,"Studio comparison-family epsilon mismatch: bytecode15=%.0f bytecode17=%.0f\n",
        studio1_compare.t==V_REAL?studio1_compare.d:-1.0,
        studio2_compare.t==V_REAL?studio2_compare.d:-1.0); return 1;
    }
  }
  if(gml_global_num(&vm,"transition_kind")!=12 || gml_global_num(&vm,"transition_steps")!=40){
    fprintf(stderr,"classic unqualified transition variables did not route globally: kind=%.0f steps=%.0f\n",
      gml_global_num(&vm,"transition_kind"),gml_global_num(&vm,"transition_steps")); return 1;
  }
  gml_set_global_scalar(&vm,"transition_kind",0);
  gml_set_global_scalar(&vm,"transition_steps",80);
  implicit_arg=vreal(-1);
  implicit_result=gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1);
  if(implicit_result.t!=V_REAL || implicit_result.d!=42){
    fprintf(stderr,"classic script implicit assignment result mismatch: %.0f\n",
      implicit_result.t==V_REAL?implicit_result.d:-1.0); return 1;
  }
  GmlVal *create_order_value=gml_varmap_get(&vm.globals,"create_order");
  if(!create_order_value || create_order_value->t!=V_REAL || create_order_value->d!=12){
    fprintf(stderr,"classic instance/Create order mismatch: %.0f\n",
      create_order_value&&create_order_value->t==V_REAL?create_order_value->d:-1.0); return 1;
  }
  GmlInstance *created=gml_instance_create(&vm,12,34,0); if(!created)return 1;
  if(created->id!=100001){
    fprintf(stderr,"placed instances consumed a dynamic instance id: %u\n",created->id); return 1;
  }
  int exact_family=gml_instance_number(&vm,0);
  GmlVal parent_args[2]={vreal(0),vreal(1)};
  (void)gml_builtin_call(&vm,"object_set_parent",parent_args,2);
  GmlVal child_arg=vreal(0);
  GmlVal observed_parent=gml_builtin_call(&vm,"object_get_parent",&child_arg,1);
  if(observed_parent.t!=V_REAL || observed_parent.d!=1 || !gml_object_is(&vm,0,1) ||
     gml_instance_number(&vm,1)!=exact_family){
    fprintf(stderr,"runtime object parent did not update hierarchy queries\n"); return 1;
  }
  parent_args[0]=vreal(1); parent_args[1]=vreal(0);
  (void)gml_builtin_call(&vm,"object_set_parent",parent_args,2);
  if(vm.objects[1].parent==0 || gml_object_is(&vm,1,0)){
    fprintf(stderr,"runtime object parent accepted an inheritance cycle\n"); return 1;
  }
  parent_args[0]=vreal(0); parent_args[1]=vreal(-100);
  (void)gml_builtin_call(&vm,"object_set_parent",parent_args,2);
  if(vm.objects[0].parent!=-1 || gml_instance_number(&vm,1)!=0){
    fprintf(stderr,"runtime object parent detach did not rebuild family counts\n"); return 1;
  }
  GmlInstance *all_first=NULL;
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){ all_first=&vm.inst[i]; break; }
  if(!all_first || all_first==created) return 1;
  *gml_varmap_put(&all_first->vars,"fixture_all_scope")=vreal(11);
  *gml_varmap_put(&created->vars,"fixture_all_scope")=vreal(22);
  implicit_arg=vreal(-2);
  implicit_result=gml_vm_run_code(&vm,implicit_code,created,NULL,&implicit_arg,1);
  if(implicit_result.t!=V_REAL || implicit_result.d!=11){
    fprintf(stderr,"all-scope read did not use the first active instance: %.0f\n",
      implicit_result.t==V_REAL?implicit_result.d:-1.0); return 1;
  }
  implicit_arg=vreal(-3);
  implicit_result=gml_vm_run_code(&vm,implicit_code,created,NULL,&implicit_arg,1);
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){
    GmlVal *all_value=gml_varmap_get(&vm.inst[i].vars,"fixture_all_scope");
    if(!all_value || all_value->t!=V_REAL || all_value->d!=77){
      fprintf(stderr,"all-scope write did not fan out to instance %u\n",vm.inst[i].id); return 1;
    }
  }
  vm.cur_self=created;
  GmlVal change_args[2]={vreal(1),vreal(0)};
  (void)gml_builtin_call(&vm,"action_change_object",change_args,2);
  if(created->obj!=1){ fprintf(stderr,"classic change-object action did not replace the object\n"); return 1; }
  change_args[0]=vreal(0);
  (void)gml_builtin_call(&vm,"action_change_object",change_args,2);
  vm.cur_self=created;
  GmlVal object_args[4]={vreal(1),vreal(32),vreal(0),vreal(1)};
  GmlVal object_hit=gml_builtin_call(&vm,"action_if_object",object_args,4);
  GmlVal exists_arg=vreal((double)created->id);
  GmlVal legacy_exists=gml_builtin_call(&vm,"existe",&exists_arg,1);
  GmlVal previous_room=gml_builtin_call(&vm,"action_if_previous_room",NULL,0);
  if(object_hit.t!=V_REAL || object_hit.d!=0 || legacy_exists.t!=V_REAL || legacy_exists.d!=1 ||
     previous_room.t!=V_REAL || previous_room.d!=0){
    fprintf(stderr,"classic object/existence/previous-room conditions did not match: object=%.0f exists=%.0f previous=%.0f\n",
      object_hit.d,legacy_exists.d,previous_room.d); return 1;
  }
  created->path_index=0;
  GmlVal path_speed=vreal(0.75);
  (void)gml_builtin_call(&vm,"action_path_speed",&path_speed,1);
  (void)gml_builtin_call(&vm,"action_path_end",NULL,0);
  GmlVal timeline_args[4]={vreal(1),vreal(3),vreal(1),vreal(0)};
  (void)gml_builtin_call(&vm,"action_timeline_set",timeline_args,4);
  GmlVal fullscreen=vreal(2);
  (void)gml_builtin_call(&vm,"action_fullscreen",&fullscreen,1);
  if(created->path_index!=-1 || created->path_speed!=0.75 ||
     created->timeline_index!=1 || created->timeline_position!=3 ||
     created->timeline_running!=1 || created->timeline_loop!=0 || vm.window_fullscreen!=1){
    fprintf(stderr,"classic path/timeline/fullscreen actions did not update state\n"); return 1;
  }
  GmlVal gravity_args[2]={vreal(270),vreal(1)};
  (void)gml_builtin_call(&vm,"action_set_gravity",gravity_args,2);
  vm.action_relative=1; gravity_args[0]=vreal(10); gravity_args[1]=vreal(0.5);
  (void)gml_builtin_call(&vm,"action_set_gravity",gravity_args,2);
  vm.action_relative=0;
  if(created->gravity_direction!=280 || created->gravity!=1.5){
    fprintf(stderr,"classic gravity action argument order mismatch: direction=%.0f gravity=%.1f\n",
      created->gravity_direction,created->gravity); return 1;
  }
  created->gravity=0;
  GmlVal potential_args[4]={vreal(22),vreal(34),vreal(2),vreal(0)};
  (void)gml_builtin_call(&vm,"action_potential_step",potential_args,4);
  if(created->x!=14 || created->y!=34){
    fprintf(stderr,"classic potential-step action did not move toward its target: (%.0f,%.0f)\n",
      created->x,created->y); return 1;
  }
  gml_rng_seed(&vm,4);
  GmlVal move_args[2]={vstr("101101101"),vreal(5)};
  (void)gml_builtin_call(&vm,"action_move",move_args,2);
  if(created->direction!=180 || created->hspeed!=-5 || created->vspeed!=0 ||
     vm.rng_classic_state!=1503470698u){
    fprintf(stderr,"classic direction-pad rejection mismatch: direction=%.0f velocity=(%.17g,%.17g) seed=%u\n",
      created->direction,created->hspeed,created->vspeed,vm.rng_classic_state); return 1;
  }
  created->hspeed=3; created->vspeed=4;
  created->speed=5; created->direction=atan2(-4.0,3.0)*180.0/M_PI+360.0;
  vm.action_relative=1;
  GmlVal relative_motion[2]={vreal(0),vreal(2)};
  (void)gml_builtin_call(&vm,"action_set_motion",relative_motion,2);
  vm.action_relative=0;
  if(created->hspeed!=5 || created->vspeed!=4 ||
     fabs(created->speed-sqrt(41.0))>1e-12 ||
     fabs(created->direction-(atan2(-4.0,5.0)*180.0/M_PI+360.0))>1e-12){
    fprintf(stderr,"classic relative motion did not add vector components: direction=%.17g speed=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->speed,created->hspeed,created->vspeed); return 1;
  }
  created->direction=created->speed=created->hspeed=created->vspeed=0;
  vm.cur_self=created;
  GmlVal cardinal_motion[2]={vreal(450),vreal(1)};
  (void)gml_builtin_call(&vm,"motion_set",cardinal_motion,2);
  if(created->direction!=90 || created->hspeed!=0 || created->vspeed!=-1){
    fprintf(stderr,"classic cardinal motion normalization mismatch: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  GmlVal cardinal_target[3]={vreal(created->x),vreal(created->y+10),vreal(1)};
  (void)gml_builtin_call(&vm,"move_towards_point",cardinal_target,3);
  if(created->direction!=270 || created->hspeed!=0 || created->vspeed!=1){
    fprintf(stderr,"classic cardinal target motion mismatch: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  created->direction=created->speed=created->hspeed=created->vspeed=0;
  GmlVal distance_arg=vreal(0);
  GmlVal object_distance=gml_builtin_call(&vm,"distance_to_object",&distance_arg,1);
  distance_arg=vreal((double)created->id);
  GmlVal self_distance=gml_builtin_call(&vm,"distance_to_object",&distance_arg,1);
  vm.cur_self=NULL;
  if(object_distance.t!=V_REAL || object_distance.d!=1000000 ||
     self_distance.t!=V_REAL || self_distance.d!=1000000){
    fprintf(stderr,"distance_to_object missing/self sentinel mismatch: object=%.0f self=%.0f\n",
      object_distance.d,self_distance.d); return 1;
  }
  GmlRender collision_render; GmlSprite collision_sprites[1];
  memset(&collision_render,0,sizeof(collision_render));
  memset(collision_sprites,0,sizeof(collision_sprites));
  collision_render.n_spr=1; collision_render.spr=collision_sprites;
  collision_sprites[0].w=2; collision_sprites[0].h=2;
  collision_sprites[0].ml=collision_sprites[0].mt=0;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  collision_sprites[0].collision_kind=1;
  vm.render=&collision_render;
  created->x=0; created->y=100; created->sprite_index=created->mask_index=0;
  created->image_xscale=created->image_yscale=1;
  GmlInstance *contact=gml_instance_create(&vm,5,100,1); if(!contact)return 1;
  contact->sprite_index=contact->mask_index=0;
  contact->image_xscale=contact->image_yscale=1;
  contact->solid=0;
  GmlInstance *late_order=gml_instance_create(&vm,300,100,0); if(!late_order)return 1;
  collision_sprites[0].w=collision_sprites[0].h=1;
  collision_sprites[0].mr=collision_sprites[0].mb=0;
  created->x=created->y=created->xprevious=created->yprevious=0;
  contact->x=1; contact->y=0; contact->solid=1;
  vm.cur_self=created;
  GmlVal bounce_motion[2]={vreal(0),vreal(1)};
  (void)gml_builtin_call(&vm,"motion_set",bounce_motion,2);
  GmlVal advanced_bounce=vreal(1);
  gml_colgrid_invalidate(&vm);
  (void)gml_builtin_call(&vm,"move_bounce_solid",&advanced_bounce,1);
  if(created->direction!=180 || created->hspeed!=-1 || created->vspeed!=0){
    fprintf(stderr,"classic advanced bounce did not reflect around the sampled free directions: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  contact->x=1; contact->y=1;
  bounce_motion[0]=vreal(315);
  (void)gml_builtin_call(&vm,"motion_set",bounce_motion,2);
  GmlVal basic_bounce=vreal(0);
  gml_colgrid_invalidate(&vm);
  (void)gml_builtin_call(&vm,"move_bounce_solid",&basic_bounce,1);
  if(fabs(created->direction-135)>1e-9 ||
     fabs(created->hspeed+sqrt(0.5))>1e-9 || fabs(created->vspeed+sqrt(0.5))>1e-9){
    fprintf(stderr,"classic basic bounce omitted the diagonal contact: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  collision_sprites[0].w=collision_sprites[0].h=2;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  created->x=0; created->y=100; created->xprevious=0; created->yprevious=100;
  created->direction=created->speed=created->hspeed=created->vspeed=0;
  contact->x=5; contact->y=100; contact->solid=0;
  gml_colgrid_invalidate(&vm);
  vm.cur_self=created;
  GmlVal contact_direction=vreal(0);
  (void)gml_builtin_call(&vm,"move_contact",&contact_direction,1);
  if(created->x!=3){
    fprintf(stderr,"move_contact omitted-distance/any-instance mismatch: x=%.0f\n",created->x); return 1;
  }
  created->x=0;
  GmlVal contact_solid_args[2]={vreal(0),vreal(5)};
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=5){
    fprintf(stderr,"move_contact_solid incorrectly stopped at a non-solid: x=%.0f\n",created->x); return 1;
  }
  created->x=0; contact->solid=1;
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=3){
    fprintf(stderr,"move_contact_solid did not stop before a solid: x=%.0f\n",created->x); return 1;
  }
  created->x=4;
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=4){
    fprintf(stderr,"move_contact_solid moved an initially overlapping instance: x=%.0f\n",created->x); return 1;
  }
  contact->solid=0;
  (void)gml_builtin_call(&vm,"move_contact",&contact_direction,1);
  if(created->x!=4){
    fprintf(stderr,"move_contact moved an initially overlapping instance: x=%.0f\n",created->x); return 1;
  }
  created->x=0;
  GmlVal fractional_contact_args[2]={vreal(0),vreal(4.6)};
  (void)gml_builtin_call(&vm,"move_contact_solid",fractional_contact_args,2);
  if(created->x!=5){
    fprintf(stderr,"move_contact_solid did not round a positive maximum distance: x=%.0f\n",created->x); return 1;
  }
  collision_sprites[0].w=collision_sprites[0].h=1;
  collision_sprites[0].mr=collision_sprites[0].mb=0;
  created->x=0.49; created->y=100; contact->x=0; contact->y=100;
  GmlVal rounded_mask_args[3]={vreal(created->x),vreal(created->y),vreal(1)};
  GmlVal rounded_mask_hit=gml_builtin_call(&vm,"place_meeting",rounded_mask_args,3);
  if(rounded_mask_hit.t!=V_REAL || rounded_mask_hit.d!=1){
    fprintf(stderr,"classic precise mask did not use its rounded instance origin\n"); return 1;
  }
  uint8_t edge_mask=0x80;
  collision_sprites[0].w=2; collision_sprites[0].h=1;
  collision_sprites[0].mr=1; collision_sprites[0].mb=0;
  collision_sprites[0].collision_kind=0;
  collision_sprites[0].mask=&edge_mask;
  collision_sprites[0].mask_rowb=collision_sprites[0].mask_count=1;
  created->x=0.5; contact->x=0;
  rounded_mask_args[0]=vreal(created->x);
  rounded_mask_hit=gml_builtin_call(&vm,"place_meeting",rounded_mask_args,3);
  if(rounded_mask_hit.t!=V_REAL || rounded_mask_hit.d!=1){
    fprintf(stderr,"classic collision geometry did not round a half coordinate to even\n"); return 1;
  }
  collision_sprites[0].mask=NULL;
  collision_sprites[0].mask_rowb=collision_sprites[0].mask_count=0;
  collision_sprites[0].collision_kind=1;
  collision_sprites[0].w=collision_sprites[0].h=2;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  contact->x=5; contact->y=100;
  win.classic_version=0;
  created->x=4; contact->solid=1;
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=3){
    fprintf(stderr,"Studio move_contact_solid did not recover an initial overlap: x=%.0f\n",created->x); return 1;
  }
  created->x=0; contact->solid=0;
  (void)gml_builtin_call(&vm,"move_contact_solid",fractional_contact_args,2);
  if(created->x!=4){
    fprintf(stderr,"Studio move_contact_solid did not retain fractional-distance truncation: x=%.0f\n",created->x); return 1;
  }
  win.classic_version=800;
  contact->solid=1;
  GmlVal potential_settings[4]={vreal(30),vreal(10),vreal(3),vreal(1)};
  (void)gml_builtin_call(&vm,"mp_potential_settings",potential_settings,4);
  created->x=0; created->y=100; created->direction=0; contact->x=5; contact->y=100;
  GmlVal blocked_goal[4]={vreal(5),vreal(100),vreal(10),vreal(0)};
  GmlVal reached=gml_builtin_call(&vm,"mp_potential_step",blocked_goal,4);
  if(reached.t!=V_REAL || reached.d!=0 || created->x!=0 || created->y!=100){
    fprintf(stderr,"potential-step moved into a blocked nearby goal: reached=%.0f pos=(%.1f,%.1f)\n",
      reached.d,created->x,created->y); return 1;
  }
  GmlVal ahead_goal[4]={vreal(20),vreal(100),vreal(2),vreal(0)};
  (void)gml_builtin_call(&vm,"mp_potential_step",ahead_goal,4);
  if(created->x==2 && created->y==100){
    fprintf(stderr,"potential-step ignored its forward obstacle probe\n"); return 1;
  }
  potential_settings[2]=vreal(1);
  (void)gml_builtin_call(&vm,"mp_potential_settings",potential_settings,4);
  created->x=0; created->y=100; created->direction=0;
  (void)gml_builtin_call(&vm,"mp_potential_step",ahead_goal,4);
  if(created->x!=2 || created->y!=100){
    fprintf(stderr,"potential-step settings did not update check distance: (%.1f,%.1f)\n",
      created->x,created->y); return 1;
  }
  contact->x=100; created->x=10; created->y=100; vm.action_relative=1;
  GmlVal relative_goal[4]={vreal(2),vreal(0),vreal(2),vreal(0)};
  reached=gml_builtin_call(&vm,"action_potential_step",relative_goal,4);
  vm.action_relative=0;
  if(reached.t!=V_REAL || reached.d!=1 || created->x!=12 || created->y!=100){
    fprintf(stderr,"relative potential-step target mismatch: reached=%.0f pos=(%.1f,%.1f)\n",
      reached.d,created->x,created->y); return 1;
  }
  *gml_varmap_put(&created->vars,"side")=vreal(180);
  GmlVal local_name=vstr("side");
  vm.cur_self=created;
  GmlVal local_exists=gml_builtin_call(&vm,"variable_local_exists",&local_name,1);
  if(local_exists.t!=V_REAL || local_exists.d!=1){
    fprintf(stderr,"classic instance-local field was reported missing\n"); return 1;
  }
  GmlVal alarm_args[2]={vreal(1.6),vreal(0)};
  (void)gml_builtin_call(&vm,"action_set_alarm",alarm_args,2);
  vm.cur_self=NULL;
  if(created->alarm[0]!=2){
    fprintf(stderr,"classic fractional alarm was not rounded: %.3f\n",created->alarm[0]); return 1;
  }
  vm.cur_self=created;
  GmlVal alarm_set_args[2]={vreal(1),vreal(6.4)};
  (void)gml_builtin_call(&vm,"alarm_set",alarm_set_args,2);
  GmlVal alarm_index=vreal(1);
  GmlVal alarm_value=gml_builtin_call(&vm,"alarm_get",&alarm_index,1);
  vm.cur_self=NULL;
  if(created->alarm[1]!=6 || alarm_value.t!=V_REAL || alarm_value.d!=6){
    fprintf(stderr,"alarm_set/get accessor mismatch: stored=%.3f read=%.3f\n",
      created->alarm[1],alarm_value.t==V_REAL?alarm_value.d:-999.0); return 1;
  }
  GmlVal message_args[4]={vstr("prompt"),vstr(""),vstr("accept"),vstr("cancel")};
  GmlVal message_result=gml_builtin_call(&vm,"show_message_ext",message_args,4);
  if(message_result.t!=V_REAL || message_result.d!=2){
    fprintf(stderr,"non-interactive message button selection mismatch: %.0f\n",message_result.d); return 1;
  }
  GmlVal sleep_args[2]={vreal(1000),vreal(1)};
  (void)gml_builtin_call(&vm,"action_sleep",sleep_args,2);
  (void)gml_builtin_call(&vm,"sleep",sleep_args,1);
  GmlVal another_room_args[2]={vreal(1),vreal(21)};
  (void)gml_builtin_call(&vm,"action_another_room",another_room_args,2);
  GmlVal *transition_kind=gml_varmap_get(&vm.globals,"transition_kind");
  if(vm.pending_room!=1 || !transition_kind || transition_kind->t!=V_REAL ||
     transition_kind->d!=21){
    fprintf(stderr,"classic another-room action mismatch: pending=%d transition=%.0f\n",
      vm.pending_room,transition_kind&&transition_kind->t==V_REAL?transition_kind->d:-1.0);
    return 1;
  }
  vm.pending_room=-1;
  (void)gml_builtin_call(&vm,"action_restart_game",NULL,0);
  if(vm.game_end!=2){ fprintf(stderr,"classic restart action did not request a cold boot\n"); return 1; }
  vm.game_end=0;
  (void)gml_builtin_call(&vm,"action_end_game",NULL,0);
  if(vm.game_end!=1){ fprintf(stderr,"classic end action did not request shutdown\n"); return 1; }
  vm.game_end=0;
  created->alarm[0]=created->alarm[1]=1;
  contact->alarm[0]=contact->alarm[1]=1;
  late_order->alarm[0]=late_order->alarm[1]=1;
  created->gravity=0; created->gravity_direction=270;
  created->hspeed=-1e-16; created->vspeed=0;
  GmlInstance *friction_probe=find_slot(&vm,100000); if(!friction_probe)return 1;
  friction_probe->direction=0; friction_probe->speed=-0.4;
  friction_probe->hspeed=-0.4; friction_probe->vspeed=0; friction_probe->friction=0.2;
  double friction_position=friction_probe->x;
  double position_before_step=created->x;
  fixture_joystick_button3=1;
  gml_vm_step(&vm);
  fixture_joystick_button3=0;
  if(created->x!=position_before_step+5 || created->xprevious!=position_before_step || created->hspeed!=0){
    fprintf(stderr,"previous position/cardinal gravity mismatch: x=%.0f previous=%.0f hspeed=%.17g\n",
      created->x,created->xprevious,created->hspeed); return 1;
  }
  if(fabs(friction_probe->speed+0.2)>1e-9 || fabs(friction_probe->x-(friction_position-0.2))>1e-9){
    fprintf(stderr,"classic negative-speed friction mismatch: speed=%.3f x=%.3f\n",
      friction_probe->speed,friction_probe->x); return 1;
  }
  GmlInstance *same_step_spawn=NULL;
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked &&
      vm.inst[i].obj==1 && vm.inst[i].x>=50 && vm.inst[i].x<70){ same_step_spawn=&vm.inst[i]; break; }
  if(!same_step_spawn || same_step_spawn->x!=60){
    fprintf(stderr,"classic same-step movement/end-step mismatch: x=%.0f\n",
      same_step_spawn?same_step_spawn->x:-1.0); return 1;
  }
  GmlVal *step_order=gml_varmap_get(&vm.globals,"step_order");
  if(!step_order || step_order->t!=V_REAL || step_order->d!=1122){
    fprintf(stderr,"classic object-group Step order mismatch: %.0f\n",
      step_order&&step_order->t==V_REAL?step_order->d:-1.0); return 1;
  }
  GmlVal *persistent_alias_len=gml_varmap_get(&vm.globals,"persistent_alias_len");
  GmlVal *persistent_alias_value=gml_varmap_get(&vm.globals,"persistent_alias_value");
  if(!persistent_alias_len || persistent_alias_len->t!=V_REAL || persistent_alias_len->d!=1 ||
     !persistent_alias_value || persistent_alias_value->t!=V_REAL || persistent_alias_value->d!=7){
    fprintf(stderr,"persistent array/local alias lifetime mismatch: len=%.0f value=%.0f\n",
      persistent_alias_len&&persistent_alias_len->t==V_REAL?persistent_alias_len->d:-1.0,
      persistent_alias_value&&persistent_alias_value->t==V_REAL?persistent_alias_value->d:-1.0);
    return 1;
  }
  GmlVal *alarm_order=gml_varmap_get(&vm.globals,"alarm_order");
  if(!alarm_order || alarm_order->t!=V_REAL || alarm_order->d!=112334){
    fprintf(stderr,"classic alarm/object order mismatch: %.0f\n",
      alarm_order&&alarm_order->t==V_REAL?alarm_order->d:-1.0); return 1;
  }
  GmlVal *key_order=gml_varmap_get(&vm.globals,"key_order");
  if(!key_order || key_order->t!=V_REAL || key_order->d!=112){
    fprintf(stderr,"classic keyboard/object order mismatch: %.0f\n",
      key_order&&key_order->t==V_REAL?key_order->d:-1.0); return 1;
  }
  GmlVal *mouse_order=gml_varmap_get(&vm.globals,"mouse_order");
  if(!mouse_order || mouse_order->t!=V_REAL || mouse_order->d!=112){
    fprintf(stderr,"classic mouse/object order mismatch: %.0f\n",
      mouse_order&&mouse_order->t==V_REAL?mouse_order->d:-1.0); return 1;
  }
  GmlVal *joystick_event_hits=gml_varmap_get(&vm.globals,"joystick_event_hits");
  if(!joystick_event_hits || joystick_event_hits->t!=V_REAL || joystick_event_hits->d!=1){
    fprintf(stderr,"classic joystick button event mismatch: %.0f\n",
      joystick_event_hits&&joystick_event_hits->t==V_REAL?joystick_event_hits->d:-1.0); return 1;
  }
  GmlVal *trigger_hits=gml_varmap_get(&vm.globals,"trigger_hits");
  GmlVal *trigger_order=gml_varmap_get(&vm.globals,"trigger_order");
  if(!trigger_hits || trigger_hits->t!=V_REAL || trigger_hits->d!=3 ||
     !trigger_order || trigger_order->t!=V_REAL || trigger_order->d!=112){
    fprintf(stderr,"classic trigger did not fire\n"); return 1;
  }
  *gml_varmap_put(&vm.globals,"secondary_boundary_order")=vreal(0);
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){
    vm.inst[i].x=400; vm.inst[i].y=100;
    vm.inst[i].hspeed=vm.inst[i].vspeed=vm.inst[i].gravity=0;
    vm.inst[i].sprite_index=vm.inst[i].mask_index=-1;
    vm.inst[i].image_xscale=vm.inst[i].image_yscale=1;
  }
  gml_vm_step(&vm);
  GmlVal *boundary_order=gml_varmap_get(&vm.globals,"boundary_order");
  if(!boundary_order || boundary_order->t!=V_REAL || boundary_order->d!=11223344){
    fprintf(stderr,"classic boundary/object order mismatch: %.0f\n",
      boundary_order&&boundary_order->t==V_REAL?boundary_order->d:-1.0); return 1;
  }
  GmlVal *view_boundary_order=gml_varmap_get(&vm.globals,"secondary_boundary_order");
  if(!view_boundary_order || view_boundary_order->t!=V_REAL || view_boundary_order->d!=11223344){
    fprintf(stderr,"classic secondary-view boundary order mismatch: %.0f\n",
      view_boundary_order&&view_boundary_order->t==V_REAL?view_boundary_order->d:-1.0); return 1;
  }
  created->gravity=created->vspeed=0;
  created->x=200.6; created->y=100.4;
  created->hspeed=created->vspeed=created->gravity=0;
  gml_set_global_arr(&vm,"view_visible",0,0);
  gml_set_global_arr(&vm,"view_visible",1,1);
  gml_set_global_arr(&vm,"view_xview",1,0); gml_set_global_arr(&vm,"view_yview",1,0);
  gml_set_global_arr(&vm,"view_wview",1,100); gml_set_global_arr(&vm,"view_hview",1,100);
  gml_set_global_arr(&vm,"view_hborder",1,10); gml_set_global_arr(&vm,"view_vborder",1,50);
  gml_set_global_arr(&vm,"view_hspeed",1,4); gml_set_global_arr(&vm,"view_vspeed",1,-1);
  gml_set_global_arr(&vm,"view_object",1,(double)created->id);
  gml_vm_step(&vm);
  if(global_array_value(&vm,"view_xview",1)!=4 || global_array_value(&vm,"view_yview",1)!=50){
    fprintf(stderr,"classic secondary-view limited follow mismatch: x=%.0f y=%.0f\n",
      global_array_value(&vm,"view_xview",1),global_array_value(&vm,"view_yview",1)); return 1;
  }
  gml_set_global_arr(&vm,"view_xview",1,0);
  gml_set_global_arr(&vm,"view_hspeed",1,-1);
  gml_vm_step(&vm);
  if(global_array_value(&vm,"view_xview",1)!=121){
    fprintf(stderr,"classic secondary-view snap/round mismatch: x=%.0f\n",
      global_array_value(&vm,"view_xview",1)); return 1;
  }
  {
    GmlVal camera_args[10]={vreal(0),vreal(0),vreal(100),vreal(100),vreal(0),
      vreal(created->id),vreal(4),vreal(-1),vreal(10),vreal(50)};
    GmlVal camera=gml_builtin_call(&vm,"camera_create_view",camera_args,10);
    if(camera.t!=V_REAL || camera.d<0){
      fprintf(stderr,"studio camera follow fixture allocation failed\n"); return 1;
    }
    int camera_id=(int)camera.d;
    gml_vm_step(&vm);
    if(fabs(global_array_value(&vm,"__gml_camera_x",camera_id)-4)>1e-9 ||
       fabs(global_array_value(&vm,"__gml_camera_y",camera_id)-50.4)>1e-9){
      fprintf(stderr,"studio camera limited follow mismatch: x=%.3f y=%.3f\n",
        global_array_value(&vm,"__gml_camera_x",camera_id),
        global_array_value(&vm,"__gml_camera_y",camera_id)); return 1;
    }
    GmlVal speed_args[3]={camera,vreal(-1),vreal(-1)};
    (void)gml_builtin_call(&vm,"camera_set_view_speed",speed_args,3);
    gml_vm_step(&vm);
    if(fabs(global_array_value(&vm,"__gml_camera_x",camera_id)-130.6)>1e-9 ||
       fabs(global_array_value(&vm,"__gml_camera_y",camera_id)-50.4)>1e-9){
      fprintf(stderr,"studio camera snap/fraction mismatch: x=%.3f y=%.3f\n",
        global_array_value(&vm,"__gml_camera_x",camera_id),
        global_array_value(&vm,"__gml_camera_y",camera_id)); return 1;
    }
    (void)gml_builtin_call(&vm,"camera_destroy",&camera,1);
  }
  gml_set_global_arr(&vm,"background_x",0,123);
  gml_set_global_arr(&vm,"view_xview",0,77);
  *gml_varmap_put(&vm.globals,"room_speed")=vreal(55);
  gml_tile_layer_shift(&vm,300,8,9);
  uint32_t id=created->id; *gml_varmap_put(&created->vars,"value")=vreal(42);
  gml_room_enter(&vm,1);
  room_view_enabled=gml_varmap_get(&vm.globals,"view_enabled");
  if(!room_view_enabled || room_view_enabled->t!=V_REAL || room_view_enabled->d!=0){
    fprintf(stderr,"room flags did not disable the legacy view system: %.0f\n",
      room_view_enabled&&room_view_enabled->t==V_REAL?room_view_enabled->d:-1.0);
    return 1;
  }
  GmlInstance *slot=find_slot(&vm,id);
  if(!slot||!slot->room_dormant||slot->active){ fprintf(stderr,"room was not stored\n"); return 1; }
  size_t size=gml_vm_state_size(&vm),written=0,used=0; void *state=malloc(size);
  if(!state||!gml_vm_state_save(&vm,state,size,&written)||written!=size)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  if(!slot||!slot->active||slot->room_dormant)return 1;
  *gml_varmap_put(&slot->vars,"value")=vreal(99);
  if(!gml_vm_state_load(&vm,state,written,&used)||used!=written)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  GmlVal *value=slot?gml_varmap_get(&slot->vars,"value"):NULL;
  GmlVal *room_speed=gml_varmap_get(&vm.globals,"room_speed");
  int ok=slot&&slot->active&&!slot->room_dormant&&value&&value->t==V_REAL&&value->d==42 &&
    global_array_value(&vm,"background_x",0)==123 && global_array_value(&vm,"view_xview",0)==77 &&
    room_speed&&room_speed->t==V_REAL&&room_speed->d==55 && vm.n_tile_mut==1 &&
    vm.tile_mut[0].depth==300&&vm.tile_mut[0].dx==8&&vm.tile_mut[0].dy==9 &&
    vm.potential_max_rotation==30 && vm.potential_rotate_step==10 &&
    vm.potential_check_distance==1 && vm.potential_rotate_on_spot==1;
  if(!ok) fprintf(stderr,
    "persistent room state did not roundtrip: slot=%d value=%.0f bg=%.0f view=%.0f speed=%.0f tiles=%d depth=%d shift=(%.0f,%.0f)\n",
    slot&&slot->active&&!slot->room_dormant,value&&value->t==V_REAL?value->d:-1,
    global_array_value(&vm,"background_x",0),global_array_value(&vm,"view_xview",0),
    room_speed&&room_speed->t==V_REAL?room_speed->d:-1,vm.n_tile_mut,
    vm.n_tile_mut?vm.tile_mut[0].depth:-1,vm.n_tile_mut?vm.tile_mut[0].dx:0,vm.n_tile_mut?vm.tile_mut[0].dy:0);
  collision_sprites[0].n_frames=2;
  slot->sprite_index=slot->mask_index=0;
  slot->image_index=0;
  slot->image_speed=0.5;
  uint32_t animation_id=slot->id;
  gml_vm_step(&vm);
  slot=find_slot(&vm,animation_id);
  if(!slot){ fprintf(stderr,"classic animation fixture instance disappeared\n"); return 1; }
  if(slot->image_index!=0){
    fprintf(stderr,"classic animation advanced before the draw phase: index=%.2f\n",slot->image_index); return 1;
  }
  gml_vm_post_draw(&vm);
  if(slot->image_index!=0.5){
    fprintf(stderr,"classic animation did not advance after the draw phase: index=%.2f\n",slot->image_index); return 1;
  }
  slot->sprite_index=slot->mask_index=-1;
  slot->image_index=0;
  slot->image_speed=0.5;
  gml_vm_post_draw(&vm);
  if(slot->image_index!=0.5){
    fprintf(stderr,"classic sprite-less drawing frame did not advance: index=%.2f\n",slot->image_index); return 1;
  }
  win.classic_version=0;
  win.bytecode=15;
  GmlInstance *studio_contact=gml_instance_create(&vm,40,40,1); if(!studio_contact)return 1;
  uint32_t studio_contact_id=studio_contact->id;
  GmlInstance *studio_actor=gml_instance_create(&vm,40,40,2); if(!studio_actor)return 1;
  uint32_t studio_actor_id=studio_actor->id;
  studio_contact=find_slot(&vm,studio_contact_id); studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_contact || !studio_actor)return 1;
  studio_contact->sprite_index=studio_contact->mask_index=0;
  studio_actor->sprite_index=studio_actor->mask_index=0;
  studio_contact->image_xscale=studio_contact->image_yscale=1;
  studio_actor->image_xscale=studio_actor->image_yscale=1;
  studio_contact->solid=1;
  studio_contact->speed=studio_contact->hspeed=studio_contact->vspeed=0;
  studio_actor->speed=1; studio_actor->direction=270;
  studio_actor->hspeed=0; studio_actor->vspeed=-1;
  studio_contact->xprevious=studio_contact->x; studio_contact->yprevious=studio_contact->y;
  studio_actor->y=41; studio_actor->xprevious=studio_actor->x; studio_actor->yprevious=40;
  *gml_varmap_put(&vm.globals,"studio_solid_hits")=vreal(0);
  gml_colgrid_invalidate(&vm);
  gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  GmlVal *studio_solid_hits=gml_varmap_get(&vm.globals,"studio_solid_hits");
  if(!studio_actor || studio_actor->y!=39 || !studio_solid_hits ||
     studio_solid_hits->t!=V_REAL || studio_solid_hits->d!=1){
    fprintf(stderr,"Studio 1.x solid collision current-position mismatch: y=%.0f hits=%.0f\n",
      studio_actor?studio_actor->y:-1.0,
      studio_solid_hits&&studio_solid_hits->t==V_REAL?studio_solid_hits->d:-1.0); return 1;
  }
  studio_contact->marked=1; studio_actor->marked=1;

  /* GMS2 changed solid dispatch to expose pre-movement coordinates to the event. Keep the
   * two families explicit so fixing one cannot silently alter the other. */
  win.bytecode=17;
  studio_contact=gml_instance_create(&vm,80,80,1); if(!studio_contact)return 1;
  studio_contact_id=studio_contact->id;
  studio_actor=gml_instance_create(&vm,80,80,2); if(!studio_actor)return 1;
  studio_actor_id=studio_actor->id;
  studio_contact=find_slot(&vm,studio_contact_id); studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_contact || !studio_actor)return 1;
  studio_contact->sprite_index=studio_contact->mask_index=0;
  studio_actor->sprite_index=studio_actor->mask_index=0;
  studio_contact->image_xscale=studio_contact->image_yscale=1;
  studio_actor->image_xscale=studio_actor->image_yscale=1;
  studio_contact->solid=1;
  studio_contact->speed=studio_contact->hspeed=studio_contact->vspeed=0;
  studio_actor->speed=1; studio_actor->direction=270;
  studio_actor->hspeed=0; studio_actor->vspeed=-1;
  studio_contact->xprevious=studio_contact->x; studio_contact->yprevious=studio_contact->y;
  studio_actor->y=81; studio_actor->xprevious=studio_actor->x; studio_actor->yprevious=80;
  /* A Studio alarm without a corresponding event is a normal user-visible counter.  It must not
   * be consumed by the automatic alarm phase. */
  studio_actor->alarm[5]=1;
  *gml_varmap_put(&vm.globals,"studio_solid_hits")=vreal(0);
  gml_colgrid_invalidate(&vm);
  gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  studio_solid_hits=gml_varmap_get(&vm.globals,"studio_solid_hits");
  if(!studio_actor || studio_actor->y!=80 || studio_actor->alarm[5]!=1 || !studio_solid_hits ||
     studio_solid_hits->t!=V_REAL || studio_solid_hits->d!=1){
    fprintf(stderr,"GMS2 solid/alarm semantics mismatch: y=%.0f alarm=%.0f hits=%.0f\n",
      studio_actor?studio_actor->y:-1.0,
      studio_actor?studio_actor->alarm[5]:-1.0,
      studio_solid_hits&&studio_solid_hits->t==V_REAL?studio_solid_hits->d:-1.0); return 1;
  }

  /* Studio preserves an empty native Alarm declaration even though it has no CODE entry.  Its
   * counter must run to completion; this differs from the truly undeclared slot checked above. */
  GmlObject *studio_actor_object=&vm.objects[studio_actor->obj];
  int empty_event_index=studio_actor_object->n_events;
  void *grown_events=realloc(studio_actor_object->events,
    (size_t)(empty_event_index+1)*sizeof(*studio_actor_object->events));
  if(!grown_events) return 1;
  studio_actor_object->events=grown_events;
  studio_actor_object->events[empty_event_index].evtype=2;
  studio_actor_object->events[empty_event_index].subtype=5;
  studio_actor_object->events[empty_event_index].code=-1;
  studio_actor_object->n_events++;
  studio_actor->alarm[5]=1;
  gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_actor || studio_actor->alarm[5]!=-1){
    fprintf(stderr,"empty native alarm declaration did not count down: %.0f\n",
      studio_actor?studio_actor->alarm[5]:-999.0); return 1;
  }

  /* Studio takes a fixed event snapshot at frame start, but dead slots from older frames are
   * safe to recycle.  Exercise both halves together: a newly created instance placed in an old
   * low slot must NOT receive Step in its birth frame, and create/destroy churn must stay bounded
   * without moving any live instance pointer. */
  int changed_before=gml_instance_number(&vm,1);
  *gml_varmap_put(&vm.globals,"spawned_once")=vreal(0);
  *gml_varmap_put(&vm.globals,"churn_pool")=vreal(0);
  *gml_varmap_put(&vm.globals,"changed_step_hits")=vreal(0);
  GmlInstance *stable=find_slot(&vm,id);
  uint32_t stable_id=stable?stable->id:0;
  gml_vm_step(&vm);
  GmlVal *changed_hits=gml_varmap_get(&vm.globals,"changed_step_hits");
  if(!stable || find_slot(&vm,stable_id)!=stable || !changed_hits ||
     changed_hits->t!=V_REAL || changed_hits->d!=changed_before){
    fprintf(stderr,"Studio recycled-slot snapshot mismatch: before=%d hits=%.0f stable=%d\n",
      changed_before,changed_hits&&changed_hits->t==V_REAL?changed_hits->d:-1.0,
      stable&&find_slot(&vm,stable_id)==stable); return 1;
  }
  *gml_varmap_put(&vm.globals,"spawned_once")=vreal(1);
  *gml_varmap_put(&vm.globals,"churn_pool")=vreal(1);
  int churn_base=vm.inst_count;
  int churn_producers=gml_instance_number(&vm,0);
  for(int frame=0;frame<96;frame++) gml_vm_step(&vm);
  *gml_varmap_put(&vm.globals,"churn_pool")=vreal(0);
  if(vm.inst_count>churn_base+churn_producers+2 || find_slot(&vm,stable_id)!=stable){
    fprintf(stderr,"instance pool churn was unbounded/moved a live slot: base=%d final=%d producers=%d stable=%d\n",
      churn_base,vm.inst_count,churn_producers,find_slot(&vm,stable_id)==stable); return 1;
  }
  free(state); gml_vm_free(&vm); gml_win_free(&win); unlink(path); unlink(startup); unlink(implicit_script); unlink(shadowed_alias_script); unlink(condition); unlink(event); unlink(changed_trigger); unlink(create_order); unlink(joystick_event); unlink(solid_collision); unlink(destroy_reentry); unlink(instance_order); unlink(step); unlink(end_step); unlink(changed_step); unlink(included_path);
  for(int i=0;i<4;i++) unlink(alarm_files[i]);
  for(int i=0;i<2;i++) unlink(key_files[i]);
  for(int i=0;i<2;i++) unlink(mouse_files[i]);
  for(int i=0;i<4;i++) unlink(boundary_files[i]);
  for(int i=0;i<4;i++) unlink(view_boundary_files[i]);
  for(int i=0;i<3;i++) unlink(timeline_files[i]);
  rmdir(save_root);
  if(ok) puts("persistent room fixtures: ok");
  return ok?0:1;
}
