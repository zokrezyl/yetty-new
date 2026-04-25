/*
 * yplatform/compat.h - Cross-platform compatibility for common POSIX functions
 */

#ifndef YETTY_YPLATFORM_COMPAT_H
#define YETTY_YPLATFORM_COMPAT_H

#ifdef _MSC_VER
#include <stdlib.h>
#include <string.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

/* POSIX setenv -> Windows _putenv_s. _putenv_s always overwrites, so the
 * `overwrite` flag is ignored. Returns 0 on success. */
static __inline int setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
    return _putenv_s(name, value);
}
#endif

#endif /* YETTY_YPLATFORM_COMPAT_H */
