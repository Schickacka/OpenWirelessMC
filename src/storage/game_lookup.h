#ifndef _GAME_LOOKUP_H
#define _GAME_LOOKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define kMaxGameMatches 3
#define kTitlesLineMax 512

namespace openmc
{
  namespace storage
  {
    struct GameMatch
    {
      char id[16];
      char title[128];
      char folder[64];
      uint8_t confidence;
    };

    bool LookupGameByName(const char *name, GameMatch *out, size_t max, size_t *count);
  } // namespace storage
} // namespace openmc

#endif // _GAME_LOOKUP_H
