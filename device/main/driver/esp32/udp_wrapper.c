/**
 * @file udp_wrapper_esp32.c
 * @brief ESP32-specific UDP socket wrapper implementation
 * 
 * This implementation is specific to ESP32 series chips.
 */

#include "udp_wrapper.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <esp_netif.h>
#include "esp_log.h"
#if CONFIG_ENABLE_MM_HALOW
#include "mmwlan.h"
#endif

static const char *TAG = "udp_wrapper";

static void log_sockaddr(const char *prefix, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!addr) {
        ESP_LOGI(TAG, "%s: (null)", prefix);
        return;
    }

    if (addr->sa_family == AF_INET && addrlen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        char host[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) != NULL) {
            ESP_LOGI(TAG, "%s: family=%d addr=%s port=%u", prefix, addr->sa_family, host,
                     (unsigned)ntohs(sin->sin_port));
        } else {
            ESP_LOGW(TAG, "%s: family=%d inet_ntop(AF_INET) failed", prefix, addr->sa_family);
        }
        return;
    }

    if (addr->sa_family == AF_INET6 && addrlen >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        char host[INET6_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) != NULL) {
            ESP_LOGI(TAG, "%s: family=%d addr=%s port=%u", prefix, addr->sa_family, host,
                     (unsigned)ntohs(sin6->sin6_port));
        } else {
            ESP_LOGW(TAG, "%s: family=%d inet_ntop(AF_INET6) failed", prefix, addr->sa_family);
        }
        return;
    }

    ESP_LOGW(TAG, "%s: unsupported family=%d addrlen=%u", prefix, addr->sa_family,
             (unsigned)addrlen);
}

udp_socket_wrapper_t *udp_socket_create(const char *port, int ai_family, lwm2m_connection_type_t conn_type) {
    udp_socket_wrapper_t *sock = calloc(1, sizeof(udp_socket_wrapper_t));
    if (!sock) return NULL;
    sock->sock_fd = -1;
    sock->conn_type = conn_type;

    ESP_LOGI(TAG, "Creating UDP socket for connection type: %d", conn_type);
    
    // For now, both WiFi and HaLow use lwIP sockets
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = ai_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        ESP_LOGE(TAG, "getaddrinfo failed");
        free(sock);
        return NULL;
    }
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        ESP_LOGI(TAG, "Bind candidate: family=%d socktype=%d proto=%d", p->ai_family, p->ai_socktype, p->ai_protocol);
        log_sockaddr("Bind address", p->ai_addr, p->ai_addrlen);
        int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s >= 0) {
            ESP_LOGI(TAG, "Socket created, fd=%d, attempting bind...", s);
            if (bind(s, p->ai_addr, p->ai_addrlen) == -1) {
                ESP_LOGE(TAG, "Bind failed: %d %s", errno, strerror(errno));
                close(s);
                continue;
            }
            ESP_LOGI(TAG, "Socket bound successfully");
            struct sockaddr_storage local_addr = {0};
            socklen_t local_len = sizeof(local_addr);
            if (getsockname(s, (struct sockaddr *)&local_addr, &local_len) == 0) {
                log_sockaddr("Socket local endpoint", (const struct sockaddr *)&local_addr, local_len);
            } else {
                ESP_LOGW(TAG, "getsockname failed on fd=%d: %d %s", s, errno, strerror(errno));
            }
            sock->sock_fd = s;
            break;
        } else {
            ESP_LOGE(TAG, "Socket creation failed: %d %s", errno, strerror(errno));
        }
    }
    freeaddrinfo(res);
    if (sock->sock_fd < 0) {
        ESP_LOGE(TAG, "Failed to create and bind socket");
        free(sock);
        return NULL;
    }
    
    // Log local IP addresses to verify routing
    /* Iterate netifs using non-locking API; safe here because we're only logging. */
    esp_netif_t *netif = esp_netif_next_unsafe(NULL);
    while (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "Interface %s: IP=" IPSTR, esp_netif_get_desc(netif), 
                     IP2STR(&ip_info.ip));
        }
        netif = esp_netif_next_unsafe(netif);
    }
    
    return sock;
}

int udp_socket_send(udp_socket_wrapper_t *sock, const void *buf, size_t len, const struct sockaddr *dest_addr, socklen_t addrlen) {
    if (!sock) return -1;
    // Add HaLow-specific send logic if needed
    int sent = sendto(sock->sock_fd, buf, len, 0, dest_addr, addrlen);
    if (sent < 0) {
        int err = errno;
        log_sockaddr("UDP send destination", dest_addr, addrlen);
        ESP_LOGW(TAG, "sendto failed fd=%d req_len=%u errno=%d (%s)",
                 sock->sock_fd, (unsigned)len, err, strerror(err));
    } else {
        ESP_LOGI(TAG, "sendto ok fd=%d sent=%d req_len=%u", sock->sock_fd, sent, (unsigned)len);
    }
    return sent;
}

int udp_socket_recv(udp_socket_wrapper_t *sock, void *buf, size_t len, struct sockaddr *src_addr, socklen_t *addrlen) {
    if (!sock) return -1;
    // Add HaLow-specific recv logic if needed
    int received = recvfrom(sock->sock_fd, buf, len, 0, src_addr, addrlen);
    if (received < 0) {
        int err = errno;
        if (err == EWOULDBLOCK || err == EAGAIN) {
            ESP_LOGD(TAG, "recvfrom would block fd=%d", sock->sock_fd);
        } else {
            ESP_LOGW(TAG, "recvfrom failed fd=%d req_len=%u errno=%d (%s)",
                     sock->sock_fd, (unsigned)len, err, strerror(err));
        }
    } else if (received > 0) {
        if (src_addr && addrlen) {
            log_sockaddr("UDP recv source", src_addr, *addrlen);
        }
        ESP_LOGI(TAG, "recvfrom ok fd=%d received=%d", sock->sock_fd, received);
    }
    return received;
}

void udp_socket_close(udp_socket_wrapper_t *sock) {
    if (!sock) return;
    if (sock->sock_fd >= 0) {
        ESP_LOGI(TAG, "Closing UDP socket fd=%d", sock->sock_fd);
        close(sock->sock_fd);
    }
    free(sock);
}
