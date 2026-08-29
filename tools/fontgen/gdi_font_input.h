/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef ANYGM_GDI_FONT_INPUT_H
#define ANYGM_GDI_FONT_INPUT_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A family name alone cannot establish which file GDI selected. These generators
 * accept individual TrueType files, not collections or installed-font aliases. */
static int verify_gdi_font_input(HDC dc, const char *utf8_path) {
  wchar_t path[1024];
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1,
                           path, (int)(sizeof path / sizeof path[0]))) return 0;
  FILE *file = _wfopen(path, L"rb");
  if (!file) return 0;
  int ok = 0;
  unsigned char *expected = NULL, *selected = NULL;
  if (fseek(file, 0, SEEK_END) != 0) goto done;
  long size = ftell(file);
  if (size <= 0 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
    goto done;
  expected = (unsigned char *)malloc((size_t)size);
  selected = (unsigned char *)malloc((size_t)size);
  if (!expected || !selected || fread(expected, 1, (size_t)size, file) != (size_t)size)
    goto done;
  if (GetFontData(dc, 0, 0, NULL, 0) != (DWORD)size ||
      GetFontData(dc, 0, 0, selected, (DWORD)size) != (DWORD)size) goto done;
  ok = memcmp(expected, selected, (size_t)size) == 0;
done:
  free(selected);
  free(expected);
  fclose(file);
  if (!ok) fprintf(stderr, "GDI did not select the exact supplied TrueType file: %s\n", utf8_path);
  return ok;
}

#endif
