/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* JSON encoding, decoding, and ordered JSON builtin adaptation. */
#include "gml_builtin_internal.h"
#include "anygm_host.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int json_writer_reserve(GmlBuiltinJsonWriter *b, size_t add){
  if(b->length+add+1<=b->capacity) return 1;
  size_t nc=b->capacity?b->capacity*2:128;
  while(nc<b->length+add+1) nc*=2;
  char *ns=realloc(b->text,nc);
  if(!ns) return 0;
  b->text=ns; b->capacity=nc; return 1;
}
int gml_builtin_json_writer_putc(GmlBuiltinJsonWriter *b, char c){
  if(!json_writer_reserve(b,1)) return 0;
  b->text[b->length++]=c;
  b->text[b->length]=0;
  return 1;
}
int gml_builtin_json_writer_puts(GmlBuiltinJsonWriter *b, const char *s){
  size_t n=s?strlen(s):0;
  if(!json_writer_reserve(b,n)) return 0;
  if(n) memcpy(b->text+b->length,s,n);
  b->length+=n;
  b->text[b->length]=0;
  return 1;
}
void gml_builtin_json_writer_discard(GmlBuiltinJsonWriter *b){
  if(!b) return;
  free(b->text);
  memset(b,0,sizeof(*b));
}
char *gml_builtin_json_writer_take(GmlBuiltinJsonWriter *b,
                                   const char *fallback){
  if(!b) return strdup(fallback?fallback:"");
  char *text=b->text;
  b->text=NULL;
  b->length=0;
  b->capacity=0;
  return text?text:strdup(fallback?fallback:"");
}
static int jb_put_json_string(GmlBuiltinJsonWriter *b, const char *s){
  if(!gml_builtin_json_writer_putc(b,'"')) return 0;
  for(const unsigned char *p=(const unsigned char*)(s?s:""); *p; p++){
    char tmp[8];
    switch(*p){
      case '"': if(!gml_builtin_json_writer_puts(b,"\\\"")) return 0; break;
      case '\\': if(!gml_builtin_json_writer_puts(b,"\\\\")) return 0; break;
      case '\b': if(!gml_builtin_json_writer_puts(b,"\\b")) return 0; break;
      case '\f': if(!gml_builtin_json_writer_puts(b,"\\f")) return 0; break;
      case '\n': if(!gml_builtin_json_writer_puts(b,"\\n")) return 0; break;
      case '\r': if(!gml_builtin_json_writer_puts(b,"\\r")) return 0; break;
      case '\t': if(!gml_builtin_json_writer_puts(b,"\\t")) return 0; break;
      default:
        if(*p<0x20){ snprintf(tmp,sizeof(tmp),"\\u%04x",*p); if(!gml_builtin_json_writer_puts(b,tmp)) return 0; }
        else if(!gml_builtin_json_writer_putc(b,(char)*p)) return 0;
    }
  }
  return gml_builtin_json_writer_putc(b,'"');
}
GmlVal var_store_clone(GmlVal v);
static int json_struct_skip_key(const char *key){
  return key && (!strcmp(key,"__fn") || !strcmp(key,"__self") ||
                 !strcmp(key,"__name") || !strcmp(key,"__ctor"));
}
static int json_encode_struct(GmlVM *vm, GmlBuiltinJsonWriter *b, GmlInstance *st, int depth, int allow_ds){
  if(depth>16) return gml_builtin_json_writer_puts(b,"null");
  if(!st) return gml_builtin_json_writer_puts(b,"null");
  if(!gml_builtin_json_writer_putc(b,'{')) return 0;
  int first=1;
  for(int i=0;i<st->vars.cap;i++){
    GmlVarSlot *slot=&st->vars.slots[i];
    if(!slot->key || json_struct_skip_key(slot->key)) continue;
    if(!first && !gml_builtin_json_writer_putc(b,',')) return 0;
    first=0;
    if(!jb_put_json_string(b,slot->key) || !gml_builtin_json_writer_putc(b,':')) return 0;
    if(!gml_builtin_json_writer_put_value(vm,b,slot->val,depth+1,allow_ds)) return 0;
  }
  return gml_builtin_json_writer_putc(b,'}');
}
static int json_encode_map(GmlVM *vm, GmlBuiltinJsonWriter *b, int id, int depth){
  if(depth>16) return gml_builtin_json_writer_puts(b,"null");
  int count=gml_ds_map_view_count(vm,id);
  if(count<0) return gml_builtin_json_writer_puts(b,"null");
  if(!gml_builtin_json_writer_putc(b,'{')) return 0;
  for(int i=0;i<count;i++){
    GmlBuiltinMapItemView item={0};
    if(!gml_ds_map_item_view(vm,id,i,&item)) return 0;
    if(i && !gml_builtin_json_writer_putc(b,',')) return 0;
    char keybuf[64]; const char *key=NULL;
    if(item.key.t==V_STR) key=item.key.s?item.key.s:"";
    else { snprintf(keybuf,sizeof(keybuf),"%.17g",item.key.t==V_REAL?item.key.d:0.0); key=keybuf; }
    if(!jb_put_json_string(b,key) || !gml_builtin_json_writer_putc(b,':')) return 0;
    if(!gml_builtin_json_writer_put_value(
         vm,b,item.value,depth+1,
         item.nested_kind==1?2:(item.nested_kind==2?3:0))) return 0;
  }
  return gml_builtin_json_writer_putc(b,'}');
}
static int json_encode_list(GmlVM *vm, GmlBuiltinJsonWriter *b, int id, int depth){
  if(depth>16) return gml_builtin_json_writer_puts(b,"null");
  int count=gml_ds_list_view_count(vm,id);
  if(count<0) return gml_builtin_json_writer_puts(b,"null");
  if(!gml_builtin_json_writer_putc(b,'[')) return 0;
  for(int i=0;i<count;i++){
    GmlBuiltinListItemView item={0};
    if(!gml_ds_list_item_view(vm,id,i,&item)) return 0;
    if(i && !gml_builtin_json_writer_putc(b,',')) return 0;
    if(!gml_builtin_json_writer_put_value(
         vm,b,item.value,depth+1,
         item.nested_kind==1?2:(item.nested_kind==2?3:0))) return 0;
  }
  return gml_builtin_json_writer_putc(b,']');
}
static int json_encode_arr(GmlVM *vm, GmlBuiltinJsonWriter *b, GmlArr *A, int depth){
  if(depth>16) return gml_builtin_json_writer_puts(b,"null");
  if(!A) return gml_builtin_json_writer_puts(b,"[]");
  if(!gml_builtin_json_writer_putc(b,'[')) return 0;
  for(int i=0;i<A->len;i++){
    if(i && !gml_builtin_json_writer_putc(b,',')) return 0;
    if(!gml_builtin_json_writer_put_value(vm,b,A->data[i],depth+1,0)) return 0;
  }
  return gml_builtin_json_writer_putc(b,']');
}
int gml_builtin_json_writer_put_value(GmlVM *vm, GmlBuiltinJsonWriter *b,
                                      GmlVal v, int depth, int allow_ds){
  if(v.t==V_STR) return jb_put_json_string(b,v.s?v.s:"");
  if(v.t==V_UNDEF) return gml_builtin_json_writer_puts(b,"null");
  if(v.t==V_ARR) return json_encode_arr(vm,b,(GmlArr*)v.arr,depth);
  if(v.t==V_REAL){
    int id=(int)v.d;
    if(GML_IS_STRUCT_ID(v.d)) return json_encode_struct(vm,b,gml_struct_find(vm,(unsigned)v.d),depth,allow_ds);
    if(fabs(v.d-(double)id)<1e-9){
      if((allow_ds==1 || allow_ds==3) &&
         gml_ds_map_view_count(vm,id)>=0)
        return json_encode_map(vm,b,id,depth);
      if((allow_ds==1 || allow_ds==2) &&
         gml_ds_list_view_count(vm,id)>=0)
        return json_encode_list(vm,b,id,depth);
    }
    if(!isfinite(v.d)) return gml_builtin_json_writer_puts(b,"null");
    char num[64]; snprintf(num,sizeof(num),"%.17g",v.d); return gml_builtin_json_writer_puts(b,num);
  }
  return gml_builtin_json_writer_puts(b,"null");
}

