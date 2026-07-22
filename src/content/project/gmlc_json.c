/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_json.h"
#include "anygm_vfs.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *base;
  const char *p;
  const char *end;
  const char *label;
  char *err;
  size_t errcap;
} JsonParser;

static void json_err(JsonParser *p, const char *msg){
  if(!p->err || !p->errcap || p->err[0]) return;
  snprintf(p->err,p->errcap,"%s: %s near byte %ld",p->label?p->label:"json",msg,(long)(p->p - p->base));
}

static void json_err_at(JsonParser *p, const char *msg, const char *at){
  if(!p->err || !p->errcap || p->err[0]) return;
  snprintf(p->err,p->errcap,"%s: %s near byte %ld",p->label?p->label:"json",msg,(long)(at - p->base));
}

static void skip_ws(JsonParser *p){
  while(p->p<p->end && isspace((unsigned char)*p->p)) p->p++;
}

static GmlcJson *node_new(GmlcJsonType t){
  GmlcJson *v=(GmlcJson*)calloc(1,sizeof(*v));
  if(v) v->type=t;
  return v;
}

static int hexv(int c){
  if(c>='0'&&c<='9') return c-'0';
  if(c>='a'&&c<='f') return c-'a'+10;
  if(c>='A'&&c<='F') return c-'A'+10;
  return -1;
}

static int append_utf8(char **buf, size_t *len, size_t *cap, unsigned cp){
  unsigned char tmp[4]; int n=0;
  if(cp<0x80){ tmp[n++]=(unsigned char)cp; }
  else if(cp<0x800){ tmp[n++]=(unsigned char)(0xC0|(cp>>6)); tmp[n++]=(unsigned char)(0x80|(cp&0x3F)); }
  else { tmp[n++]=(unsigned char)(0xE0|(cp>>12)); tmp[n++]=(unsigned char)(0x80|((cp>>6)&0x3F)); tmp[n++]=(unsigned char)(0x80|(cp&0x3F)); }
  if(*len+(size_t)n+1>*cap){
    size_t nc=*cap?*cap*2:32;
    while(*len+(size_t)n+1>nc) nc*=2;
    char *nb=(char*)realloc(*buf,nc);
    if(!nb) return 0;
    *buf=nb; *cap=nc;
  }
  memcpy(*buf+*len,tmp,(size_t)n); *len+=(size_t)n; (*buf)[*len]=0;
  return 1;
}

static char *parse_string_raw(JsonParser *p){
  if(p->p>=p->end || *p->p!='"'){ json_err(p,"expected string"); return NULL; }
  p->p++;
  char *out=NULL; size_t len=0,cap=0;
  while(p->p<p->end){
    unsigned char c=(unsigned char)*p->p++;
    if(c=='"'){
      if(!out){ out=(char*)calloc(1,1); }
      return out;
    }
    if(c=='\\'){
      if(p->p>=p->end){ json_err(p,"unterminated escape"); break; }
      c=(unsigned char)*p->p++;
      switch(c){
        case '"': case '\\': case '/': if(!append_utf8(&out,&len,&cap,c)) return NULL; break;
        case 'b': if(!append_utf8(&out,&len,&cap,'\b')) return NULL; break;
        case 'f': if(!append_utf8(&out,&len,&cap,'\f')) return NULL; break;
        case 'n': if(!append_utf8(&out,&len,&cap,'\n')) return NULL; break;
        case 'r': if(!append_utf8(&out,&len,&cap,'\r')) return NULL; break;
        case 't': if(!append_utf8(&out,&len,&cap,'\t')) return NULL; break;
        case 'u': {
          if(p->end-p->p<4){ json_err(p,"short unicode escape"); free(out); return NULL; }
          unsigned cp=0;
          for(int i=0;i<4;i++){ int h=hexv((unsigned char)p->p[i]); if(h<0){ json_err(p,"bad unicode escape"); free(out); return NULL; } cp=(cp<<4)|(unsigned)h; }
          p->p+=4;
          if(!append_utf8(&out,&len,&cap,cp)) return NULL;
          break;
        }
        default: json_err(p,"bad escape"); free(out); return NULL;
      }
    } else {
      if(!append_utf8(&out,&len,&cap,c)) return NULL;
    }
  }
  json_err(p,"unterminated string");
  free(out);
  return NULL;
}

