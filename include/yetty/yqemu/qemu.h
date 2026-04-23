/* QEMU - Start QEMU RISC-V VM */

#ifndef YETTY_QEMU_H
#define YETTY_QEMU_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QEMU_TELNET_PORT 23000

/**
 * Start QEMU process with telnet serial on specified port
 *
 * @param port Telnet port for serial console
 * @return QEMU process ID, or -1 on error
 */
pid_t qemu_start(uint16_t port);

/**
 * Stop QEMU process
 *
 * @param pid QEMU process ID
 */
void qemu_stop(pid_t pid);

/**
 * Wait for QEMU telnet to be ready
 *
 * @param port Telnet port
 * @param timeout_ms Timeout in milliseconds
 * @return 1 if ready, 0 on timeout
 */
int qemu_wait_ready(uint16_t port, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_QEMU_H */