typedef struct { const char *s; size_t p, n; GmlVM *vm; int ok, obj_as_struct; } JsonIn;
static void js_ws(JsonIn *j){ while(j->p<j->n && (j->s[j->p]==' '||j->s[j->p]=='\n'||j->s[j->p]=='\r'||j->s[j->p]=='\t')) j->p++; }
static int js_consume(JsonIn *j, char c){ js_ws(j); if(j->p<j->n && j->s[j->p]==c){ j->p++; return 1; } return 0; }
static char *js_string(JsonIn *j){
  js_ws(j); if(j->p>=j->n || j->s[j->p++]!='"'){ j->ok=0; return strdup(""); }
  GmlBuiltinJsonWriter b={0};
  while(j->p<j->n){
    unsigned char c=(unsigned char)j->s[j->p++];
    if(c=='"') return gml_builtin_json_writer_take(&b,"");
    if(c=='\\'){
      if(j->p>=j->n){ j->ok=0; break; }
      c=(unsigned char)j->s[j->p++];
      if(c=='"'||c=='\\'||c=='/') gml_builtin_json_writer_putc(&b,(char)c);
      else if(c=='b') gml_builtin_json_writer_putc(&b,'\b');
      else if(c=='f') gml_builtin_json_writer_putc(&b,'\f');
      else if(c=='n') gml_builtin_json_writer_putc(&b,'\n');
      else if(c=='r') gml_builtin_json_writer_putc(&b,'\r');
      else if(c=='t') gml_builtin_json_writer_putc(&b,'\t');
      else if(c=='u'){
        int v=0;
        for(int k=0;k<4 && j->p<j->n;k++){
          char h=j->s[j->p++]; v*=16;
          if(h>='0'&&h<='9') v+=h-'0';
          else if(h>='a'&&h<='f') v+=h-'a'+10;
          else if(h>='A'&&h<='F') v+=h-'A'+10;
          else { j->ok=0; break; }
        }
        gml_builtin_json_writer_putc(&b,(v>=0x20 && v<0x80)?(char)v:'?');
      } else { j->ok=0; break; }
    } else gml_builtin_json_writer_putc(&b,(char)c);
  }
  gml_builtin_json_writer_discard(&b); j->ok=0; return strdup("");
}
static int js_arr_push(GmlArr *A, GmlVal v){
  if(A->len>=A->cap){
    int nc=A->cap?A->cap*2:8;
    GmlVal *nd=realloc(A->data,(size_t)nc*sizeof(GmlVal));
    if(!nd) return 0;
    A->data=nd; A->cap=nc;
  }
  A->data[A->len++]=v; return 1;
}


