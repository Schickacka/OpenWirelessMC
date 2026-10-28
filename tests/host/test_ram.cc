#include "storage/game_lookup.h"
#include "storage/storage.h"

static_assert(sizeof(openmc::storage::GameMatch) <= 224, "GameMatch cresceu");
static_assert(kMaxGameMatches == 3, "mais de 3 sugestoes nao cabem");
static_assert(kTitlesLineMax == 512, "linha do titles.db grande demais");
static_assert(sizeof(openmc::storage::LegacyProbeResult) <= 8, "sonda legada cresceu");

void run_ram_tests()
{
}

int main()
{
  run_ram_tests();
  return 0;
}
