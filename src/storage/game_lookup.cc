#include "storage/game_lookup.h"
#include "storage/storage.h"
#include "storage/parent_db.h"
#include "logging.h"

#include <sdkconfig.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <esp_log.h>

namespace openmc
{
  namespace storage
  {
    static const char *const kStopWords[] = {
      "the", "a", "an", "of", "and", "disc", "disco", "cd", "usa", "europe", "ntsc", "pal"
    };
    static const size_t kStopWordCount = sizeof(kStopWords) / sizeof(kStopWords[0]);

    static bool IsStopWord(const char *tok, size_t len)
    {
      if (len <= 1)
      {
        return true;
      }
      for (size_t i = 0; i < kStopWordCount; i++)
      {
        if (strlen(kStopWords[i]) == len && strncasecmp(tok, kStopWords[i], len) == 0)
        {
          return true;
        }
      }
      return false;
    }

    static size_t TokenizeString(const char *src, char tokens[32][32], size_t max_tokens)
    {
      size_t count = 0;
      size_t pos = 0;
      char cur[32];
      size_t cur_len = 0;

      while (src[pos] != '\0' && count < max_tokens)
      {
        unsigned char c = (unsigned char)src[pos];
        if (isalnum(c))
        {
          if (cur_len < sizeof(cur) - 1)
          {
            cur[cur_len++] = (char)tolower(c);
          }
        }
        else
        {
          if (cur_len > 0)
          {
            cur[cur_len] = '\0';
            if (!IsStopWord(cur, cur_len))
            {
              strlcpy(tokens[count], cur, sizeof(tokens[count]));
              count++;
            }
            cur_len = 0;
          }
        }
        pos++;
      }

      if (cur_len > 0 && count < max_tokens)
      {
        cur[cur_len] = '\0';
        if (!IsStopWord(cur, cur_len))
        {
          strlcpy(tokens[count], cur, sizeof(tokens[count]));
          count++;
        }
      }

      return count;
    }

    static int ExtractDiscNumber(const char *str)
    {
      if (!str)
      {
        return 0;
      }
      const char *p = str;
      while (*p != '\0')
      {
        int match_len = 0;
        if (strncasecmp(p, "disco", 5) == 0 && !isalnum((unsigned char)p[5]))
        {
          match_len = 5;
        }
        else if (strncasecmp(p, "disc", 4) == 0 && !isalpha((unsigned char)p[4]))
        {
          match_len = 4;
        }
        else if (strncasecmp(p, "cd", 2) == 0 && !isalpha((unsigned char)p[2]))
        {
          match_len = 2;
        }

        if (match_len > 0)
        {
          const char *sub = p + match_len;
          while (*sub == ' ' || *sub == '\t' || *sub == '-' || *sub == '_' || *sub == '#' || *sub == ':')
          {
            sub++;
          }
          if (isdigit((unsigned char)*sub))
          {
            int num = atoi(sub);
            if (num >= 1 && num <= 9)
            {
              return num;
            }
          }
        }
        p++;
      }
      return 0;
    }

    static bool IsDemoTitle(const char *title, const char *id)
    {
      if (id)
      {
        if (strncasecmp(id, "PAPX", 4) == 0 || strncasecmp(id, "PBPX", 4) == 0)
        {
          return true;
        }
      }
      if (!title)
      {
        return false;
      }

      static const char *const kDemoKeywords[] = {
        "demo", "preview", "beta", "trial", "sampler", "taikenban"
      };
      size_t kw_count = sizeof(kDemoKeywords) / sizeof(kDemoKeywords[0]);
      size_t t_len = strlen(title);

      for (size_t k = 0; k < kw_count; k++)
      {
        const char *kw = kDemoKeywords[k];
        size_t kw_len = strlen(kw);
        if (t_len < kw_len)
        {
          continue;
        }
        for (size_t i = 0; i <= t_len - kw_len; i++)
        {
          if (strncasecmp(title + i, kw, kw_len) == 0)
          {
            bool bound_before = (i == 0) || !isalnum((unsigned char)title[i - 1]);
            bool bound_after = (i + kw_len >= t_len) || !isalnum((unsigned char)title[i + kw_len]);
            if (bound_before && bound_after)
            {
              return true;
            }
          }
        }
      }
      return false;
    }

