#ifndef _PARENT_DB_H
#define _PARENT_DB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *FindParentGameID(const char *child_id);

#ifdef __cplusplus
}
#endif

#endif // _PARENT_DB_H
