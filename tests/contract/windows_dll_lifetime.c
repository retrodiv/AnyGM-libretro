/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* MinGW x64 static-runtime contract. Resolve the unstripped image's COFF symbol
 * rather than exporting a private runtime function or pinning its link offset. */
static void (*real_free)(void *);
static LPVOID (WINAPI *real_tls_get)(DWORD);
static BOOL (WINAPI *real_tls_set)(DWORD, LPVOID);
static DWORD tls_key;
static void *tls_array;
static unsigned capture_tls, tls_freed, positive_reads;

static void event(const char *name, void *pointer) {
  char line[160];
  int size=snprintf(line,sizeof line,"[dll-lifetime] %s pointer=%p freed=%u\r\n",
                    name,pointer,tls_freed);
  DWORD written;
  if(size>0 && size<(int)sizeof line)
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,(DWORD)size,&written,NULL);
}

static void observe_free(void *pointer) {
  if(pointer && pointer==tls_array){ tls_freed=1; event("free-tls-array",pointer); }
  real_free(pointer);
}

static LPVOID WINAPI observe_get(DWORD key) {
  LPVOID pointer=real_tls_get(key);
  DWORD error=GetLastError();
  if(capture_tls){ tls_key=key; tls_array=pointer; }
  if(pointer && key==tls_key && pointer==tls_array){
    event("read-tls-array",pointer);
    if(tls_freed){
      event("FAIL-read-after-free",pointer);
      /* A failing detach callback must not recursively enter process teardown. */
      TerminateProcess(GetCurrentProcess(),86);
    }
    ++positive_reads;
  }
  SetLastError(error);
  return pointer;
}

static BOOL WINAPI observe_set(DWORD key, LPVOID pointer) {
  BOOL result=real_tls_set(key,pointer);
  DWORD error=GetLastError();
  if(capture_tls && result){ tls_key=key; tls_array=pointer; }
  SetLastError(error);
  return result;
}

