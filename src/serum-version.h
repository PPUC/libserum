#pragma once

#define SERUM_VERSION_MAJOR 2         // X Digits
#define SERUM_VERSION_MINOR 99        // Max 2 Digits
#define SERUM_VERSION_PATCH 11        // Max 2 Digits
#define SERUM_CONCENTRATE_VERSION 17  // Max 2 Digits
// The newest cROM format this library reads (the header's format version,
// header revision 21 on; 0 for a cROM without it). 1: up to
// MAX_SPRITES_PER_FRAME sprites per frame, the count in the header, 16-bit
// sprite IDs.
#define SERUM_CROM_FORMAT_VERSION 1

#define _SERUM_STR(x) #x
#define SERUM_STR(x) _SERUM_STR(x)

#define SERUM_VERSION            \
  SERUM_STR(SERUM_VERSION_MAJOR) \
  "." SERUM_STR(SERUM_VERSION_MINOR) "." SERUM_STR(SERUM_VERSION_PATCH)
#define SERUM_MINOR_VERSION \
  SERUM_STR(SERUM_VERSION_MAJOR) "." SERUM_STR(SERUM_VERSION_MINOR)
