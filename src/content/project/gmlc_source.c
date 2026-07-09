/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_source.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_text(const char *path){
  FILE *f=fopen(path,"rb");
  if(!f) return NULL;
  fseek(f,0,SEEK_END);
  long sz=ftell(f);
  rewind(f);
  if(sz<0){ fclose(f); return NULL; }
  char *buf=(char*)malloc((size_t)sz+1);
  if(!buf){ fclose(f); return NULL; }
  if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); return NULL; }
  fclose(f);
  buf[sz]=0;
  return buf;
}

static int word_at(const char *base, const char *p, const char *w){
  size_t n=strlen(w);
  if(strncmp(p,w,n)) return 0;
  if((p>base && (isalnum((unsigned char)p[-1]) || p[-1]=='_')) || (isalnum((unsigned char)p[n]) || p[n]=='_')) return 0;
  return 1;
}

static void scan_text(const char *s, GmlcSourceReport *r){
  int any=0;
  for(const char *p=s; *p; p++){
    if(!isspace((unsigned char)*p)) any=1;
    if(*p=='/' && p[1]=='/'){ while(*p && *p!='\n') p++; if(!*p) break; }
    if(*p=='/' && p[1]=='*'){ p+=2; while(*p && !(p[0]=='*'&&p[1]=='/')) p++; if(*p) p++; continue; }
    if(*p=='#' && !strncmp(p,"#macro",6)) r->macros++;
    if(word_at(s,p,"if")) r->ifs++;
    else if(word_at(s,p,"while") || word_at(s,p,"for") || word_at(s,p,"repeat")) r->loops++;
    else if(word_at(s,p,"switch")) r->switches++;
    else if(word_at(s,p,"with")) r->withs++;
    else if(word_at(s,p,"return") || word_at(s,p,"exit")) r->returns++;
    if(*p=='(') r->calls++;
    if(*p=='[') r->arrays++;
  }
  if(any) r->nonempty_files++;
}

int gmlc_source_scan_project(const GmlcProject *p, GmlcSourceReport *out, char *err, size_t errcap){
  memset(out,0,sizeof(*out));
  for(int i=0;i<p->n_scripts;i++){
    char *txt=read_text(p->scripts[i].source_path);
    if(!txt){
      snprintf(err,errcap,"%s: read failed",p->scripts[i].source_path);
      return 0;
    }
    out->files++;
    scan_text(txt,out);
    free(txt);
  }
  for(int i=0;i<p->n_resources;i++){
    if(p->resources[i].kind!=GMLC_RES_OBJECT) continue;
    char *dir=gmlc_path_dirname(p->resources[i].abs_path);
    if(!dir) continue;
    static const char *names[]={"Create_0.gml","Step_0.gml","Step_1.gml","Step_2.gml","Draw_0.gml","Draw_64.gml","Draw_73.gml","Destroy_0.gml","CleanUp_0.gml","Other_0.gml","Other_2.gml","Other_3.gml","Other_4.gml","Other_5.gml","Other_7.gml","Alarm_0.gml","Alarm_1.gml","Alarm_2.gml","Alarm_3.gml","Alarm_4.gml","Alarm_5.gml","Alarm_6.gml","Alarm_7.gml","Alarm_8.gml","Alarm_9.gml","Alarm_10.gml","Alarm_11.gml"};
    for(size_t k=0;k<sizeof(names)/sizeof(names[0]);k++){
      char *path=gmlc_path_join(dir,names[k]);
      FILE *f=path?fopen(path,"rb"):NULL;
      if(f){ fclose(f); char *txt=read_text(path); if(txt){ out->files++; scan_text(txt,out); free(txt); } }
      free(path);
    }
    free(dir);
  }
  return 1;
}

void gmlc_source_print_report(const GmlcSourceReport *r){
  printf("source files=%d nonempty=%d macros=%d if=%d loops=%d switch=%d with=%d return/exit=%d call-sites-ish=%d arrays=%d\n",
    r->files,r->nonempty_files,r->macros,r->ifs,r->loops,r->switches,r->withs,r->returns,r->calls,r->arrays);
}
