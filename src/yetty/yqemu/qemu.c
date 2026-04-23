/* QEMU - Start QEMU RISC-V VM */

#include <yetty/yqemu/qemu.h>

#include <yetty/ytrace.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Platform paths */
extern const char *yetty_yplatform_get_data_dir(void);
extern const char *yetty_yplatform_get_config_dir(void);

/* mkdir -p helper (local; we don't want to pull a bigger dep here) */
static int qemu_mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* QEMU tunables, read from <config_dir>/qemu/qemu.cfg if present. */
struct qemu_settings {
    unsigned int memory_mb;
    unsigned int smp;
    char extra_append[128];
};

static void qemu_settings_defaults(struct qemu_settings *s)
{
    s->memory_mb = 256;
    s->smp = 1;
    s->extra_append[0] = '\0';
}

/* Trim ASCII whitespace in-place (lightweight, no locale) */
static char *qemu_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

/* Parse simple "key = value" lines (# for comments). */
static void qemu_settings_load(struct qemu_settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = qemu_trim(line);
        if (*p == '\0' || *p == '#')
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = qemu_trim(p);
        char *val = qemu_trim(eq + 1);

        if (strcmp(key, "memory_mb") == 0) {
            s->memory_mb = (unsigned int)strtoul(val, NULL, 10);
        } else if (strcmp(key, "smp") == 0) {
            s->smp = (unsigned int)strtoul(val, NULL, 10);
        } else if (strcmp(key, "extra_append") == 0) {
            snprintf(s->extra_append, sizeof(s->extra_append), "%s", val);
        }
    }
    fclose(f);
}

/* Ensure a default qemu.cfg exists so users can discover/tune settings. */
static void qemu_settings_ensure_default(const char *path)
{
    if (access(path, F_OK) == 0)
        return;

    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f,
        "# yetty --qemu settings\n"
        "# Lines are key = value; '#' starts a comment.\n"
        "memory_mb = 256\n"
        "smp = 1\n"
        "# Appended verbatim to the kernel cmdline (optional):\n"
        "# extra_append =\n");
    fclose(f);
}

pid_t qemu_start(uint16_t port)
{
    const char *data_dir = yetty_yplatform_get_data_dir();
    const char *config_dir = yetty_yplatform_get_config_dir();
    char qemu_bin[512];
    char bios_path[512];
    char kernel_path[512];
    char rootfs_path[512];
    char qemu_cfg_dir[512];
    char qemu_cfg_path[512];
    char chardev_arg[128];
    char append_arg[384];
    char fsdev_arg[256];
    char memory_arg[32];
    char smp_arg[16];
    struct qemu_settings settings;
    pid_t pid;

    /* qemu binary is program-specific; shared runtime lives under yemu/. */
    snprintf(qemu_bin, sizeof(qemu_bin), "%s/qemu/qemu-system-riscv64", data_dir);
    snprintf(bios_path, sizeof(bios_path), "%s/yemu/opensbi-fw_dynamic.bin", data_dir);
    snprintf(kernel_path, sizeof(kernel_path), "%s/yemu/kernel-riscv64.bin", data_dir);
    snprintf(rootfs_path, sizeof(rootfs_path), "%s/yemu/alpine-rootfs", data_dir);

    /* User-tunable qemu settings live under <config_dir>/qemu/. */
    snprintf(qemu_cfg_dir, sizeof(qemu_cfg_dir), "%s/qemu", config_dir);
    snprintf(qemu_cfg_path, sizeof(qemu_cfg_path), "%s/qemu.cfg", qemu_cfg_dir);
    qemu_mkdir_p(qemu_cfg_dir);
    qemu_settings_ensure_default(qemu_cfg_path);
    qemu_settings_defaults(&settings);
    qemu_settings_load(&settings, qemu_cfg_path);

    if (access(qemu_bin, X_OK) != 0) {
        yerror("QEMU binary not found: %s", qemu_bin);
        return -1;
    }
    if (access(bios_path, R_OK) != 0) {
        yerror("OpenSBI not found: %s", bios_path);
        return -1;
    }
    if (access(kernel_path, R_OK) != 0) {
        yerror("Kernel not found: %s", kernel_path);
        return -1;
    }

    snprintf(chardev_arg, sizeof(chardev_arg),
        "socket,id=char0,host=127.0.0.1,port=%u,server=on,wait=off,telnet=on", port);

    if (settings.extra_append[0]) {
        snprintf(append_arg, sizeof(append_arg),
            "earlycon=sbi console=hvc0 root=/dev/root rootfstype=9p rootflags=trans=virtio rw init=/init %s",
            settings.extra_append);
    } else {
        snprintf(append_arg, sizeof(append_arg),
            "earlycon=sbi console=hvc0 root=/dev/root rootfstype=9p rootflags=trans=virtio rw init=/init");
    }

    snprintf(fsdev_arg, sizeof(fsdev_arg),
        "local,id=fsdev0,path=%s,security_model=none", rootfs_path);
    snprintf(memory_arg, sizeof(memory_arg), "%u", settings.memory_mb);
    snprintf(smp_arg, sizeof(smp_arg), "%u", settings.smp);

    yinfo("Starting QEMU on port %u (mem=%uMB smp=%u)", port,
          settings.memory_mb, settings.smp);

    pid = fork();
    if (pid < 0) {
        yerror("fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child - redirect stdio to /dev/null */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        setsid();

        execlp(qemu_bin, "qemu-system-riscv64",
            "-machine", "virt", "-smp", smp_arg, "-m", memory_arg,
            "-bios", bios_path, "-kernel", kernel_path,
            "-append", append_arg,
            "-fsdev", fsdev_arg,
            "-device", "virtio-9p-device,fsdev=fsdev0,mount_tag=/dev/root",
            "-netdev", "user,id=net0",
            "-device", "virtio-net-device,netdev=net0",
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=char0",
            "-chardev", chardev_arg,
            "-serial", "none", "-display", "none",
            NULL);
        _exit(127);
    }

    yinfo("QEMU started with PID %d", pid);
    return pid;
}

void qemu_stop(pid_t pid)
{
    if (pid <= 0)
        return;

    kill(pid, SIGTERM);
    usleep(100000);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

int qemu_wait_ready(uint16_t port, int timeout_ms)
{
    struct timespec start, now;
    char port_str[16];
    struct addrinfo hints, *res;
    int sock;

    clock_gettime(CLOCK_MONOTONIC, &start);
    snprintf(port_str, sizeof(port_str), "%u", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    while (1) {
        if (getaddrinfo("127.0.0.1", port_str, &hints, &res) == 0) {
            sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock >= 0) {
                if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
                    close(sock);
                    freeaddrinfo(res);
                    yinfo("QEMU telnet ready on port %u", port);
                    return 1;
                }
                close(sock);
            }
            freeaddrinfo(res);
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                      (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms) {
            yerror("Timeout waiting for QEMU on port %u", port);
            return 0;
        }

        usleep(100000);
    }
}
