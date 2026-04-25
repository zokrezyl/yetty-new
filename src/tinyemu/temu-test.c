/*
 * temu-test.c — minimal driver to reproduce/diagnose tinyemu init crashes
 * on Windows without going through yetty.exe (GUI/threading/etc.).
 *
 * Usage:
 *   temu-test.exe <config.cfg>
 *
 * Each step prints a checkpoint to stderr. The first crash is the bug site.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* SEH crash handler: print the call stack with module+symbol names so we
 * see exactly where the crash is without a separate debugger. */
static LONG WINAPI temu_seh_handler(EXCEPTION_POINTERS *ep)
{
    fprintf(stderr, "\n[temu-test] *** UNHANDLED EXCEPTION 0x%08lX at %p ***\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    fflush(stderr);

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);

    CONTEXT *ctx = ep->ContextRecord;
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset    = ctx->Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp; frame.AddrStack.Mode = AddrModeFlat;

    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 256;

    for (int i = 0; i < 32; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(),
                         &frame, ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (!frame.AddrPC.Offset) break;

        DWORD64 disp = 0;
        const char *name = "??";
        if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym))
            name = sym->Name;

        IMAGEHLP_LINE64 line = {0};
        line.SizeOfStruct = sizeof(line);
        DWORD col = 0;
        if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &col, &line)) {
            fprintf(stderr, "  #%d %p  %s + 0x%llx  (%s:%lu)\n",
                    i, (void *)(uintptr_t)frame.AddrPC.Offset, name,
                    (unsigned long long)disp, line.FileName, line.LineNumber);
        } else {
            fprintf(stderr, "  #%d %p  %s + 0x%llx\n",
                    i, (void *)(uintptr_t)frame.AddrPC.Offset, name,
                    (unsigned long long)disp);
        }
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "fs.h"
#include "machine.h"

#define CHECKPOINT(msg, ...) \
    do { fprintf(stderr, "[temu-test] " msg "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

/* ---- Block device for virtio-blk root (raw image, snapshot mode) -------*/

typedef enum { BF_MODE_RO, BF_MODE_RW, BF_MODE_SNAPSHOT } BlockDeviceModeEnum;

typedef struct {
    FILE *f;
    int64_t nb_sectors;
    BlockDeviceModeEnum mode;
    uint8_t **sector_table;
} BlockDeviceFile;

static int64_t bf_get_sector_count(BlockDevice *bs)
{
    BlockDeviceFile *bf = bs->opaque;
    return bf->nb_sectors;
}

static int bf_read_async(BlockDevice *bs, uint64_t sector_num, uint8_t *buf,
                         int n, BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    BlockDeviceFile *bf = bs->opaque;
    if (bf->mode == BF_MODE_SNAPSHOT) {
        for (int i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                _fseeki64(bf->f, sector_num * 512LL, SEEK_SET);
                fread(buf, 1, 512, bf->f);
            } else {
                memcpy(buf, bf->sector_table[sector_num], 512);
            }
            sector_num++;
            buf += 512;
        }
    } else {
        _fseeki64(bf->f, sector_num * 512LL, SEEK_SET);
        fread(buf, 1, n * 512, bf->f);
    }
    return 0;
}

static int bf_write_async(BlockDevice *bs, uint64_t sector_num,
                          const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    BlockDeviceFile *bf = bs->opaque;
    switch (bf->mode) {
    case BF_MODE_RO:
        return -1;
    case BF_MODE_RW:
        _fseeki64(bf->f, sector_num * 512LL, SEEK_SET);
        fwrite(buf, 1, n * 512, bf->f);
        return 0;
    case BF_MODE_SNAPSHOT:
        for (int i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num])
                bf->sector_table[sector_num] = malloc(512);
            memcpy(bf->sector_table[sector_num], buf, 512);
            sector_num++;
            buf += 512;
        }
        return 0;
    }
    return -1;
}

static BlockDevice *block_device_init(const char *filename, BlockDeviceModeEnum mode)
{
    FILE *f = fopen(filename, mode == BF_MODE_RW ? "r+b" : "rb");
    if (!f) return NULL;

    _fseeki64(f, 0, SEEK_END);
    int64_t file_size = _ftelli64(f);

    BlockDevice *bs = mallocz(sizeof(*bs));
    BlockDeviceFile *bf = mallocz(sizeof(*bf));
    bf->f = f;
    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    if (mode == BF_MODE_SNAPSHOT)
        bf->sector_table = mallocz(sizeof(bf->sector_table[0]) * bf->nb_sectors);

    bs->opaque = bf;
    bs->get_sector_count = bf_get_sector_count;
    bs->read_async = bf_read_async;
    bs->write_async = bf_write_async;
    return bs;
}