static GmlcJson *parse_value(JsonParser *p);

static void append_child(GmlcJson *parent, GmlcJson *child){
  if(!parent->child){ parent->child=child; return; }
  GmlcJson *tail=parent->child;
  while(tail->next) tail=tail->next;
  tail->next=child;
}

static GmlcJson *parse_array(JsonParser *p){
  p->p++;
  GmlcJson *a=node_new(GMLC_JSON_ARRAY);
  if(!a) return NULL;
  skip_ws(p);
  if(p->p<p->end && *p->p==']'){ p->p++; return a; }
  for(;;){
    skip_ws(p);
    GmlcJson *v=parse_value(p);
    if(!v){ gmlc_json_free(a); return NULL; }
    append_child(a,v);
    skip_ws(p);
    if(p->p>=p->end){ json_err(p,"unterminated array"); gmlc_json_free(a); return NULL; }
    if(*p->p==']'){ p->p++; return a; }
    if(*p->p!=','){ json_err(p,"expected comma"); gmlc_json_free(a); return NULL; }
    p->p++;
    skip_ws(p);
    if(p->p<p->end && *p->p==']'){ p->p++; return a; }
  }
}

static GmlcJson *parse_object(JsonParser *p){
  p->p++;
  GmlcJson *o=node_new(GMLC_JSON_OBJECT);
  if(!o) return NULL;
  skip_ws(p);
  if(p->p<p->end && *p->p=='}'){ p->p++; return o; }
  for(;;){
    skip_ws(p);
    char *name=parse_string_raw(p);
    if(!name){ gmlc_json_free(o); return NULL; }
    skip_ws(p);
    if(p->p>=p->end || *p->p!=':'){ free(name); json_err(p,"expected colon"); gmlc_json_free(o); return NULL; }
    p->p++;
    skip_ws(p);
    GmlcJson *v=parse_value(p);
    if(!v){ free(name); gmlc_json_free(o); return NULL; }
    v->name=name;
    append_child(o,v);
    skip_ws(p);
    if(p->p>=p->end){ json_err(p,"unterminated object"); gmlc_json_free(o); return NULL; }
    if(*p->p=='}'){ p->p++; return o; }
    if(*p->p!=','){ json_err(p,"expected comma"); gmlc_json_free(o); return NULL; }
    p->p++;
    skip_ws(p);
    if(p->p<p->end && *p->p=='}'){ p->p++; return o; }
  }
}

static GmlcJson *parse_number(JsonParser *p){
  const char *start=p->p;
  if(p->p<p->end && *p->p=='-') p->p++;
  while(p->p<p->end && isdigit((unsigned char)*p->p)) p->p++;
  if(p->p<p->end && *p->p=='.'){ p->p++; while(p->p<p->end && isdigit((unsigned char)*p->p)) p->p++; }
  if(p->p<p->end && (*p->p=='e'||*p->p=='E')){
    p->p++;
    if(p->p<p->end && (*p->p=='+'||*p->p=='-')) p->p++;
    while(p->p<p->end && isdigit((unsigned char)*p->p)) p->p++;
  }
  char tmp[128];
  size_t n=(size_t)(p->p-start);
  if(n>=sizeof(tmp)){ json_err_at(p,"number too long",start); return NULL; }
  memcpy(tmp,start,n); tmp[n]=0;
  char *ep=NULL;
  errno=0;
  double d=strtod(tmp,&ep);
  if(errno || ep==tmp){ json_err_at(p,"bad number",start); return NULL; }
  GmlcJson *v=node_new(GMLC_JSON_NUMBER);
  if(v) v->n=d;
  return v;
}

static int match_word(JsonParser *p, const char *word){
  size_t n=strlen(word);
  if((size_t)(p->end-p->p)<n || memcmp(p->p,word,n)) return 0;
  p->p+=n;
  return 1;
}

static GmlcJson *parse_value(JsonParser *p){
  skip_ws(p);
  if(p->p>=p->end){ json_err(p,"expected value"); return NULL; }
  if(*p->p=='"'){
    GmlcJson *v=node_new(GMLC_JSON_STRING);
    if(!v) return NULL;
    v->s=parse_string_raw(p);
    if(!v->s){ free(v); return NULL; }
    return v;
  }
  if(*p->p=='{') return parse_object(p);
  if(*p->p=='[') return parse_array(p);
  if(*p->p=='-' || isdigit((unsigned char)*p->p)) return parse_number(p);
  if(match_word(p,"true")){ GmlcJson *v=node_new(GMLC_JSON_BOOL); if(v) v->b=1; return v; }
  if(match_word(p,"false")){ GmlcJson *v=node_new(GMLC_JSON_BOOL); if(v) v->b=0; return v; }
  if(match_word(p,"null")) return node_new(GMLC_JSON_NULL);
  json_err(p,"unexpected token");
  return NULL;
}

