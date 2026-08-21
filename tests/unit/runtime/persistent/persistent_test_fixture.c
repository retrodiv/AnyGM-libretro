/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include <stdio.h>
#include <string.h>

int fixture_joystick_button3;

const char *fixture_development_setting(void *userdata,const char *name){
  (void)userdata;
  return name && !strcmp(name,"GML_ENVIRONMENT_FIXTURE")?"visible":NULL;
}

static int fixture_input_key(void *userdata,int key,int edge){
  (void)userdata;
  return key==65 && edge==0;
}

static int fixture_input_gamepad(void *userdata,int device,int button,int edge){
  (void)userdata;
  return fixture_joystick_button3 && device==0 && button==32771 && edge==0;
}

static void fixture_input_mouse(void *userdata,double *rx,double *ry,double *gx,double *gy,
                                double *wx,double *wy,int *held,int *pressed,
                                int *released,int *wheel){
  (void)userdata;
  if(rx) *rx=0;
  if(ry) *ry=0;
  if(gx) *gx=0;
  if(gy) *gy=0;
  if(wx) *wx=0;
  if(wy) *wy=0;
  if(held) *held=1;
  if(pressed) *pressed=0;
  if(released) *released=0;
  if(wheel) *wheel=0;
}

void fixture_attach_input(GmlVM *vm){
  vm->input.key=fixture_input_key;
  vm->input.gamepad=fixture_input_gamepad;
  vm->input.mouse=fixture_input_mouse;
}


int fixture_write_text(const char *path,const char *text){
  FILE *f=fopen(path,"wb");
  int ok=f && fwrite(text,1,strlen(text),f)==strlen(text);
  if(f && fclose(f)) ok=0;
  return ok;
}


int fixture_write_large_ini(const char *path){
  FILE *f=fopen(path,"wb");
  if(!f) return 0;
  int ok=fprintf(f,"[localization]\n")>0;
  for(int i=0;ok && i<600;i++)
    ok=fprintf(f,i==599?"key_%03d=\"value_%03d\"\n":"key_%03d=value_%03d\n",i,i)>0;
  if(fclose(f)) ok=0;
  return ok;
}


int fixture_read_text(const char *path,char *out,size_t cap){
  FILE *f=fopen(path,"rb");
  if(!f || !cap){ if(f) fclose(f); return 0; }
  size_t n=fread(out,1,cap-1,f); out[n]=0;
  int ok=!ferror(f);
  fclose(f);
  return ok;
}


GmlInstance *find_slot(GmlVM *vm,uint32_t id){
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].id==id) return &vm->inst[i];
  return NULL;
}


uint32_t fixture_u32(const uint8_t *data,size_t offset){
  return (uint32_t)data[offset] | (uint32_t)data[offset+1]<<8 |
    (uint32_t)data[offset+2]<<16 | (uint32_t)data[offset+3]<<24;
}


void fixture_w32(uint8_t *data,size_t offset,uint32_t value){
  data[offset]=(uint8_t)value; data[offset+1]=(uint8_t)(value>>8);
  data[offset+2]=(uint8_t)(value>>16); data[offset+3]=(uint8_t)(value>>24);
}



double global_array_value(GmlVM *vm,const char *name,int index){
  GmlVal *value=gml_varmap_get(&vm->globals,name);
  GmlArr *array=value&&value->t==V_ARR?(GmlArr*)value->arr:NULL;
  return array&&index>=0&&index<array->len&&array->data[index].t==V_REAL?array->data[index].d:0;
}


void fixture_word(unsigned char *data,int index,uint32_t word){
  data[index*4+0]=(unsigned char)word;
  data[index*4+1]=(unsigned char)(word>>8);
  data[index*4+2]=(unsigned char)(word>>16);
  data[index*4+3]=(unsigned char)(word>>24);
}
