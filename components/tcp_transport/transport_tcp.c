// Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <string.h>

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"

#include "esp_transport_utils.h"
#include "esp_transport.h"

static const char *TAG = "TRANS_TCP";

typedef struct {
    int sock;
} transport_tcp_t;

static int resolve_dns(const char *host, struct sockaddr_in *ip) {

    struct hostent *he;
    struct in_addr **addr_list;
    he = gethostbyname(host);
    if (he == NULL) {
        return ESP_FAIL;
    }
    addr_list = (struct in_addr **)he->h_addr_list;
    if (addr_list[0] == NULL) {
        return ESP_FAIL;
    }
    ip->sin_family = AF_INET;
    memcpy(&ip->sin_addr, addr_list[0], sizeof(ip->sin_addr));
    return ESP_OK;
}

static int tcp_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    struct sockaddr_in remote_ip;
    transport_tcp_t *tcp = esp_transport_get_context_data(t);

    bzero(&remote_ip, sizeof(struct sockaddr_in));

    //if stream_host is not ip address, resolve it AF_INET,servername,&serveraddr.sin_addr
    if (inet_pton(AF_INET, host, &remote_ip.sin_addr) != 1) {
        if (resolve_dns(host, &remote_ip) < 0) {
            return -1;
        }
    }

    tcp->sock = socket(PF_INET, SOCK_STREAM, 0);

    if (tcp->sock < 0) {
        ESP_LOGE(TAG, "Error create socket");
        return -1;
    }

    remote_ip.sin_family = AF_INET;
    remote_ip.sin_port = htons(port);

    // Set socket to non-blocking
    int flags;
    if ((flags = fcntl(tcp->sock, F_GETFL, NULL)) < 0 || fcntl(tcp->sock, F_SETFL, flags |= O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "[sock=%d] set nonblocking error: %s", tcp->sock, strerror(errno));
        goto error;
    }
    else {
        ESP_LOGD(TAG, "[sock=%d] Connecting to server. IP: %s, Port: %d",
                tcp->sock, ipaddr_ntoa((const ip_addr_t*)&remote_ip.sin_addr.s_addr), port); 

        if (connect(tcp->sock, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr)) < 0) {
            if (errno == EINPROGRESS) {
                struct timeval tv;
                fd_set fdset;

                esp_transport_utils_ms_to_timeval(timeout_ms, &tv);
                FD_ZERO(&fdset);
                FD_SET(tcp->sock, &fdset);

                int res = select(tcp->sock+1, NULL, &fdset, NULL, &tv);
                if (res < 0) {
                    ESP_LOGE(TAG, "[sock=%d] select() error: %s", tcp->sock, strerror(errno));
                    goto error;
                }
                else if (res == 0) {
                    ESP_LOGE(TAG, "[sock=%d] select() timeout", tcp->sock);
                    goto error;
                }
                else {
                    int sockerr;
                    socklen_t len = (socklen_t)sizeof(int);

                    if (getsockopt(tcp->sock, SOL_SOCKET, SO_ERROR, (void*)(&sockerr), &len) < 0) {
                        ESP_LOGE(TAG, "[sock=%d] getsockopt() error: %s", tcp->sock, strerror(errno));
                        goto error;
                    }
                    else if (sockerr) {
                        ESP_LOGE(TAG, "[sock=%d] delayed connect error: %s", tcp->sock, strerror(sockerr));
                        goto error;
                    }
                }
            }
            else { 
                ESP_LOGE(TAG, "[sock=%d] connect() error: %s", tcp->sock, strerror(errno));
                goto error;
            }
        }
        // Reset socket to blocking
        if ((flags = fcntl(tcp->sock, F_GETFL, NULL)) < 0 || fcntl(tcp->sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
            ESP_LOGE(TAG, "[sock=%d] reset blocking error: %s", tcp->sock, strerror(errno));
            goto error;
        }
    }
    return tcp->sock;
error:
    close(tcp->sock);
    tcp->sock = -1;
    return -1;
}

static int tcp_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    int poll;
    transport_tcp_t *tcp = esp_transport_get_context_data(t);
    if ((poll = esp_transport_poll_write(t, timeout_ms)) <= 0) {
        return poll;
    }
    return write(tcp->sock, buffer, len);
}

static int tcp_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    transport_tcp_t *tcp = esp_transport_get_context_data(t);
    int poll = -1;
    if ((poll = esp_transport_poll_read(t, timeout_ms)) <= 0) {
        return poll;
    }
    int read_len = read(tcp->sock, buffer, len);
    if (read_len == 0) {
        return -1;
    }
    return read_len;
}

static int tcp_poll_read(esp_transport_handle_t t, int timeout_ms)
{
    transport_tcp_t *tcp = esp_transport_get_context_data(t);
    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(tcp->sock, &readset);
    struct timeval timeout;
    esp_transport_utils_ms_to_timeval(timeout_ms, &timeout);
    return select(tcp->sock + 1, &readset, NULL, NULL, &timeout);
}

static int tcp_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    transport_tcp_t *tcp = esp_transport_get_context_data(t);
    fd_set writeset;
    FD_ZERO(&writeset);
    FD_SET(tcp->sock, &writeset);
    struct timeval timeout;
    esp_transport_utils_ms_to_timeval(timeout_ms, &timeout);
    return select(tcp->sock + 1, NULL, &writeset, NULL, &timeout);
}

static int tcp_close(esp_transport_handle_t t)
{
    transport_tcp_t *tcp = esp_transport_get_context_data(t);
    int ret = -1;
    if (tcp->sock >= 0) {
        ret = close(tcp->sock);
        tcp->sock = -1;
    }
    return ret;
}

static esp_err_t tcp_destroy(esp_transport_handle_t t)
{
    transport_tcp_t *tcp = esp_transport_get_context_data(t);
    esp_transport_close(t);
    free(tcp);
    return 0;
}

esp_transport_handle_t esp_transport_tcp_init()
{
    esp_transport_handle_t t = esp_transport_init();
    transport_tcp_t *tcp = calloc(1, sizeof(transport_tcp_t));
    ESP_TRANSPORT_MEM_CHECK(TAG, tcp, return NULL);
    tcp->sock = -1;
    esp_transport_set_func(t, tcp_connect, tcp_read, tcp_write, tcp_close, tcp_poll_read, tcp_poll_write, tcp_destroy);
    esp_transport_set_context_data(t, tcp);

    return t;
}
