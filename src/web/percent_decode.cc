#include "web/percent_decode.h"

namespace openmc
{
    namespace web
    {
        static int HexToInt(const char hex)
        {
            if (hex >= '0' && hex <= '9')
            {
                return hex - '0';
            }
            if (hex >= 'A' && hex <= 'F')
            {
                return hex - 'A' + 10;
            }
            if (hex >= 'a' && hex <= 'f')
            {
                return hex - 'a' + 10;
            }
            return -1;
        }

        // Stateless on purpose: the previous implementation kept its parser state in globals,
        // so an input ending in a partial "%X" escape corrupted the next (unrelated) call and
        // concurrent callers could trample each other.
        bool PercentDecode(const char *encoded, char *decoded, size_t dest_size, bool plus_as_space)
        {
            if (decoded == NULL || dest_size == 0)
            {
                return false;
            }

            size_t out = 0;
            bool truncated = false;

            if (encoded != NULL)
            {
                for (const char *p = encoded; *p != '\0'; p++)
                {
                    char c = *p;

                    if (c == '%' && HexToInt(p[1]) >= 0 && HexToInt(p[2]) >= 0)
                    {
                        c = (char)((HexToInt(p[1]) << 4) | HexToInt(p[2]));
                        p += 2;
                        if (c == '\0')
                        {
                            continue;
                        }
                    }
                    else if (c == '+' && plus_as_space)
                    {
                        c = ' ';
                    }

                    if (out + 1 >= dest_size)
                    {
                        truncated = true;
                        break;
                    }
                    decoded[out++] = c;
                }
            }

            decoded[out] = '\0';
            return !truncated;
        }
    } // web
} // openmc
