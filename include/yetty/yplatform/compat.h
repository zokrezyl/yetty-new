/*
 * yplatform/compat.h - Cross-platform compatibility for common POSIX functions
 */

#ifndef YETTY_YPLATFORM_COMPAT_H
#define YETTY_YPLATFORM_COMPAT_H

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#endif /* YETTY_YPLATFORM_COMPAT_H */
