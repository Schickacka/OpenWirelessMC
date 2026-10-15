#ifndef _PERCENT_DECODE_H
#define _PERCENT_DECODE_H

#include <stddef.h>

namespace openmc
{
    namespace web
    {
        // Decodes %XX escapes from `encoded` into `decoded` (always NUL terminated,
        // never writes more than `dest_size` bytes).
        //  - plus_as_space: true for application/x-www-form-urlencoded bodies ('+' -> ' '),
        //    false for URL paths, where '+' is a literal character.
        //  - "%00" is dropped (a decoded NUL would silently truncate the string).
        //  - malformed escapes ("%", "%4", "%zz") are copied literally.
        // Returns false if the output had to be truncated.
        bool PercentDecode(const char *encoded, char *decoded, size_t dest_size, bool plus_as_space = true);
    } // web
} // openmc

#endif // _PERCENT_DECODE_H
