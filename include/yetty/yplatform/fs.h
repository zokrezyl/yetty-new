/*
 * yplatform/fs.h - Cross-platform filesystem helpers
 */

#ifndef YETTY_YPLATFORM_FS_H
#define YETTY_YPLATFORM_FS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create a single directory (returns 0 on success or if already exists) */
int yplatform_mkdir(const char *path);

/* Create directory and all parent directories */
void yplatform_mkdir_p(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YPLATFORM_FS_H */
