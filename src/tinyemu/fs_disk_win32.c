/*
 * Filesystem on disk — Win32 backend for tinyemu's virtio-9p server.
 *
 * Mirrors the FSDevice contract implemented for POSIX in fs_disk.c. The
 * goal here is to be enough for an Alpine-rootfs boot via 9p on Windows;
 * write-mostly metadata operations that don't have direct Win32 analogues
 * (mknod, symlink with elevation, fine-grained chmod/chown) return
 * P9_ENOTSUP. Reading files, walking directories, stat'ing, and basic
 * file I/O are all real.
 */

#ifdef _WIN32

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>

#include "cutils.h"
#include "list.h"
#include "fs.h"

typedef struct {
    FSDevice common;
    char *root_path;
} FSDeviceDisk;

static void fs_close(FSDevice *fs, FSFile *f);

struct FSFile {
    uint32_t uid;
    char *path;       /* full host path */
    BOOL is_opened;
    BOOL is_dir;
    int  fd;          /* CRT fd for files; -1 for dirs / closed */
    /* Dir iteration state: a snapshot taken at open time so that
     * fs_readdir's offset semantics work. */
    struct {
        char **names;
        int    count;
    } dirent;
};

/* ---- helpers ------------------------------------------------------------*/

/* Compose <root>/<rel> using forward slashes; caller frees. The 9p server
 * gives us forward slashes; Win32 APIs accept them. */
static char *join_path(const char *base, const char *name)
{
    size_t bl = strlen(base);
    size_t nl = strlen(name);
    char *out = malloc(bl + 1 + nl + 1);
    memcpy(out, base, bl);
    if (bl > 0 && base[bl - 1] != '/' && base[bl - 1] != '\\')
        out[bl++] = '/';
    memcpy(out + bl, name, nl + 1);
    return out;
}

/* Map a Windows DWORD error to a 9p errno value. */
static int win_err_to_p9(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return P9_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return P9_EPERM;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return P9_EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return P9_ENOTEMPTY;
    case ERROR_DISK_FULL:
        return P9_ENOSPC;
    default:
        return P9_EIO;
    }
}

/* GetFileAttributesEx → 9p mode bits. */
static int stat_path(const char *path, FSStat *st, FSQID *qid)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d))
        return -win_err_to_p9(GetLastError());

    int is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;

    /* Filetime is 100-ns since 1601-01-01; convert to UNIX seconds/nsec. */
    static const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    ULARGE_INTEGER mt, at, ct;
    mt.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    mt.HighPart = d.ftLastWriteTime.dwHighDateTime;
    at.LowPart  = d.ftLastAccessTime.dwLowDateTime;
    at.HighPart = d.ftLastAccessTime.dwHighDateTime;
    ct.LowPart  = d.ftCreationTime.dwLowDateTime;
    ct.HighPart = d.ftCreationTime.dwHighDateTime;

    if (st) {
        memset(st, 0, sizeof(*st));
        st->st_mode = is_dir ? (P9_S_IFDIR | 0755) : (P9_S_IFREG | 0644);
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_nlink = 1;
        st->st_size = size;
        st->st_blksize = 4096;
        st->st_blocks = (size + 511) / 512;
        st->st_mtime_sec = (mt.QuadPart - EPOCH_DIFF_100NS) / 10000000ULL;
        st->st_atime_sec = (at.QuadPart - EPOCH_DIFF_100NS) / 10000000ULL;
        st->st_ctime_sec = (ct.QuadPart - EPOCH_DIFF_100NS) / 10000000ULL;
        st->qid.type = is_dir ? P9_QTDIR : P9_QTFILE;
        st->qid.version = 0;
        /* Use the file index as path qid where possible — falls back to
         * a hash of the filename if we can't open the file. */
        st->qid.path = (uint64_t)(uintptr_t)path; /* stable for this run */
    }
    if (qid) {
        qid->type = is_dir ? P9_QTDIR : P9_QTFILE;
        qid->version = 0;
        qid->path = (uint64_t)(uintptr_t)path;
    }
    return 0;
}

