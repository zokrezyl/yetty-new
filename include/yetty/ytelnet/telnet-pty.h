/* Telnet PTY - TCP/Telnet as PTY backend */

#ifndef YETTY_TELNET_PTY_H
#define YETTY_TELNET_PTY_H

#include <yetty/platform/pty.h>
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
struct yetty_yplatform_pty_result telnet_pty_create(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_TELNET_PTY_H */