/* Console hook so we see kernel boot bytes on stderr. */
static void temu_console_write(void *opaque, const uint8_t *buf, int len)
{
    (void)opaque;
    fwrite(buf, 1, len, stderr);
    fflush(stderr);
}
static int temu_console_read(void *opaque, uint8_t *buf, int len)
{
    (void)opaque; (void)buf; (void)len;
    return 0;   /* no input */
}
static CharacterDevice g_console = {
    .opaque = NULL,
    .write_data = temu_console_write,
    .read_data  = temu_console_read,
};

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(temu_seh_handler);
#endif

    if (argc < 2) {
        fprintf(stderr, "usage: %s <config.cfg>\n", argv[0]);
        return 1;
    }

    CHECKPOINT("config = %s", argv[1]);

    VirtMachineParams p = { 0 };
    virt_machine_set_defaults(&p);
    CHECKPOINT("set_defaults ok");

    virt_machine_load_config_file(&p, argv[1], NULL, NULL);
    CHECKPOINT("load_config_file ok: drives=%d fs=%d eth=%d",
               p.drive_count, p.fs_count, p.eth_count);

    /* No SLIRP on Windows: tab_eth[i].net stays NULL, and virtio_net_init
     * deref's it. Drop eth so the VM boots without networking. */
    p.eth_count = 0;
    CHECKPOINT("  bios_filename=%s", p.files[VM_FILE_BIOS].filename ? p.files[VM_FILE_BIOS].filename : "(null)");
    CHECKPOINT("  bios buf=%p len=%d",
               (void *)p.files[VM_FILE_BIOS].buf, p.files[VM_FILE_BIOS].len);
    CHECKPOINT("  kernel_filename=%s", p.files[VM_FILE_KERNEL].filename ? p.files[VM_FILE_KERNEL].filename : "(null)");
    CHECKPOINT("  kernel buf=%p len=%d",
               (void *)p.files[VM_FILE_KERNEL].buf, p.files[VM_FILE_KERNEL].len);
    CHECKPOINT("  cmdline=%s", p.cmdline ? p.cmdline : "(null)");
    CHECKPOINT("  ram_size=%dMB", (int)(p.ram_size >> 20));

    /* Init virtio-blk drives. */
    for (int i = 0; i < p.drive_count; i++) {
        CHECKPOINT("drive%d: filename=%s", i,
                   p.tab_drive[i].filename ? p.tab_drive[i].filename : "(null)");
        char *fname = get_file_path(p.cfg_filename, p.tab_drive[i].filename);
        CHECKPOINT("drive%d: resolved path = %s", i, fname);
        BlockDevice *bd = block_device_init(fname, BF_MODE_SNAPSHOT);
        free(fname);
        if (!bd) {
            CHECKPOINT("block_device_init FAILED for drive%d", i);
            return 4;
        }
        p.tab_drive[i].block_dev = bd;
        CHECKPOINT("drive%d: block_device_init ok (sectors=%lld)",
                   i, (long long)bd->get_sector_count(bd));
    }

    for (int i = 0; i < p.fs_count; i++) {
        CHECKPOINT("fs%d: filename=%s tag=%s", i,
                   p.tab_fs[i].filename ? p.tab_fs[i].filename : "(null)",
                   p.tab_fs[i].tag ? p.tab_fs[i].tag : "(null)");
        char *fname = get_file_path(p.cfg_filename, p.tab_fs[i].filename);
        CHECKPOINT("fs%d: resolved path = %s", i, fname);
        FSDevice *fs = fs_disk_init(fname);
        free(fname);
        if (!fs) {
            CHECKPOINT("fs_disk_init FAILED for fs%d", i);
            return 2;
        }
        p.tab_fs[i].fs_dev = fs;
        CHECKPOINT("fs%d: fs_disk_init ok", i);
    }

    p.rtc_real_time = TRUE;
    p.console = &g_console;

    CHECKPOINT("calling virt_machine_init...");
    VirtMachine *vm = virt_machine_init(&p);
    if (!vm) {
        CHECKPOINT("virt_machine_init FAILED");
        return 3;
    }
    CHECKPOINT("virt_machine_init ok");

    virt_machine_free_config(&p);
    CHECKPOINT("free_config ok");

    CHECKPOINT("CPU threads running. kernel output below ============");
    /* riscv_machine_interp is a no-op; the RISC-V CPUs run in their own
     * pthreads. virt_machine_get_sleep_duration is what FIRES the timer
     * interrupts — the main thread must call it periodically or the
     * CPUs sit in WFI forever (kernel hangs after cpuidle init). */
    for (int i = 0; i < 60 * 1000 / 10; i++) {   /* ~60 s wall time */
        virt_machine_get_sleep_duration(vm, 10);
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10 * 1000);
#endif
    }
    CHECKPOINT("DONE");
    virt_machine_end(vm);
    return 0;
}
