/* incbin-assets.c - Embedded asset management */

#include <brotli/decode.h>
#include <yetty/ytrace.h>
#include <yetty/yplatform/fs.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YETTY_BUILD_VERSION
#define YETTY_BUILD_VERSION "dev"
#endif

#define MAX_ASSETS 1024
#define MAX_PATH_LEN 512

/* Asset entry */
struct asset_entry {
  const char *name;
  const uint8_t *data;
  size_t size;
  int compressed;
};

/* Assets container */
struct yetty_incbin_assets {
  struct asset_entry entries[MAX_ASSETS];
  size_t count;
};

/* Forward declarations for manifest registration functions */
#ifdef HAS_DATA_MANIFEST
#include "yetty_data_manifest.h"
#endif

#ifdef HAS_YCONFIG_MANIFEST
#include "yetty_yconfig_manifest.h"
#endif

#ifdef HAS_YEMU_MANIFEST
#include "yetty_yemu_manifest.h"
#endif

#ifdef HAS_QEMU_MANIFEST
#include "yetty_qemu_manifest.h"
#endif

/* Helper to add asset entry */
static void add_asset(struct yetty_incbin_assets *assets, const char *name,
                      const uint8_t *data, size_t size, int compressed) {
  if (assets->count >= MAX_ASSETS)
    return;

  assets->entries[assets->count].name = name;
  assets->entries[assets->count].data = data;
  assets->entries[assets->count].size = size;
  assets->entries[assets->count].compressed = compressed;
  assets->count++;
}

/* C callback for manifest registration */
static struct yetty_incbin_assets *g_current_assets = NULL;

static void register_asset_callback(const char *name, const uint8_t *data,
                                    size_t size, int compressed) {
  if (g_current_assets)
    add_asset(g_current_assets, name, data, size, compressed);
}

/* Create assets container */
struct yetty_incbin_assets *yetty_incbin_assets_create(void) {
  struct yetty_incbin_assets *assets;

  assets = calloc(1, sizeof(struct yetty_incbin_assets));
  if (!assets)
    return NULL;

  /* Register assets from manifests */
  g_current_assets = assets;

#ifdef HAS_DATA_MANIFEST
  register_data_assets_c(register_asset_callback);
  ydebug("Registered data assets from manifest");
#endif

#ifdef HAS_YCONFIG_MANIFEST
  register_yconfig_assets_c(register_asset_callback);
  ydebug("Registered config assets from manifest");
#endif

#ifdef HAS_YEMU_MANIFEST
  register_yemu_assets_c(register_asset_callback);
  ydebug("Registered yemu (shared RISC-V runtime) assets from manifest");
#endif

#ifdef HAS_QEMU_MANIFEST
  register_qemu_assets_c(register_asset_callback);
  ydebug("Registered qemu assets from manifest");
#endif

  g_current_assets = NULL;

  ydebug("IncbinAssets: %zu embedded assets available", assets->count);
  return assets;
}

/* Destroy assets container */
void yetty_incbin_assets_destroy(struct yetty_incbin_assets *assets) {
  free(assets);
}

/* Get marker path */
static void get_marker_path(const char *cache_dir, char *out, size_t out_size) {
  snprintf(out, out_size, "%s/.yetty-assets/version", cache_dir);
}

/* Create directory recursively */
static int mkdir_p(const char *path) {
  char tmp[MAX_PATH_LEN];
  char *p = NULL;
  size_t len;

  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  if (tmp[len - 1] == '/')
    tmp[len - 1] = 0;

  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (yplatform_mkdir(tmp) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }

  if (yplatform_mkdir(tmp) != 0 && errno != EEXIST)
    return -1;

  return 0;
}

/* Check if extraction is needed */
int yetty_incbin_assets_needs_extraction(struct yetty_incbin_assets *assets,
                                         const char *cache_dir) {
  char marker_path[MAX_PATH_LEN];
  char version[64];
  FILE *f;

  (void)assets;

  get_marker_path(cache_dir, marker_path, sizeof(marker_path));
  ydebug("needsExtraction: marker path = %s", marker_path);

  f = fopen(marker_path, "r");
  if (!f) {
    ydebug("needsExtraction: returning 1 (marker not found)");
    return 1;
  }

  if (!fgets(version, sizeof(version), f)) {
    fclose(f);
    return 1;
  }
  fclose(f);

  /* Strip newline */
  version[strcspn(version, "\n")] = 0;

  ydebug("needsExtraction: marker version = '%s', build version = '%s'",
         version, YETTY_BUILD_VERSION);

  if (strcmp(version, YETTY_BUILD_VERSION) != 0) {
    ydebug("needsExtraction: returning 1 (version mismatch)");
    return 1;
  }

  ydebug("needsExtraction: returning 0");
  return 0;
}