static char *read_file(const AnygmHostServices *host,const char *path,size_t *out_len,
                       char *err,size_t errcap){
  uint8_t *bytes=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(host,path,&bytes,&size,128u*1024u*1024u)){
    snprintf(err,errcap,"%s: read failed",path); return NULL;
  }
  char *text=realloc(bytes,size+1);
  if(!text){ free(bytes); snprintf(err,errcap,"%s: out of memory",path); return NULL; }
  text[size]=0;
  if(out_len) *out_len=size;
  return text;
}

GmlcJson *gmlc_json_parse_text(const char *text, const char *label, char *err, size_t errcap){
  if(err && errcap) err[0]=0;
  JsonParser p;
  memset(&p,0,sizeof(p));
  p.base=text; p.p=text; p.end=text+strlen(text); p.label=label; p.err=err; p.errcap=errcap;
  GmlcJson *v=parse_value(&p);
  if(!v) return NULL;
  skip_ws(&p);
  if(p.p!=p.end){
    json_err(&p,"trailing input");
    gmlc_json_free(v);
    return NULL;
  }
  return v;
}

GmlcJson *gmlc_json_parse_file(const AnygmHostServices *host,const char *path,
                               char *err,size_t errcap){
  size_t len=0;
  if(err && errcap) err[0]=0;
  char *txt=read_file(host,path,&len,err,errcap);
  (void)len;
  if(!txt) return NULL;
  GmlcJson *v=gmlc_json_parse_text(txt,path,err,errcap);
  free(txt);
  return v;
}

void gmlc_json_free(GmlcJson *v){
  while(v){
    GmlcJson *next=v->next;
    gmlc_json_free(v->child);
    free(v->name);
    free(v->s);
    free(v);
    v=next;
  }
}

const GmlcJson *gmlc_json_obj(const GmlcJson *v, const char *key){
  if(!v || v->type!=GMLC_JSON_OBJECT || !key) return NULL;
  for(const GmlcJson *c=v->child;c;c=c->next) if(c->name && !strcmp(c->name,key)) return c;
  return NULL;
}

const GmlcJson *gmlc_json_index(const GmlcJson *v, int index){
  if(!v || v->type!=GMLC_JSON_ARRAY || index<0) return NULL;
  int i=0;
  for(const GmlcJson *c=v->child;c;c=c->next,i++) if(i==index) return c;
  return NULL;
}

int gmlc_json_len(const GmlcJson *v){
  if(!v || (v->type!=GMLC_JSON_ARRAY && v->type!=GMLC_JSON_OBJECT)) return 0;
  int n=0;
  for(const GmlcJson *c=v->child;c;c=c->next) n++;
  return n;
}

const char *gmlc_json_str(const GmlcJson *v, const char *fallback){
  if(v && v->type==GMLC_JSON_STRING && v->s) return v->s;
  if(v && v->type==GMLC_JSON_OBJECT){
    const GmlcJson *name=gmlc_json_obj(v,"name");
    if(name && name->type==GMLC_JSON_STRING && name->s) return name->s;
    const GmlcJson *path=gmlc_json_obj(v,"path");
    if(path && path->type==GMLC_JSON_STRING && path->s) return path->s;
  }
  return fallback;
}

double gmlc_json_num(const GmlcJson *v, double fallback){
  if(v && v->type==GMLC_JSON_NUMBER) return v->n;
  if(v && v->type==GMLC_JSON_BOOL) return v->b?1.0:0.0;
  return fallback;
}

int gmlc_json_int(const GmlcJson *v, int fallback){
  return (int)gmlc_json_num(v,(double)fallback);
}

int gmlc_json_bool(const GmlcJson *v, int fallback){
  if(v && v->type==GMLC_JSON_BOOL) return v->b;
  if(v && v->type==GMLC_JSON_NUMBER) return v->n!=0.0;
  return fallback;
}