static int replace_import(HMODULE module,const char *name,uintptr_t replacement,
                          uintptr_t *previous) {
  unsigned char *base=(void *)module;
  IMAGE_DOS_HEADER *dos=(void *)base;
  IMAGE_NT_HEADERS64 *nt=(void *)(base+dos->e_lfanew);
  DWORD size=nt->OptionalHeader.SizeOfImage;
  DWORD offset=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if(!offset || offset>size-sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;
  for(unsigned i=0;i<128;i++){
    uint64_t position=(uint64_t)offset+i*sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if(position>size-sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;
    IMAGE_IMPORT_DESCRIPTOR *entry=(void *)(base+position);
    if(!entry->Name) break;
    if(!entry->OriginalFirstThunk || !entry->FirstThunk) return 0;
    for(unsigned j=0;j<4096;j++){
      uint64_t n=(uint64_t)entry->OriginalFirstThunk+j*sizeof(IMAGE_THUNK_DATA64);
      uint64_t a=(uint64_t)entry->FirstThunk+j*sizeof(IMAGE_THUNK_DATA64);
      if(n>size-sizeof(IMAGE_THUNK_DATA64) || a>size-sizeof(IMAGE_THUNK_DATA64)) return 0;
      IMAGE_THUNK_DATA64 *names=(void *)(base+n),*addresses=(void *)(base+a);
      if(!names->u1.AddressOfData) break;
      if(IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      uint64_t rva=names->u1.AddressOfData;
      if(rva>size-sizeof(IMAGE_IMPORT_BY_NAME)) return 0;
      IMAGE_IMPORT_BY_NAME *symbol=(void *)(base+rva);
      size_t available=size-rva-2;
      if(!memchr(symbol->Name,0,available)) return 0;
      if(strcmp((char *)symbol->Name,name)) continue;
      DWORD protection,ignored;
      if(!VirtualProtect(&addresses->u1.Function,sizeof(ULONGLONG),PAGE_READWRITE,&protection)) return 0;
      *previous=(uintptr_t)addresses->u1.Function;
      addresses->u1.Function=(ULONGLONG)replacement;
      return VirtualProtect(&addresses->u1.Function,sizeof(ULONGLONG),protection,&ignored)!=0;
    }
  }
  return 0;
}

static void *runtime_symbol(const char *path,HMODULE module,const char *wanted) {
  HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
  LARGE_INTEGER length;
  if(file==INVALID_HANDLE_VALUE) return NULL;
  if(!GetFileSizeEx(file,&length) || length.QuadPart<(LONGLONG)sizeof(IMAGE_DOS_HEADER)){
    CloseHandle(file); return NULL;
  }
  uint64_t size=(uint64_t)length.QuadPart;
  HANDLE mapping=CreateFileMappingW(file,NULL,PAGE_READONLY,0,0,NULL);
  unsigned char *data=mapping?MapViewOfFile(mapping,FILE_MAP_READ,0,0,0):NULL;
  void *result=NULL;
  if(!data) goto done;
  IMAGE_DOS_HEADER *dos=(void *)data;
  if(size<sizeof(IMAGE_NT_HEADERS64) || dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 ||
     (uint64_t)dos->e_lfanew>size-sizeof(IMAGE_NT_HEADERS64)) goto done;
  IMAGE_NT_HEADERS64 *nt=(void *)(data+dos->e_lfanew);
  if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
     nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) goto done;
  uint64_t count=nt->FileHeader.NumberOfSymbols,offset=nt->FileHeader.PointerToSymbolTable;
  if(!offset || !count || count>2000000 || offset>size || count>(size-offset)/sizeof(IMAGE_SYMBOL)) goto done;
  uint64_t strings=offset+count*sizeof(IMAGE_SYMBOL);
  if(strings>size-4) goto done;
  uint32_t string_bytes;
  memcpy(&string_bytes,data+strings,4);
  if(string_bytes<4 || string_bytes>size-strings) goto done;
  IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
  uint64_t section_offset=(unsigned char *)sections-data;
  if(section_offset>size || nt->FileHeader.NumberOfSections>(size-section_offset)/sizeof(*sections)) goto done;
  IMAGE_SYMBOL *symbols=(void *)(data+offset);
  for(uint64_t i=0;i<count;){
    IMAGE_SYMBOL *symbol=symbols+i;
    char short_name[9]={0};
    const char *name=short_name;
    if(symbol->N.Name.Short) memcpy(short_name,symbol->N.ShortName,8);
    else {
      uint32_t start=symbol->N.Name.Long;
      if(start<4 || start>=string_bytes || !memchr(data+strings+start,0,string_bytes-start)) goto done;
      name=(char *)data+strings+start;
    }
    if(!strcmp(name,wanted) && symbol->SectionNumber>0 &&
       symbol->SectionNumber<=nt->FileHeader.NumberOfSections){
      uint64_t rva=(uint64_t)sections[symbol->SectionNumber-1].VirtualAddress+symbol->Value;
      if(rva<nt->OptionalHeader.SizeOfImage) result=(unsigned char *)module+rva;
      break;
    }
    uint64_t step=1u+symbol->NumberOfAuxSymbols;
    if(step>count-i) goto done;
    i+=step;
  }
done:
  if(data) UnmapViewOfFile(data);
  if(mapping) CloseHandle(mapping);
  CloseHandle(file);
  return result;
}

int main(int argc,char **argv) {
  if(argc!=3 || (strcmp(argv[1],"unload") && strcmp(argv[1],"exit"))){
    fputs("usage: windows_dll_lifetime unload|exit unstripped-dll\n",stderr); return 2;
  }
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  HMODULE module=LoadLibraryA(argv[2]);
  if(!module){ fprintf(stderr,"DLL load failed: %lu\n",(unsigned long)GetLastError()); return 3; }
  void *(*address)(void *)=runtime_symbol(argv[2],module,"__emutls_get_address");
  if(!address){ fputs("The contract requires an unstripped MinGW x64 image with emulated TLS\n",stderr); return 4; }
  uintptr_t prior_free,prior_get,prior_set;
  if(!replace_import(module,"free",(uintptr_t)observe_free,&prior_free) ||
     !replace_import(module,"TlsGetValue",(uintptr_t)observe_get,&prior_get) ||
     !replace_import(module,"TlsSetValue",(uintptr_t)observe_set,&prior_set)) return 5;
  real_free=(void *)prior_free;
  real_tls_get=(void *)prior_get;
  real_tls_set=(void *)prior_set;
  /* Compiler-emitted GCC emutls descriptor ABI, not a runtime state fixture. */
  struct { uintptr_t size,alignment,slot; void *initial; } descriptor={8,8,0,NULL};
  capture_tls=1;
  uint64_t *value=address(&descriptor);
  capture_tls=0;
  if(!value || *value || !tls_array) return 6;
  *value=UINT64_C(0x1234abcd);
  if(address(&descriptor)!=value || *value!=UINT64_C(0x1234abcd) || !positive_reads) return 7;
  event("positive-tls-reuse",tls_array);
  if(!strcmp(argv[1],"unload") && !FreeLibrary(module)) return 8;
  event("main-return",NULL);
  return 0;
}