/* Decompress brotli data */
static uint8_t *decompress_brotli(const uint8_t *data, size_t size,
                                  size_t *out_size) {
  size_t decoded_size;
  uint8_t *output;
  size_t capacity;
  BrotliDecoderResult result;

  capacity = size * 10;
  output = malloc(capacity);
  if (!output)
    return NULL;

  do {
    decoded_size = capacity;
    result = BrotliDecoderDecompress(size, data, &decoded_size, output);
    if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
      capacity *= 2;
      uint8_t *new_output = realloc(output, capacity);
      if (!new_output) {
        free(output);
        return NULL;
      }
      output = new_output;
    }
  } while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

  if (result != BROTLI_DECODER_RESULT_SUCCESS) {
    free(output);
    return NULL;
  }

  *out_size = decoded_size;
  return output;
}

/* Extract assets with given prefix to directory, stripping the prefix */
static int extract_with_prefix(struct yetty_incbin_assets *assets,
                               const char *prefix, const char *dest_dir) {
  char path[MAX_PATH_LEN];
  size_t i;
  size_t prefix_len = strlen(prefix);
  FILE *f;

  ydebug("extract_with_prefix: prefix='%s' dest='%s'", prefix, dest_dir);

  for (i = 0; i < assets->count; i++) {
    const struct asset_entry *entry = &assets->entries[i];
    const uint8_t *write_data;
    size_t write_size;
    uint8_t *decompressed = NULL;
    char *last_slash;
    const char *rel_name;

    /* Skip assets that don't match prefix */
    if (strncmp(entry->name, prefix, prefix_len) != 0)
      continue;

    /* Strip prefix from name */
    rel_name = entry->name + prefix_len;

    snprintf(path, sizeof(path), "%s/%s", dest_dir, rel_name);
    ydebug("extractTo: extracting '%s' -> '%s'", entry->name, path);

    /* Create parent directories */
    last_slash = strrchr(path, '/');
    if (last_slash) {
      *last_slash = 0;
      if (mkdir_p(path) != 0) {
        ydebug("Failed to create directory: %s", path);
        return 0;
      }
      *last_slash = '/';
    }

    /* Decompress if needed */
    if (entry->compressed) {
      decompressed = decompress_brotli(entry->data, entry->size, &write_size);
      if (!decompressed) {
        ydebug("Failed to decompress: %s", entry->name);
        return 0;
      }
      write_data = decompressed;
      ydebug("Decompressed: %s (%zu -> %zu bytes)", entry->name, entry->size,
             write_size);
    } else {
      write_data = entry->data;
      write_size = entry->size;
    }

    /* Write file */
    f = fopen(path, "wb");
    if (!f) {
      free(decompressed);
      ydebug("Failed to open file: %s", path);
      return 0;
    }

    if (fwrite(write_data, 1, write_size, f) != write_size) {
      fclose(f);
      free(decompressed);
      ydebug("Failed to write file: %s", path);
      return 0;
    }

    fclose(f);
    free(decompressed);

    if (!entry->compressed) {
      ydebug("Extracted: %s (%zu bytes)", entry->name, write_size);
    }
  }

  return 1;
}

/* Write version marker to directory */
static void write_marker(const char *dir) {
  char marker_path[MAX_PATH_LEN];
  char marker_dir[MAX_PATH_LEN];
  FILE *f;

  get_marker_path(dir, marker_path, sizeof(marker_path));
  strncpy(marker_dir, marker_path, sizeof(marker_dir) - 1);
  marker_dir[sizeof(marker_dir) - 1] = 0;
  char *slash = strrchr(marker_dir, '/');
  if (slash) {
    *slash = 0;
    mkdir_p(marker_dir);
  }

  f = fopen(marker_path, "w");
  if (f) {
    fprintf(f, "%s", YETTY_BUILD_VERSION);
    fclose(f);
  }
}

/* Extract data assets (shaders, fonts, etc.) to data directory */
int yetty_incbin_assets_extract_data_to(struct yetty_incbin_assets *assets,
                                        const char *data_dir) {
  ydebug("extract_data_to: starting extraction to %s", data_dir);

  if (!extract_with_prefix(assets, "data/", data_dir))
    return 0;

  write_marker(data_dir);
  ydebug("Data asset extraction complete");
  return 1;
}

