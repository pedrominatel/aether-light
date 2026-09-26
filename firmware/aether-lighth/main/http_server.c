#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "http_server.h"

static const char *TAG = "http_server";

static httpd_handle_t s_http_server;
static TaskHandle_t s_monitor_task;

static const char *state_to_string(network_manager_state_t state)
{
    switch (state) {
    case NETWORK_MANAGER_DISCONNECTED:
        return "Disconnected";
    case NETWORK_MANAGER_CONNECTING:
        return "Connecting";
    case NETWORK_MANAGER_CONNECTED_ETH:
        return "Connected via Ethernet";
    case NETWORK_MANAGER_CONNECTED_WIFI:
        return "Connected via WiFi";
    default:
        return "Unknown";
    }
}

static int network_is_ready(void)
{
    network_manager_state_t state = network_manager_get_state();
    return state == NETWORK_MANAGER_CONNECTED_ETH || state == NETWORK_MANAGER_CONNECTED_WIFI;
}

static esp_netif_t *get_active_netif(void)
{
    network_manager_state_t state = network_manager_get_state();
    if (state == NETWORK_MANAGER_CONNECTED_ETH) {
        return esp_netif_get_handle_from_ifkey("ETH_DEF");
    }
    if (state == NETWORK_MANAGER_CONNECTED_WIFI) {
        return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    return NULL;
}

static void html_escape_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_len; i++) {
        switch (src[i]) {
        case '&':
            if (out + 5 >= dst_len) {
                goto done;
            }
            memcpy(dst + out, "&amp;", 5);
            out += 5;
            break;
        case '<':
            if (out + 4 >= dst_len) {
                goto done;
            }
            memcpy(dst + out, "&lt;", 4);
            out += 4;
            break;
        case '>':
            if (out + 4 >= dst_len) {
                goto done;
            }
            memcpy(dst + out, "&gt;", 4);
            out += 4;
            break;
        case '"':
            if (out + 6 >= dst_len) {
                goto done;
            }
            memcpy(dst + out, "&quot;", 6);
            out += 6;
            break;
        default:
            dst[out++] = src[i];
            break;
        }
    }

done:
    dst[out] = '\0';
}

static esp_err_t send_html(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static void url_decode(char *text)
{
    char *read = text;
    char *write = text;

    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            read++;
            continue;
        }

        if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            char hex[3] = { read[1], read[2], '\0' };
            *write++ = (char)strtol(hex, NULL, 16);
            read += 3;
            continue;
        }

        *write++ = *read++;
    }

    *write = '\0';
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    char ip[16] = "-";
    char ssid[33] = {0};
    char password[65] = {0};
    const char *state = state_to_string(network_manager_get_state());

    esp_netif_t *netif = get_active_netif();
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    if (network_manager_get_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ssid[0] = '\0';
        password[0] = '\0';
    }

    char escaped_ssid[96];
    char escaped_password[128];
    html_escape_copy(escaped_ssid, sizeof(escaped_ssid), ssid);
    html_escape_copy(escaped_password, sizeof(escaped_password), password);

    char html[2048];
    snprintf(html, sizeof(html),
             "<!doctype html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Aether Light</title>"
             "<style>body{font-family:system-ui,sans-serif;max-width:900px;margin:40px auto;padding:0 16px;line-height:1.5;background:#0f172a;color:#e2e8f0}"
             ".card{background:#111827;border:1px solid #334155;border-radius:16px;padding:20px;margin:16px 0;box-shadow:0 20px 40px rgba(0,0,0,.2)}"
             "h1,h2{margin:0 0 12px}label{display:block;margin:12px 0 6px}input{width:100%%;padding:10px 12px;border-radius:10px;border:1px solid #475569;background:#0b1220;color:#e2e8f0}button{margin-top:16px;padding:10px 16px;border:0;border-radius:10px;background:#38bdf8;color:#0f172a;font-weight:700;cursor:pointer}"
             ".muted{color:#94a3b8}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.kv{background:#0b1220;border:1px solid #334155;border-radius:12px;padding:12px}</style></head><body>"
             "<h1>Aether Light</h1>"
             "<div class='card'><h2>Status</h2><div class='grid'>"
             "<div class='kv'><div class='muted'>Network state</div><div>%s</div></div>"
             "<div class='kv'><div class='muted'>HTTP IP</div><div>%s</div></div>"
             "</div></div>"
             "<div class='card'><h2>Configuration</h2><form method='post' action='/config'>"
             "<label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' maxlength='32' value='%s'>"
             "<label for='password'>WiFi Password</label><input id='password' name='password' type='password' maxlength='64' value='%s'>"
             "<button type='submit'>Save WiFi Settings</button></form></div>"
             "</body></html>",
             state,
             ip,
             escaped_ssid,
             escaped_password);

    return send_html(req, html);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    char body[256] = {0};
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive body");
            return ESP_FAIL;
        }
        received += ret;
    }

    body[received] = '\0';

    char *ssid = strstr(body, "ssid=");
    char *password = strstr(body, "password=");
    if (ssid == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    ssid += 5;
    char *ssid_end = strchr(ssid, '&');
    if (ssid_end != NULL) {
        *ssid_end = '\0';
    }
    if (password != NULL) {
        password += 9;
        char *password_end = strchr(password, '&');
        if (password_end != NULL) {
            *password_end = '\0';
        }
    }

    url_decode(ssid);
    if (password != NULL) {
        url_decode(password);
    }

    esp_err_t err = network_manager_set_wifi_credentials(ssid, password != NULL ? password : "");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi credentials");
        return err;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };

    static const httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL,
    };

    err = httpd_register_uri_handler(s_http_server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &config_uri);
    }
    if (err != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return err;
    }

    esp_netif_t *netif = get_active_netif();
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "HTTP server available at http://" IPSTR "/", IP2STR(&ip_info.ip));
        }
    }

    return ESP_OK;
}

static void stop_http_server(void)
{
    if (s_http_server == NULL) {
        return;
    }

    httpd_stop(s_http_server);
    s_http_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped because no valid network is up");
}

static void monitor_task(void *arg)
{
    int was_ready = 0;

    while (1) {
        int ready = network_is_ready();
        if (ready && !was_ready) {
            if (start_http_server() == ESP_OK) {
                was_ready = 1;
            }
        } else if (!ready && was_ready) {
            stop_http_server();
            was_ready = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void http_server_start_when_network_ready(void)
{
    if (s_monitor_task != NULL) {
        return;
    }

    xTaskCreate(monitor_task, "http_server", 4096, NULL, 5, &s_monitor_task);
}