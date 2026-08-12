/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_hash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int hex_value(char value){
  if(value>='0' && value<='9') return value-'0';
  if(value>='a' && value<='f') return value-'a'+10;
  return -1;
}

static int vector(const void *data,size_t size,const char *expected){
  uint8_t digest[32];
  gml_sha256(data,size,digest);
  for(size_t i=0;i<sizeof(digest);i++){
    int high=hex_value(expected[i*2u]);
    int low=hex_value(expected[i*2u+1u]);
    if(high<0 || low<0 || digest[i]!=(uint8_t)((high<<4)|low)) return 0;
  }
  return expected[64]=='\0';
}

int main(void){
  static const char long_message[]=
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  int ok=vector(NULL,0,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") &&
    vector("abc",3,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") &&
    vector(long_message,strlen(long_message),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  if(!ok){ fputs("SHA-256 standard vector mismatch\n",stderr); return 1; }
  puts("SHA-256 vectors: ok");
  return 0;
}
