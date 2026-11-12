/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_HASH_H
#define GML_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t bytes;
  uint8_t block[64];
  size_t used;
} GmlSha256;

void gml_sha256_init(GmlSha256 *hash);
void gml_sha256_update(GmlSha256 *hash,const void *data,size_t size);
void gml_sha256_final(GmlSha256 *hash,uint8_t digest[32]);
void gml_sha256(const void *data,size_t size,uint8_t digest[32]);
uint32_t gml_adler32(const uint8_t *bytes,size_t size);

/* Standard imohash defaults, with host-independent MurmurHash3 x64_128.
 * Readers return nonzero only after filling the requested range. Failure
 * leaves the digest untouched. This is recognition, not an integrity check. */
typedef int (*GmlHashReadAt)(void *context,uint64_t offset,void *data,size_t size);
int gml_imohash_read(GmlHashReadAt reader,void *context,uint64_t size,uint8_t digest[16]);
int gml_imohash(const void *data,size_t size,uint8_t digest[16]);

#endif