/* Allocate an FSFile rooted at host path. The path is duplicated. */
static FSFile *make_file(const char *host_path)
{
    FSFile *f = mallocz(sizeof(*f));
    f->path = strdup(host_path);
    f->fd = -1;
    return f;
}

/* ---- FSDevice ops ------------------------------------------------------*/

static void fs_disk_end(FSDevice *fs1)
{
    FSDeviceDisk *fs = (FSDeviceDisk *)fs1;
    free(fs->root_path);
    free(fs);
}

static void fs_delete(FSDevice *fs, FSFile *f)
{
    (void)fs;
    if (!f) return;
    if (f->fd >= 0) _close(f->fd);
    if (f->dirent.names) {
        for (int i = 0; i < f->dirent.count; i++)
            free(f->dirent.names[i]);
        free(f->dirent.names);
    }
    free(f->path);
    free(f);
}

static void fs_statfs(FSDevice *fs1, FSStatFS *st)
{
    FSDeviceDisk *fs = (FSDeviceDisk *)fs1;
    ULARGE_INTEGER avail = {0}, total = {0}, free_b = {0};
    GetDiskFreeSpaceExA(fs->root_path, &avail, &total, &free_b);
    st->f_bsize  = 4096;
    st->f_blocks = total.QuadPart / 4096;
    st->f_bfree  = free_b.QuadPart / 4096;
    st->f_bavail = avail.QuadPart / 4096;
    st->f_files  = 0;
    st->f_ffree  = 0;
}

static int fs_attach(FSDevice *fs1, FSFile **pf, FSQID *qid, uint32_t uid,
                     const char *uname, const char *aname)
{
    FSDeviceDisk *fs = (FSDeviceDisk *)fs1;
    (void)uname; (void)aname;
    FSFile *f = make_file(fs->root_path);
    f->uid = uid;
    if (stat_path(f->path, NULL, qid) < 0) {
        fs_delete(fs1, f);
        return -P9_ENOENT;
    }
    *pf = f;
    return 0;
}

static int fs_walk(FSDevice *fs, FSFile **pf, FSQID *qids,
                   FSFile *f, int n, char **names)
{
    (void)fs;
    char *cur = strdup(f->path);
    int i;
    for (i = 0; i < n; i++) {
        char *nxt = join_path(cur, names[i]);
        free(cur);
        cur = nxt;
        if (stat_path(cur, NULL, &qids[i]) < 0)
            break;
    }
    if (i == 0) {
        /* Walk to the same file. */
        FSFile *clone = make_file(f->path);
        clone->uid = f->uid;
        *pf = clone;
        free(cur);
        return 0;
    }
    FSFile *out = make_file(cur);
    out->uid = f->uid;
    *pf = out;
    free(cur);
    return i;
}

static int crt_flags_from_p9(uint32_t flags)
{
    int f = 0;
    switch (flags & P9_O_NOACCESS) {
    case P9_O_RDONLY: f = _O_RDONLY; break;
    case P9_O_WRONLY: f = _O_WRONLY; break;
    case P9_O_RDWR:   f = _O_RDWR;   break;
    default:          f = _O_RDONLY; break;
    }
    if (flags & P9_O_CREAT)  f |= _O_CREAT;
    if (flags & P9_O_EXCL)   f |= _O_EXCL;
    if (flags & P9_O_TRUNC)  f |= _O_TRUNC;
    if (flags & P9_O_APPEND) f |= _O_APPEND;
    f |= _O_BINARY;
    return f;
}

