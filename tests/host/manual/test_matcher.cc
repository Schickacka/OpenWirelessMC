#include "storage/game_lookup.h"
#include "storage/storage.h"
#include <stdio.h>
#include <string.h>

using namespace openmc::storage;

void run_query(const char *q)
{
  printf("Q=[%s]\n", q);
  GameMatch matches[kMaxGameMatches];
  size_t count = 0;
  bool ok = LookupGameByName(q, matches, kMaxGameMatches, &count);
  if (!ok || count == 0)
  {
    printf("   count = 0\n");
    return;
  }
  for (size_t i = 0; i < count; i++)
  {
    printf("   %-10s | %-32s | folder=%-10s | %d\n",
           matches[i].id, matches[i].title, matches[i].folder, (int)matches[i].confidence);
  }
}

int main()
{
  printf("Starting test_matcher\n");
  fflush(stdout);
  strcpy(base_path, "sdcard_assets");
  run_query("Resident Evil 2 disco 2");
  run_query("chrono cross");
  run_query("qualquer coisa xyz");
  return 0;
}
