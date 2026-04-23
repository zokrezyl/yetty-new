/* Unix PTY - PTY implementations for Unix platforms */

#ifndef YETTY_UNIX_PTY_H
#define YETTY_UNIX_PTY_H

#include <yetty/platform/pty-factory.h>
#include <yetty/yconfig.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fork PTY - native forkpty based */
struct yetty_platform_pty_result fork_pty_create(struct yetty_config *config);

/* TinyEMU PTY - RISC-V VM */
struct yetty_platform_pty_result tinyemu_pty_create(struct yetty_config *config);

/* Telnet PTY - TCP telnet connection */
struct yetty_platform_pty_result telnet_pty_create(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_UNIX_PTY_H */