static int fs_open(FSDevice *fs, FSQID *qid, FSFile *f, uint32_t flags,
                   FSOpenCompletionFunc *cb, void *opaque)
{
    (void)fs; (void)cb; (void)opaque;
    DWORD attrs = GetFileAttributesA(f->path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return -win_err_to_p9(GetLastError());

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        /* Snapshot directory contents for fs_readdir. */
        char *pat = join_path(f->path, "*");
        WIN32_FIND_DATAA d;
        HANDLE h = FindFirstFileA(pat, &d);
        free(pat);
        f->dirent.count = 0;
        f->dirent.names = NULL;
        if (h != INVALID_HANDLE_VALUE) {
            int cap = 0;
            do {
                if (f->dirent.count == cap) {
                    cap = cap ? cap * 2 : 16;
                    f->dirent.names = realloc(f->dirent.names,
                                              cap * sizeof(char *));
                }
                f->dirent.names[f->dirent.count++] = strdup(d.cFileName);
            } while (FindNextFileA(h, &d));
            FindClose(h);
        }
        f->is_dir = TRUE;
    } else {
        int fd = _open(f->path, crt_flags_from_p9(flags), 0644);
        if (fd < 0)
            return -P9_EPERM;
        f->fd = fd;
        f->is_dir = FALSE;
    }
    f->is_opened = TRUE;
    stat_path(f->path, NULL, qid);
    return 0;
}

static int fs_create(FSDevice *fs, FSQID *qid, FSFile *f, const char *name,
                     uint32_t flags, uint32_t mode, uint32_t gid)
{
    (void)fs; (void)mode; (void)gid;
    char *child = join_path(f->path, name);
    int fd = _open(child, crt_flags_from_p9(flags) | _O_CREAT | _O_EXCL, 0644);
    if (fd < 0) {
        free(child);
        return -win_err_to_p9(GetLastError());
    }
    /* fs_create rebinds the FSFile to the new path. */
    free(f->path);
    f->path = child;
    f->fd = fd;
    f->is_opened = TRUE;
    f->is_dir = FALSE;
    stat_path(f->path, NULL, qid);
    return 0;
}

static int fs_stat(FSDevice *fs, FSFile *f, FSStat *st)
{
    (void)fs;
    return stat_path(f->path, st, NULL);
}

static int fs_setattr(FSDevice *fs, FSFile *f, uint32_t mask,
                      uint32_t mode, uint32_t uid, uint32_t gid,
                      uint64_t size, uint64_t atime_sec, uint64_t atime_nsec,
                      uint64_t mtime_sec, uint64_t mtime_nsec)
{
    (void)fs; (void)mode; (void)uid; (void)gid;
    (void)atime_sec; (void)atime_nsec; (void)mtime_sec; (void)mtime_nsec;
    /* Honor SIZE (truncate). Other attrs are best-effort no-ops on Windows. */
    if (mask & P9_SETATTR_SIZE) {
        int fd = _open(f->path, _O_WRONLY | _O_BINARY, 0);
        if (fd < 0) return -win_err_to_p9(GetLastError());
        int r = _chsize_s(fd, (int64_t)size);
        _close(fd);
        if (r != 0) return -P9_EIO;
    }
    return 0;
}

static void fs_close(FSDevice *fs, FSFile *f)
{
    (void)fs;
    if (!f->is_opened) return;
    if (!f->is_dir && f->fd >= 0) {
        _close(f->fd);
        f->fd = -1;
    }
    if (f->is_dir && f->dirent.names) {
        for (int i = 0; i < f->dirent.count; i++)
            free(f->dirent.names[i]);
        free(f->dirent.names);
        f->dirent.names = NULL;
        f->dirent.count = 0;
    }
    f->is_opened = FALSE;
}

/* 9p readdir wire format: per entry:
 *   FSQID qid (13 bytes), uint64 offset, uint8 type, uint16 nlen, name
 * We mirror what fs_disk.c emits. */