static GmlVal json_parse_value(JsonIn *j, int depth);
static GmlVal json_parse_array(JsonIn *j, int depth){
  if(!js_consume(j,'[')){ j->ok=0; return vreal(0); }
  GmlArr *A=NULL; int list_id=-1;
  if(j->obj_as_struct){
    A=calloc(1,sizeof(*A)); if(!A){ j->ok=0; return vreal(0); }
    A->escaped=1;
  } else {
    list_id=ds_list_create_id(j->vm);
    if(list_id<0){ j->ok=0; return vreal(0); }
  }
  js_ws(j);
  if(js_consume(j,']')){
    if(!j->obj_as_struct) return vreal(list_id);
    GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
  }
  for(;;){
    js_ws(j); char lead=(j->p<j->n)?j->s[j->p]:0;
    GmlVal v=json_parse_value(j,depth+1);
    if(!j->ok){ j->ok=0; return vreal(0); }
    if(j->obj_as_struct){
      if(!js_arr_push(A,v)){ j->ok=0; return vreal(0); }
    } else {
      if(!gml_ds_list_append_direct(
           j->vm,list_id,v,lead=='['?1:(lead=='{'?2:0))){
        j->ok=0;
        return vreal(0);
      }
    }
    if(js_consume(j,']')) break;
    if(!js_consume(j,',')){ j->ok=0; return vreal(0); }
  }
  if(!j->obj_as_struct) return vreal(list_id);
  GmlVal out=vreal(0); out.t=V_ARR; out.arr=A; return out;
}
static GmlVal json_parse_object(JsonIn *j, int depth){
  if(!js_consume(j,'{')){ j->ok=0; return vreal(0); }
  if(j->obj_as_struct){
    GmlInstance *st=gml_struct_new(j->vm);
    if(!st){ j->ok=0; return vreal(0); }
    js_ws(j);
    if(js_consume(j,'}')) return vreal((double)st->id);
    for(;;){
      char *key=js_string(j);
      if(!j->ok){ free(key); return vreal(0); }
      if(!js_consume(j,':')){ free(key); j->ok=0; return vreal(0); }
      GmlVal val=json_parse_value(j,depth+1);
      if(!j->ok){ free(key); return vreal(0); }
      *gml_varmap_put(&st->vars,key)=var_store_clone(val);
      if(js_consume(j,'}')) break;
      if(!js_consume(j,',')){ j->ok=0; return vreal(0); }
    }
    return vreal((double)st->id);
  }
  int id=ds_map_create_id(j->vm); if(id<0){ j->ok=0; return vreal(0); }
  js_ws(j);
  if(js_consume(j,'}')) return vreal(id);
  for(;;){
    char *key=js_string(j);
    if(!j->ok){ free(key); return vreal(0); }
    if(!js_consume(j,':')){ free(key); j->ok=0; return vreal(0); }
    js_ws(j); char lead=(j->p<j->n)?j->s[j->p]:0;
    GmlVal val=json_parse_value(j,depth+1);
    if(!j->ok){ free(key); return vreal(0); }
    ds_map_put(j->vm,id,vstr(key),val,1);
    ds_map_mark_child(j->vm,id,vstr(key),lead=='['?1:(lead=='{'?2:0));
    free(key);
    if(js_consume(j,'}')) break;
    if(!js_consume(j,',')){ j->ok=0; return vreal(0); }
  }
  return vreal(id);
}
static int js_match(JsonIn *j, const char *lit){
  size_t n=strlen(lit);
  if(j->p+n<=j->n && !strncmp(j->s+j->p,lit,n)){ j->p+=n; return 1; }
  return 0;
}
static GmlVal json_parse_value(JsonIn *j, int depth){
  if(depth>128){ j->ok=0; return vreal(0); }
  js_ws(j); if(j->p>=j->n){ j->ok=0; return vreal(0); }
  char c=j->s[j->p];
  if(c=='{') return json_parse_object(j,depth);
  if(c=='[') return json_parse_array(j,depth);
  if(c=='"'){ char *s=js_string(j); return vstr_owned(s); }
  if(c=='t'){ if(js_match(j,"true")) return vreal(1); j->ok=0; return vreal(0); }
  if(c=='f'){ if(js_match(j,"false")) return vreal(0); j->ok=0; return vreal(0); }
  if(c=='n'){ if(js_match(j,"null")) return vundef(); j->ok=0; return vreal(0); }
  char *end=NULL; double d=strtod(j->s+j->p,&end);
  if(end==j->s+j->p){ j->ok=0; return vreal(0); }
  j->p=(size_t)(end-j->s); return vreal(d);
}
static GmlVal json_decode_text_mode(GmlVM *vm, const char *s, int obj_as_struct){
  JsonIn j={s?s:"",0,s?strlen(s):0,vm,1,obj_as_struct};
  GmlVal v=json_parse_value(&j,0);
  js_ws(&j);
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(builtin_setting(vm,"GML_DBG_JSON") && (!state || state->json_log_count++<12)){
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[json] len=%" PRIu64 " ok=%d consumed=%" PRIu64 "/%" PRIu64 " head='%.60s'\n",
      (uint64_t)(s?strlen(s):0), j.ok, (uint64_t)j.p, (uint64_t)j.n, s?s:""); }
  if(!j.ok || j.p!=j.n) return obj_as_struct?vundef():vreal(-1);
  return v;
}
GmlVal gml_builtin_json_decode_ds(GmlVM *vm, const char *s){
  const char *p=s?s:"";
  while(*p && isspace((unsigned char)*p)) p++;
  int top_object=*p=='{';
  GmlVal decoded=json_decode_text_mode(vm,s,0);
  if(decoded.t==V_REAL && decoded.d==-1) return decoded;
  /* Legacy json_decode always returns a DS map. Non-object roots live under
   * the key "default"; arrays are marked as owned lists just like nested JSON. */
  if(top_object) return decoded;
  int root=ds_map_create_id(vm);
  if(root<0){
    if(*p=='[' && decoded.t==V_REAL) ds_list_destroy_id(vm,(int)decoded.d);
    return vreal(-1);
  }
  ds_map_put(vm,root,vstr("default"),decoded,1);
  if(*p=='[') ds_map_mark_child(vm,root,vstr("default"),1);
  return vreal(root);
}
static GmlVal json_encode_root(GmlVM *vm, GmlVal v){
  GmlBuiltinJsonWriter b={0};
  if(!gml_builtin_json_writer_put_value(vm,&b,v,0,1)){
    gml_builtin_json_writer_discard(&b);
    return vstr_owned(strdup("{}"));
  }
  return vstr_owned(gml_builtin_json_writer_take(&b,"null"));
}

GmlVal gml_builtin_try_json(GmlVM *vm, const char *nm, GmlVal *a, int n){
  if(!strcmp(nm,"json_decode"))
    return gml_builtin_json_decode_ds(vm,S(vm,a,n,0));
  if(!strcmp(nm,"json_encode"))
    return json_encode_root(vm,n>0?a[0]:vundef());
  if(!strcmp(nm,"json_parse"))
    return json_decode_text_mode(vm,S(vm,a,n,0),1);
  if(!strcmp(nm,"json_stringify"))
    return json_encode_root(vm,n>0?a[0]:vundef());
  return gml_builtin_try_values_variables(vm,nm,a,n);
}
