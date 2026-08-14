#ifndef JW_PLATFORM_RAOFFLINEPROXY_H
#define JW_PLATFORM_RAOFFLINEPROXY_H

#include <stdbool.h>
#include <stdint.h>

/* RAOfflineProxy transient launch bridge (umrk-workspace/plans/RAOfflineProxy).
 * One bounded loopback readiness check against the supervised service's fixed
 * /leaf/health response. Only the fixed service id / protocol / ready body is
 * ever accepted; a host or port is never taken from the response. */

#define JW_ROP_SERVICE_ID "org.umrk.raofflineproxy"
#define JW_ROP_HEALTH_HOST "127.0.0.1"
#define JW_ROP_HEALTH_PORT 8080
#define JW_ROP_HEALTH_PATH "/leaf/health"
/* Total routing budget when the service is intended to run: at most this
 * long is ever added to a game launch. */
#define JW_ROP_ROUTING_BUDGET_MS 500

/* True only when host:port answers /leaf/health with the fixed
 * service/protocol/ready body before timeout_ms elapses. False on connect
 * failure, timeout, or any unexpected body. */
bool jw_raofflineproxy_health_ready(const char *host, uint16_t port,
                                    int timeout_ms);

#endif