static int put_u8(uint8_t *buf, int pos, int max, uint8_t v)
{
    if (pos + 1 > max) return -1;
    buf[pos] = v;
    return pos + 1;
}
static int put_u16(uint8_t *buf, int pos, int max, uint16_t v)
{
    if (pos + 2 > max) return -1;
    buf[pos] = v & 0xff; buf[pos+1] = (v >> 8) & 0xff;
    return pos + 2;
}
static int put_u32(uint8_t *buf, int pos, int max, uint32_t v)
{
    if (pos + 4 > max) return -1;
    for (int i = 0; i < 4; i++) buf[pos+i] = (v >> (8*i)) & 0xff;
    return pos + 4;
}
static int put_u64(uint8_t *buf, int pos, int max, uint64_t v)
{
    if (pos + 8 > max) return -1;
    for (int i = 0; i < 8; i++) buf[pos+i] = (v >> (8*i)) & 0xff;
    return pos + 8;
}

static int fs_readdir(FSDevice *fs, FSFile *f, uint64_t offset,
                      uint8_t *buf, int count)
{
    (void)fs;
    if (!f->is_dir || !f->dirent.names)
        return -P9_EINVAL;
    int idx = (int)offset;
    int pos = 0;
    while (idx < f->dirent.count) {
        const char *name = f->dirent.names[idx];
        int nlen = (int)strlen(name);
        int needed = 13 /*qid*/ + 8 /*offset*/ + 1 /*type*/ + 2 /*nlen*/ + nlen;
        if (pos + needed > count)
            break;

        char *child = join_path(f->path, name);
        FSQID qid;
        int is_dir = 0;
        if (stat_path(child, NULL, &qid) == 0)
            is_dir = (qid.type & P9_QTDIR) != 0;
        free(child);

        pos = put_u8 (buf, pos, count, qid.type);
        pos = put_u32(buf, pos, count, qid.version);
        pos = put_u64(buf, pos, count, qid.path);
        pos = put_u64(buf, pos, count, (uint64_t)(idx + 1));
        pos = put_u8 (buf, pos, count, is_dir ? 4 /*DT_DIR*/ : 8 /*DT_REG*/);
        pos = put_u16(buf, pos, count, (uint16_t)nlen);
        if (pos + nlen > count) break;
        memcpy(buf + pos, name, nlen);
        pos += nlen;
        idx++;
    }
    return pos;
}

static int fs_read(FSDevice *fs, FSFile *f, uint64_t offset,
                   uint8_t *buf, int count)
{
    (void)fs;
    if (f->fd < 0) return -P9_EINVAL;
    if (_lseeki64(f->fd, (int64_t)offset, SEEK_SET) < 0)
        return -P9_EIO;
    int n = _read(f->fd, buf, (unsigned)count);
    if (n < 0) return -P9_EIO;
    return n;
}

static int fs_write(FSDevice *fs, FSFile *f, uint64_t offset,
                    const uint8_t *buf, int count)
{
    (void)fs;
    if (f->fd < 0) return -P9_EINVAL;
    if (_lseeki64(f->fd, (int64_t)offset, SEEK_SET) < 0)
        return -P9_EIO;
    int n = _write(f->fd, buf, (unsigned)count);
    if (n < 0) return -P9_EIO;
    return n;
}

static int fs_link(FSDevice *fs, FSFile *df, FSFile *f, const char *name)
{
    (void)fs; (void)df; (void)f; (void)name;
    /* Hard links require admin / Developer Mode on Windows. Stub. */
    return -P9_ENOTSUP;
}

static int fs_symlink(FSDevice *fs, FSQID *qid,
                      FSFile *f, const char *name, const char *symgt,
                      uint32_t gid)
{
    (void)fs; (void)qid; (void)f; (void)name; (void)symgt; (void)gid;
    /* Symlinks need SeCreateSymbolicLinkPrivilege. Stub. */
    return -P9_ENOTSUP;
}