    static int GetRegionPriority(const char *id)
    {
      if (!id || strlen(id) < 4)
      {
        return 0;
      }
      if (strncasecmp(id, "SLUS", 4) == 0 || strncasecmp(id, "SCUS", 4) == 0)
      {
        return 3;
      }
      if (strncasecmp(id, "SLES", 4) == 0 || strncasecmp(id, "SCES", 4) == 0)
      {
        return 2;
      }
      if (strncasecmp(id, "SLPS", 4) == 0 || strncasecmp(id, "SCPS", 4) == 0 ||
          strncasecmp(id, "SLPM", 4) == 0 || strncasecmp(id, "SIPS", 4) == 0 ||
          strncasecmp(id, "CPCS", 4) == 0 || strncasecmp(id, "ALCH", 4) == 0 ||
          strncasecmp(id, "PAPX", 4) == 0 || strncasecmp(id, "PBPX", 4) == 0 ||
          strncasecmp(id, "SLKA", 4) == 0)
      {
        return 1;
      }
      char c = (char)toupper((unsigned char)id[3]);
      if (c == 'U') return 3;
      if (c == 'E') return 2;
      if (c == 'P' || c == 'J' || c == 'M' || c == 'K') return 1;
      return 0;
    }

    struct MatchRank
    {
      uint8_t score;
      uint8_t matched_disc_req;
      uint8_t not_demo;
      uint8_t unexplained_tokens;
      uint8_t is_first_disc;
      uint8_t region_priority;
    };

    static bool IsRankBetter(const MatchRank &a, const MatchRank &b)
    {
      if (a.score != b.score)
      {
        return a.score > b.score;
      }
      if (a.matched_disc_req != b.matched_disc_req)
      {
        return a.matched_disc_req > b.matched_disc_req;
      }
      if (a.not_demo != b.not_demo)
      {
        return a.not_demo > b.not_demo;
      }
      if (a.unexplained_tokens != b.unexplained_tokens)
      {
        return a.unexplained_tokens < b.unexplained_tokens;
      }
      if (a.is_first_disc != b.is_first_disc)
      {
        return a.is_first_disc > b.is_first_disc;
      }
      if (a.region_priority != b.region_priority)
      {
        return a.region_priority > b.region_priority;
      }
      return false;
    }

    static bool IsGameIdPattern(const char *str, char *out_id, size_t out_len)
    {
      if (!str || !out_id || out_len < 16)
      {
        return false;
      }

      const char *p = str;
      while (*p == ' ' || *p == '\t')
      {
        p++;
      }

      size_t rem_len = strlen(p);
      if (rem_len < 9)
      {
        return false;
      }

      if (!isalpha((unsigned char)p[0]) || !isalpha((unsigned char)p[1]) ||
          !isalpha((unsigned char)p[2]) || !isalpha((unsigned char)p[3]))
      {
        return false;
      }

      char prefix[5];
      for (int i = 0; i < 4; i++)
      {
        prefix[i] = (char)toupper((unsigned char)p[i]);
      }
      prefix[4] = '\0';

      const char *digits = p + 4;
      if (*digits == '-' || *digits == '_')
      {
        digits++;
      }

      for (int i = 0; i < 5; i++)
      {
        if (!isdigit((unsigned char)digits[i]))
        {
          return false;
        }
      }

      if (digits[5] != '\0' && !isspace((unsigned char)digits[5]))
      {
        return false;
      }

      char num[6];
      memcpy(num, digits, 5);
      num[5] = '\0';

      snprintf(out_id, out_len, "%s-%s", prefix, num);
      return true;
    }

