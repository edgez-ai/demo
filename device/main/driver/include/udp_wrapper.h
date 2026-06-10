/**
 * @file udp_wrapper.h
 * @brief Platform-agnostic UDP socket wrapper interface
 * 
 * This header provides a common interface for UDP socket operations
 * across different platforms and connection types.
 */

#ifndef DRIVER_UDP_WRAPPER_H
#define DRIVER_UDP_WRAPPER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/socket.h>
#include "lwm2m_client.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque socket type for wrapper
typedef struct udp_socket_wrapper {
    int sock_fd;
    lwm2m_connection_type_t conn_type;
} udp_socket_wrapper_t;

// Create UDP socket based on connection type
udp_socket_wrapper_t *udp_socket_create(const char *port, int ai_family, lwm2m_connection_type_t conn_type);

// Send data
int udp_socket_send(udp_socket_wrapper_t *sock, const void *buf, size_t len, const struct sockaddr *dest_addr, socklen_t addrlen);

// Receive data
int udp_socket_recv(udp_socket_wrapper_t *sock, void *buf, size_t len, struct sockaddr *src_addr, socklen_t *addrlen);

// Close socket
void udp_socket_close(udp_socket_wrapper_t *sock);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_UDP_WRAPPER_H */