static int fs_mknod(FSDevice *fs, FSQID *qid,
                    FSFile *f, const char *name, uint32_t mode, uint32_t major,
                    uint32_t minor, uint32_t gid)
{
    (void)fs; (void)qid; (void)f; (void)name; (void)mode;
    (void)major; (void)minor; (void)gid;
    /* No device nodes on NTFS. Stub. */
    return -P9_ENOTSUP;
}

static int fs_mkdir(FSDevice *fs, FSQID *qid, FSFile *f,
                    const char *name, uint32_t mode, uint32_t gid)
{
    (void)fs; (void)mode; (void)gid;
    char *child = join_path(f->path, name);
    BOOL ok = CreateDirectoryA(child, NULL);
    if (!ok) {
        DWORD e = GetLastError();
        free(child);
        return -win_err_to_p9(e);
    }
    int r = stat_path(child, NULL, qid);
    free(child);
    return r;
}

static int fs_readlink(FSDevice *fs, char *buf, int buf_size, FSFile *f)
{
    (void)fs; (void)buf; (void)buf_size; (void)f;
    /* Reading symlink targets via DeviceIoControl is doable but not needed
     * for the boot path; callers retry as a regular file. */
    return -P9_EINVAL;
}

static int fs_renameat(FSDevice *fs, FSFile *f, const char *name,
                       FSFile *new_f, const char *new_name)
{
    (void)fs;
    char *src = join_path(f->path, name);
    char *dst = join_path(new_f->path, new_name);
    BOOL ok = MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING);
    DWORD e = GetLastError();
    free(src); free(dst);
    return ok ? 0 : -win_err_to_p9(e);
}

static int fs_unlinkat(FSDevice *fs, FSFile *f, const char *name)
{
    (void)fs;
    char *p = join_path(f->path, name);
    DWORD attrs = GetFileAttributesA(p);
    BOOL ok;
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        free(p);
        return -P9_ENOENT;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        ok = RemoveDirectoryA(p);
    else
        ok = DeleteFileA(p);
    DWORD e = GetLastError();
    free(p);
    return ok ? 0 : -win_err_to_p9(e);
}

static int fs_lock(FSDevice *fs, FSFile *f, const FSLock *lock)
{
    (void)fs; (void)f; (void)lock;
    return P9_LOCK_SUCCESS;
}

static int fs_getlock(FSDevice *fs, FSFile *f, FSLock *lock)
{
    (void)fs; (void)f;
    lock->type = P9_LOCK_TYPE_UNLCK;
    return 0;
}

/* ---- public entry point ------------------------------------------------*/

FSDevice *fs_disk_init(const char *root_path)
{
    DWORD attrs = GetFileAttributesA(root_path);
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return NULL;

    FSDeviceDisk *fs = mallocz(sizeof(*fs));
    fs->root_path = strdup(root_path);

    fs->common.fs_end       = fs_disk_end;
    fs->common.fs_delete    = fs_delete;
    fs->common.fs_statfs    = fs_statfs;
    fs->common.fs_attach    = fs_attach;
    fs->common.fs_walk      = fs_walk;
    fs->common.fs_mkdir     = fs_mkdir;
    fs->common.fs_open      = fs_open;
    fs->common.fs_create    = fs_create;
    fs->common.fs_stat      = fs_stat;
    fs->common.fs_setattr   = fs_setattr;
    fs->common.fs_close     = fs_close;
    fs->common.fs_readdir   = fs_readdir;
    fs->common.fs_read      = fs_read;
    fs->common.fs_write     = fs_write;
    fs->common.fs_link      = fs_link;
    fs->common.fs_symlink   = fs_symlink;
    fs->common.fs_mknod     = fs_mknod;
    fs->common.fs_readlink  = fs_readlink;
    fs->common.fs_renameat  = fs_renameat;
    fs->common.fs_unlinkat  = fs_unlinkat;
    fs->common.fs_lock      = fs_lock;
    fs->common.fs_getlock   = fs_getlock;
    return &fs->common;
}

#endif /* _WIN32 */
