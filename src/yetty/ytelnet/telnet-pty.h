/* Telnet PTY - TCP/Telnet as PTY backend */

#ifndef YETTY_TELNET_PTY_H
#define YETTY_TELNET_PTY_H

#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a telnet PTY connected to the specified host:port
 *
 * @param host Hostname or IP address
 * @param port TCP port number
 * @return PTY result
 */
struct yetty_platform_pty_result telnet_pty_create(const char *host, uint16_t port);

/**
 * Create a telnet PTY factory
 *
 * @param host Hostname or IP address
 * @param port TCP port number
 * @return Factory result
 */
struct yetty_platform_pty_factory_result telnet_pty_factory_create(
    const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_TELNET_PTY_H */
