/* Unix PTY Factory - creates PTY based on config */

#include "unix-pty.h"

#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/types.h>
#include <yetty/yqemu/qemu.h>
#include <yetty/ytelnet/telnet-pty.h>

#include <stdlib.h>
#include <sys/types.h>

/* Unix PTY factory */
struct unix_pty_factory {
    struct yetty_platform_pty_factory base;
    struct yetty_config *config;
    pid_t qemu_pid;
};

/* Forward declarations */
static void unix_pty_factory_destroy(struct yetty_platform_pty_factory *self);
static struct yetty_platform_pty_result unix_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self);

/* Factory ops table */
static const struct yetty_platform_pty_factory_ops unix_pty_factory_ops = {
    .destroy = unix_pty_factory_destroy,
    .create_pty = unix_pty_factory_create_pty,
};

static void unix_pty_factory_destroy(struct yetty_platform_pty_factory *self)
{
    struct unix_pty_factory *factory = (struct unix_pty_factory *)self;

    if (factory->qemu_pid > 0)
        qemu_stop(factory->qemu_pid);

    free(factory);
}

static struct yetty_platform_pty_result unix_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self)
{
    struct unix_pty_factory *factory = (struct unix_pty_factory *)self;
    struct yetty_config *config = factory->config;

    /* --temu: TinyEMU RISC-V VM */
    if (config && config->ops->get_bool(config, YETTY_CONFIG_KEY_TEMU, 0)) {
        return tinyemu_pty_create(config);
    }

    /* --qemu: QEMU via telnet */
    if (config && config->ops->get_bool(config, YETTY_CONFIG_KEY_QEMU, 0)) {
        if (factory->qemu_pid <= 0) {
            factory->qemu_pid = qemu_start(QEMU_TELNET_PORT);
            if (factory->qemu_pid < 0)
                return YETTY_ERR(yetty_platform_pty, "failed to start QEMU");

            if (!qemu_wait_ready(QEMU_TELNET_PORT, 5000)) {
                qemu_stop(factory->qemu_pid);
                factory->qemu_pid = 0;
                return YETTY_ERR(yetty_platform_pty, "QEMU telnet not ready");
            }
        }
        return telnet_pty_create("127.0.0.1", QEMU_TELNET_PORT);
    }

    /* Default: native forkpty */
    return fork_pty_create(config);
}

/* Factory creation - the public API */

struct yetty_platform_pty_factory_result yetty_platform_pty_factory_create(
    struct yetty_config *config,
    void *os_specific)
{
    struct unix_pty_factory *factory;

    (void)os_specific;

    factory = calloc(1, sizeof(struct unix_pty_factory));
    if (!factory)
        return YETTY_ERR(yetty_platform_pty_factory, "failed to allocate pty factory");

    factory->base.ops = &unix_pty_factory_ops;
    factory->config = config;

    return YETTY_OK(yetty_platform_pty_factory, &factory->base);
}