    bool LookupGameByName(const char *name, GameMatch *out, size_t max, size_t *count)
    {
      if (!name || !out || max == 0 || !count)
      {
        if (count)
        {
          *count = 0;
        }
        return false;
      }

      *count = 0;

      char titles_path[CONFIG_FATFS_MAX_LFN + 1];
      snprintf(titles_path, sizeof(titles_path), "%s/titles.db", base_path);

      char direct_id[16];
      if (IsGameIdPattern(name, direct_id, sizeof(direct_id)))
      {
        FILE *f = fopen(titles_path, "r");
        if (f)
        {
          char line[kTitlesLineMax];
          const size_t id_len = strlen(direct_id);
          while (fgets(line, sizeof(line), f))
          {
            if (line[0] == '#' || line[0] == '\r' || line[0] == '\n')
            {
              continue;
            }
            if (strncasecmp(line, direct_id, id_len) == 0 && line[id_len] == '=')
            {
              const char *val = line + id_len + 1;
              strlcpy(out[0].id, direct_id, sizeof(out[0].id));
              strlcpy(out[0].title, val, sizeof(out[0].title));
              char *end = strpbrk(out[0].title, "\r\n");
              if (end)
              {
                *end = '\0';
              }

              const char *parent = FindParentGameID(direct_id);
              const char *effective_parent = (parent != NULL) ? parent : direct_id;
              char override_folder[64];
              if (ReadCardFolderOverride(effective_parent, override_folder, sizeof(override_folder)))
              {
                strlcpy(out[0].folder, override_folder, sizeof(out[0].folder));
              }
              else
              {
                strlcpy(out[0].folder, effective_parent, sizeof(out[0].folder));
              }
              out[0].confidence = 100;

              *count = 1;
              fclose(f);
              return true;
            }
          }
          fclose(f);
        }
      }

      char query_tokens[32][32];
      size_t query_token_count = TokenizeString(name, query_tokens, 32);
      if (query_token_count == 0)
      {
        return false;
      }

      int requested_disc = ExtractDiscNumber(name);

      FILE *f = fopen(titles_path, "r");
      if (!f)
      {
        return false;
      }

      MatchRank ranks[kMaxGameMatches];
      memset(ranks, 0, sizeof(ranks));

      char line[kTitlesLineMax];
      while (fgets(line, sizeof(line), f))
      {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n')
        {
          continue;
        }

        char *sep = strchr(line, '=');
        if (!sep)
        {
          continue;
        }

        *sep = '\0';
        const char *db_id = line;
        const char *db_title = sep + 1;

        char clean_title[128];
        strlcpy(clean_title, db_title, sizeof(clean_title));
        char *end = strpbrk(clean_title, "\r\n");
        if (end)
        {
          *end = '\0';
        }

        char title_tokens[32][32];
        size_t title_token_count = TokenizeString(clean_title, title_tokens, 32);
        if (title_token_count == 0)
        {
          continue;
        }

        size_t matched_query_tokens = 0;
        for (size_t q = 0; q < query_token_count; q++)
        {
          for (size_t t = 0; t < title_token_count; t++)
          {
            if (strcmp(query_tokens[q], title_tokens[t]) == 0)
            {
              matched_query_tokens++;
              break;
            }
          }
        }

        uint8_t score = (uint8_t)((matched_query_tokens * 100) / query_token_count);
        if (score >= 60)
        {
          int title_disc = ExtractDiscNumber(clean_title);
          uint8_t matched_disc_req = 0;
          if (requested_disc > 0)
          {
            matched_disc_req = (title_disc == requested_disc) ? 1 : 0;
          }

          bool is_demo = IsDemoTitle(clean_title, db_id);
          uint8_t not_demo = is_demo ? 0 : 1;

          uint8_t unexplained_tokens = 0;
          for (size_t t = 0; t < title_token_count; t++)
          {
            bool in_query = false;
            for (size_t q = 0; q < query_token_count; q++)
            {
              if (strcmp(title_tokens[t], query_tokens[q]) == 0)
              {
                in_query = true;
                break;
              }
            }
            if (!in_query)
            {
              unexplained_tokens++;
            }
          }

          uint8_t is_first_disc = (title_disc <= 1) ? 1 : 0;
          uint8_t region_prio = (uint8_t)GetRegionPriority(db_id);

          MatchRank cand_rank;
          cand_rank.score = score;
          cand_rank.matched_disc_req = matched_disc_req;
          cand_rank.not_demo = not_demo;
          cand_rank.unexplained_tokens = unexplained_tokens;
          cand_rank.is_first_disc = is_first_disc;
          cand_rank.region_priority = region_prio;

          const char *parent = FindParentGameID(db_id);
          const char *effective_parent = (parent != NULL) ? parent : db_id;
          char resolved_folder[64];
          if (!ReadCardFolderOverride(effective_parent, resolved_folder, sizeof(resolved_folder)))
          {
            strlcpy(resolved_folder, effective_parent, sizeof(resolved_folder));
          }

          GameMatch cand_match;
          strlcpy(cand_match.id, db_id, sizeof(cand_match.id));
          strlcpy(cand_match.title, clean_title, sizeof(cand_match.title));
          strlcpy(cand_match.folder, resolved_folder, sizeof(cand_match.folder));
          cand_match.confidence = score;

          size_t insert_idx = kMaxGameMatches;
          for (size_t i = 0; i < *count; i++)
          {
            if (IsRankBetter(cand_rank, ranks[i]))
            {
              insert_idx = i;
              break;
            }
          }

          if (insert_idx == kMaxGameMatches && *count < max)
          {
            insert_idx = *count;
          }

          if (insert_idx < max)
          {
            size_t move_count = (*count < max) ? *count : (max - 1);
            for (size_t i = move_count; i > insert_idx; i--)
            {
              out[i] = out[i - 1];
              ranks[i] = ranks[i - 1];
            }

            out[insert_idx] = cand_match;
            ranks[insert_idx] = cand_rank;

            if (*count < max)
            {
              (*count)++;
            }
          }
        }
      }

      fclose(f);
      return (*count > 0);
    }
  } // namespace storage
} // namespace openmc