/* Extract config assets to config directory.
 * Assets are registered by the build with prefix "yconfig/" (see
 * incbin_add_directory in build-tools/cmake/targets/shared.cmake); the
 * prefix must match here or extraction silently matches zero entries and
 * still writes the version marker, wedging all future runs. */
int yetty_incbin_assets_extract_config_to(struct yetty_incbin_assets *assets,
                                          const char *config_dir) {
  ydebug("extract_config_to: starting extraction to %s", config_dir);

  if (!extract_with_prefix(assets, "yconfig/", config_dir))
    return 0;

  write_marker(config_dir);
  ydebug("Config asset extraction complete");
  return 1;
}

/* Extract a tarball to a directory */
static int extract_tarball(const char *tar_path, const char *dest_dir) {
  char cmd[MAX_PATH_LEN * 2 + 64];

  if (mkdir_p(dest_dir) != 0)
    return 0;

  snprintf(cmd, sizeof(cmd), "tar xf '%s' -C '%s'", tar_path, dest_dir);
  ydebug("extract_tarball: running: %s", cmd);

  int ret = system(cmd);
  if (ret != 0) {
    ydebug("extract_tarball: tar command failed with %d", ret);
    return 0;
  }

  return 1;
}

/* Extract shared RISC-V runtime (kernel, opensbi, rootfs) to <data_dir>/yemu */
int yetty_incbin_assets_extract_yemu_to(struct yetty_incbin_assets *assets,
                                        const char *data_dir) {
  char yemu_dir[MAX_PATH_LEN];
  char rootfs_tar[MAX_PATH_LEN];
  char rootfs_dir[MAX_PATH_LEN];

  snprintf(yemu_dir, sizeof(yemu_dir), "%s/yemu", data_dir);
  ydebug("extract_yemu_to: starting extraction to %s", yemu_dir);

  if (mkdir_p(yemu_dir) != 0) {
    ydebug("Failed to create yemu directory: %s", yemu_dir);
    return 0;
  }

  if (!extract_with_prefix(assets, "yemu/", yemu_dir))
    return 0;

  /* Extract alpine-rootfs.tar if present */
  snprintf(rootfs_tar, sizeof(rootfs_tar), "%s/alpine-rootfs.tar", yemu_dir);
  snprintf(rootfs_dir, sizeof(rootfs_dir), "%s/alpine-rootfs", yemu_dir);

  if (yplatform_file_exists(rootfs_tar)) {
    ydebug("extract_yemu_to: extracting rootfs tarball");
    if (!extract_tarball(rootfs_tar, rootfs_dir)) {
      ydebug("Failed to extract rootfs tarball");
      return 0;
    }
    /* Remove tarball after extraction */
    yplatform_unlink(rootfs_tar);
  }

  ydebug("yemu asset extraction complete");
  return 1;
}

/* Check if yemu (shared RISC-V runtime) assets are available */
int yetty_incbin_assets_has_yemu(struct yetty_incbin_assets *assets) {
  size_t i;

  for (i = 0; i < assets->count; i++) {
    if (strncmp(assets->entries[i].name, "yemu/", 5) == 0)
      return 1;
  }
  return 0;
}

/* Extract QEMU binary to data directory */
int yetty_incbin_assets_extract_qemu_to(struct yetty_incbin_assets *assets,
                                        const char *data_dir) {
  char qemu_dir[MAX_PATH_LEN];
  char qemu_bin[MAX_PATH_LEN];

  snprintf(qemu_dir, sizeof(qemu_dir), "%s/qemu", data_dir);
  ydebug("extract_qemu_to: starting extraction to %s", qemu_dir);

  if (mkdir_p(qemu_dir) != 0) {
    ydebug("Failed to create qemu directory: %s", qemu_dir);
    return 0;
  }

  if (!extract_with_prefix(assets, "qemu/", qemu_dir))
    return 0;

  /* Make QEMU binary executable */
  snprintf(qemu_bin, sizeof(qemu_bin), "%s/qemu-system-riscv64", qemu_dir);
  if (yplatform_chmod(qemu_bin, 0755) != 0) {
    ydebug("Failed to make QEMU executable: %s", strerror(errno));
  }

  ydebug("QEMU asset extraction complete");
  return 1;
}

/* Check if QEMU assets are available */
int yetty_incbin_assets_has_qemu(struct yetty_incbin_assets *assets) {
  size_t i;

  for (i = 0; i < assets->count; i++) {
    if (strncmp(assets->entries[i].name, "qemu/", 5) == 0)
      return 1;
  }
  return 0;
}
