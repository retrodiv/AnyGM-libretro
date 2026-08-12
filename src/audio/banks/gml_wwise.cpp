/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Wwise Vorbis packet reconstruction uses ww2ogg under src/third_party/ww2ogg.
 * See LICENSES/ww2ogg.txt and THIRD_PARTY_NOTICES.md.
 */
#include "gml_wwise.h"

#include "wwriff.h"
#include "wwise_codebooks.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

extern "C" int gml_wwise_wem_to_ogg(const uint8_t *wem,size_t wem_size,
                                     uint8_t **ogg,size_t *ogg_size){
  if(ogg) *ogg=NULL;
  if(ogg_size) *ogg_size=0;
  if(!wem || !wem_size || !ogg || !ogg_size) return 0;
  try {
    Wwise_RIFF_Vorbis converter(
      reinterpret_cast<const char *>(wem),wem_size,
      reinterpret_cast<const char *>(anygm_wwise_codebooks),
      anygm_wwise_codebooks_size,false,false,kNoForcePacketFormat);
    std::ostringstream output(std::ios::binary);
    converter.generate_ogg(output);
    const std::string bytes=output.str();
    if(bytes.empty()) return 0;
    uint8_t *copy=static_cast<uint8_t *>(std::malloc(bytes.size()));
    if(!copy) return 0;
    std::memcpy(copy,bytes.data(),bytes.size());
    *ogg=copy;
    *ogg_size=bytes.size();
    return 1;
  } catch (...) {
    return 0;
  }
}
