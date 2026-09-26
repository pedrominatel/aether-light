#include "web_interface.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "sdkconfig.h"
#include "ultraled_manager.h"

#define MAX_FORM_BODY_SIZE 2048

static const char *TAG = "web_interface";

static httpd_handle_t s_http_server;
static TaskHandle_t s_monitor_task;

static void schedule_restart(void);

extern const unsigned char logo_jpg_start[] asm("_binary_logo_jpg_start");
extern const unsigned char logo_jpg_end[] asm("_binary_logo_jpg_end");

typedef struct {
    int value;
    const char *name;
} select_option_t;

static const select_option_t s_model_options[] = {
    { ULTRALED_MODEL_WS2812, "WS2812 / WS2812B" },
    { ULTRALED_MODEL_SK6812_RGB, "SK6812 RGB" },
    { ULTRALED_MODEL_APA106, "APA106" },
    { ULTRALED_MODEL_SM16703, "SM16703" },
};

static const select_option_t s_order_options[] = {
    { ULTRALED_COLOR_ORDER_MODEL_DEFAULT, "Model default" },
    { ULTRALED_COLOR_ORDER_RGB, "RGB" },
    { ULTRALED_COLOR_ORDER_RBG, "RBG" },
    { ULTRALED_COLOR_ORDER_GRB, "GRB" },
    { ULTRALED_COLOR_ORDER_GBR, "GBR" },
    { ULTRALED_COLOR_ORDER_BRG, "BRG" },
    { ULTRALED_COLOR_ORDER_BGR, "BGR" },
};

static esp_err_t send_chunkf(httpd_req_t *req, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static const char *network_state_to_string(network_manager_state_t state)
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

static const char *ultraled_state_to_string(ultraled_manager_state_t state)
{
    switch (state) {
    case ULTRALED_MANAGER_DISABLED:
        return "Disabled";
    case ULTRALED_MANAGER_ACTIVE:
        return "Active";
    case ULTRALED_MANAGER_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

static const char *model_to_string(ultraled_model_t model)
{
    for (size_t i = 0; i < sizeof(s_model_options) / sizeof(s_model_options[0]); ++i) {
        if (s_model_options[i].value == model) {
            return s_model_options[i].name;
        }
    }
    return "Unknown";
}

static bool network_is_ready(void)
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

static void get_active_ip(char ip[16])
{
    strcpy(ip, "-");
    esp_netif_t *netif = get_active_netif();
    if (netif == NULL) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip, 16, IPSTR, IP2STR(&ip_info.ip));
    }
}

static void html_escape_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_len; ++i) {
        const char *replacement = NULL;
        switch (src[i]) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = "&gt;";
            break;
        case '"':
            replacement = "&quot;";
            break;
        case '\'':
            replacement = "&#39;";
            break;
        default:
            dst[out++] = src[i];
            continue;
        }

        size_t replacement_len = strlen(replacement);
        if (out + replacement_len >= dst_len) {
            break;
        }
        memcpy(dst + out, replacement, replacement_len);
        out += replacement_len;
    }
    dst[out] = '\0';
}

static esp_err_t send_chunkf(httpd_req_t *req, const char *format, ...)
{
    char stack_buffer[768];
    va_list args;
    va_start(args, format);
    int required = vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);
    if (required < 0) {
        return ESP_FAIL;
    }

    if ((size_t)required < sizeof(stack_buffer)) {
        return httpd_resp_send_chunk(req, stack_buffer, required);
    }

    char *buffer = malloc((size_t)required + 1);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    va_start(args, format);
    vsnprintf(buffer, (size_t)required + 1, format, args);
    va_end(args);
    esp_err_t err = httpd_resp_send_chunk(req, buffer, required);
    free(buffer);
    return err;
}

static esp_err_t begin_page(httpd_req_t *req, const char *title, const char *active_path)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    return send_chunkf(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' type='image/jpeg' href='/logo.jpg'>"
        "<title>%s · Aether Light</title><style>"
        "*{box-sizing:border-box}html{background:#000}body{font-family:system-ui,sans-serif;max-width:1050px;margin:0 auto;padding:0 16px 40px;line-height:1.5;background:#000;color:#f5f5f5}"
        "header{padding:20px 0 12px}.brand{display:inline-block;text-decoration:none;margin-bottom:16px}.brand img{display:block;width:150px;height:150px;object-fit:cover;border-radius:18px}"
        "nav{display:flex;gap:8px;flex-wrap:wrap}nav a{color:#d4d4d4;text-decoration:none;padding:8px 12px;border:1px solid #2b2b2b;border-radius:10px;background:#090909}nav a:hover{border-color:#666;color:#fff}nav a.active{background:#fff;color:#000;border-color:#fff;font-weight:700}"
        ".card{background:#090909;border:1px solid #262626;border-radius:16px;padding:20px;margin:16px 0;box-shadow:0 16px 40px rgba(255,255,255,.025)}"
        "h2,h3{margin:0 0 12px}label{display:block;margin:12px 0 6px}input,select{width:100%%;padding:10px 12px;border-radius:10px;border:1px solid #363636;background:#000;color:#f5f5f5}input:focus,select:focus{outline:2px solid #737373;outline-offset:1px}"
        "input[type=checkbox]{width:auto;margin-right:8px}button{margin-top:16px;padding:10px 16px;border:1px solid #fff;border-radius:10px;background:#fff;color:#000;font-weight:700;cursor:pointer}.danger{background:#dc2626;border-color:#dc2626;color:white}"
        ".muted{color:#a3a3a3}.warning{color:#fbbf24;font-size:.9rem;margin-top:10px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}"
        ".kv,.channel{background:#000;border:1px solid #262626;border-radius:12px;padding:12px}.channel{margin-top:12px}.button-link{display:inline-block;margin:12px 0;color:#fff;text-decoration:none;padding:9px 14px;border:1px solid #525252;border-radius:10px;background:#171717}.button-link:hover{border-color:#fff}.network-list{display:grid;gap:8px;margin:12px 0 20px}.network-option{display:flex;align-items:center;justify-content:space-between;gap:12px;width:100%%;margin:0;padding:10px 12px;text-align:left;background:#000;color:#fff;border:1px solid #292929}.network-option:hover{border-color:#737373}.network-meta{color:#a3a3a3;font-size:.85rem;white-space:nowrap}.hidden{display:none}@media(max-width:520px){.brand img{width:112px;height:112px}}</style></head><body>"
        "<header><a class='brand' href='/status' aria-label='Aether Light status'><img src='/logo.jpg' alt='Aether Light'></a><nav>"
        "<a href='/status' class='%s'>Status</a>"
        "<a href='/network' class='%s'>Network</a>"
        "<a href='/led-channels' class='%s'>LED Channels</a>"
        "<a href='/configuration' class='%s'>Configuration</a>"
        "<a href='/reboot' class='%s'>Reboot</a>"
        "</nav></header><main><h2>%s</h2>",
        title,
        strcmp(active_path, "/status") == 0 ? "active" : "",
        strcmp(active_path, "/network") == 0 ? "active" : "",
        strcmp(active_path, "/led-channels") == 0 ? "active" : "",
        strcmp(active_path, "/configuration") == 0 ? "active" : "",
        strcmp(active_path, "/reboot") == 0 ? "active" : "",
        title);
}

static esp_err_t end_page(httpd_req_t *req)
{
    esp_err_t err = httpd_resp_send_chunk(req, "</main></body></html>", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t redirect_to(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool url_decode_component(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t output = 0;
    for (size_t input = 0; input < src_len; ++input) {
        if (output + 1 >= dst_len) {
            return false;
        }

        if (src[input] == '+') {
            dst[output++] = ' ';
        } else if (src[input] == '%') {
            if (input + 2 >= src_len) {
                return false;
            }
            int high = hex_value(src[input + 1]);
            int low = hex_value(src[input + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            dst[output++] = (char)((high << 4) | low);
            input += 2;
        } else {
            dst[output++] = src[input];
        }
    }
    dst[output] = '\0';
    return true;
}

static bool form_get_value(const char *body, const char *key, char *value, size_t value_len)
{
    size_t key_len = strlen(key);
    const char *pair = body;

    while (*pair != '\0') {
        const char *pair_end = strchr(pair, '&');
        if (pair_end == NULL) {
            pair_end = pair + strlen(pair);
        }

        const char *equals = memchr(pair, '=', (size_t)(pair_end - pair));
        if (equals != NULL && (size_t)(equals - pair) == key_len && memcmp(pair, key, key_len) == 0) {
            return url_decode_component(value, value_len, equals + 1, (size_t)(pair_end - equals - 1));
        }

        if (*pair_end == '\0') {
            break;
        }
        pair = pair_end + 1;
    }

    return false;
}

static bool form_get_long(const char *body, const char *key, long *value)
{
    char text[24];
    if (!form_get_value(body, key, text, sizeof(text)) || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static esp_err_t receive_form_body(httpd_req_t *req, char **body_out)
{
    int total = req->content_len;
    if (total <= 0 || total > MAX_FORM_BODY_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = calloc((size_t)total + 1, 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total) {
        int result = httpd_req_recv(req, body + received, total - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive body");
            return ESP_FAIL;
        }
        received += result;
    }

    body[received] = '\0';
    *body_out = body;
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return redirect_to(req, "/status");
}

static esp_err_t logo_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char *)logo_jpg_start, logo_jpg_end - logo_jpg_start);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16];
    get_active_ip(ip);

    ultraled_manager_config_t led_config;
    ultraled_manager_get_config(&led_config);
    ultraled_manager_state_t led_state = ultraled_manager_get_state();
    esp_err_t led_error = ultraled_manager_get_last_error();

    esp_err_t err = begin_page(req, "Status", "/status");
    if (err == ESP_OK) {
        err = send_chunkf(req,
            "<div class='card'><div class='grid'>"
            "<div class='kv'><div class='muted'>Network</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>IP address</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>UltraLED</div><div>%s%s%s</div></div>"
            "<div class='kv'><div class='muted'>Configured model</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>Configured channels</div><div>%u</div></div>"
            "</div></div>",
            network_state_to_string(network_manager_get_state()), ip, ultraled_state_to_string(led_state),
            led_state == ULTRALED_MANAGER_ERROR ? ": " : "",
            led_state == ULTRALED_MANAGER_ERROR ? esp_err_to_name(led_error) : "",
            model_to_string(led_config.led_model), led_config.channel_count);
    }
    return err == ESP_OK ? end_page(req) : err;
}

static esp_err_t send_network_page(httpd_req_t *req, const network_manager_wifi_ap_t *networks,
                                   size_t network_count, esp_err_t scan_error)
{
    char ssid[33] = {0};
    char password[65] = {0};
    if (network_manager_get_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ssid[0] = '\0';
        password[0] = '\0';
    }

    char escaped_ssid[192];
    char escaped_password[384];
    html_escape_copy(escaped_ssid, sizeof(escaped_ssid), ssid);
    html_escape_copy(escaped_password, sizeof(escaped_password), password);

    network_manager_ipv4_config_t ipv4_config;
    if (network_manager_get_ipv4_config(&ipv4_config) != ESP_OK) {
        memset(&ipv4_config, 0, sizeof(ipv4_config));
    }

    char escaped_ip[64];
    char escaped_gateway[64];
    char escaped_netmask[64];
    char escaped_dns[64];
    html_escape_copy(escaped_ip, sizeof(escaped_ip), ipv4_config.ip);
    html_escape_copy(escaped_gateway, sizeof(escaped_gateway), ipv4_config.gateway);
    html_escape_copy(escaped_netmask, sizeof(escaped_netmask), ipv4_config.netmask);
    html_escape_copy(escaped_dns, sizeof(escaped_dns), ipv4_config.dns);

    esp_err_t err = begin_page(req, "Network", "/network");
    if (err == ESP_OK) {
        err = send_chunkf(req,
            "<div class='card'><h3>WiFi fallback</h3>"
            "<p class='muted'>Ethernet is preferred. These credentials are used when Ethernet is unavailable.</p>"
            "<form method='post' action='/network'>"
            "<label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' maxlength='32' value='%s' required>"
            "<label for='password'>WiFi password</label><input id='password' name='password' type='password' maxlength='64' value='%s'>"
            "<a class='button-link' href='/wifi-scan'>Scan WiFi networks</a>",
            escaped_ssid, escaped_password);
    }

    if (err == ESP_OK && scan_error != ESP_OK) {
        err = send_chunkf(req, "<p class='warning'>WiFi scan failed: %s. Try again when the WiFi interface is idle.</p>",
                          esp_err_to_name(scan_error));
    } else if (err == ESP_OK && networks != NULL) {
        if (network_count == 0) {
            err = send_chunkf(req, "<p class='muted'>No WiFi networks were found.</p>");
        } else {
            err = send_chunkf(req, "<h3>Available networks</h3><div class='network-list'>");
            for (size_t index = 0; err == ESP_OK && index < network_count; ++index) {
                char escaped_network_ssid[192];
                html_escape_copy(escaped_network_ssid, sizeof(escaped_network_ssid), networks[index].ssid);
                err = send_chunkf(req,
                    "<button class='network-option' type='button' data-ssid='%s'>"
                    "<span>%s</span><span class='network-meta'>%d dBm · Ch %u · %s</span></button>",
                    escaped_network_ssid, escaped_network_ssid, networks[index].rssi,
                    (unsigned)networks[index].channel, networks[index].secured ? "Secured" : "Open");
            }
            if (err == ESP_OK) {
                err = send_chunkf(req, "</div>");
            }
        }
    }

    if (err == ESP_OK) {
        err = send_chunkf(req,
            "<h3 style='margin-top:24px'>IPv4 settings</h3>"
            "<label><input id='use_static_ip' type='checkbox' name='use_static_ip' value='1' %s>Use a fixed IP address</label>"
            "<p class='muted'>When disabled, the active Ethernet or WiFi interface obtains its address using DHCP.</p>"
            "<div id='static-ip-fields' class='grid'>"
            "<div><label for='ip'>IP address</label><input id='ip' name='ip' inputmode='decimal' maxlength='15' value='%s' placeholder='192.168.1.50'></div>"
            "<div><label for='gateway'>Gateway</label><input id='gateway' name='gateway' inputmode='decimal' maxlength='15' value='%s' placeholder='192.168.1.1'></div>"
            "<div><label for='netmask'>Subnet mask</label><input id='netmask' name='netmask' inputmode='decimal' maxlength='15' value='%s' placeholder='255.255.255.0'></div>"
            "<div><label for='dns'>DNS server</label><input id='dns' name='dns' inputmode='decimal' maxlength='15' value='%s' placeholder='1.1.1.1'></div>"
            "</div><p class='warning'>The fixed address applies after restart. Ensure it is unused and belongs to the same subnet as the gateway.</p>"
            "<button type='submit'>Save Network Settings and Restart</button></form></div>"
            "<script>(()=>{const toggle=document.getElementById('use_static_ip');const fields=document.getElementById('static-ip-fields');"
            "const sync=()=>{const enabled=toggle.checked;fields.classList.toggle('hidden',!enabled);fields.querySelectorAll('input').forEach(input=>{input.disabled=!enabled;input.required=enabled;});};"
            "toggle.addEventListener('change',sync);sync();document.querySelectorAll('[data-ssid]').forEach(button=>button.addEventListener('click',()=>{document.getElementById('ssid').value=button.dataset.ssid;document.getElementById('password').focus();}));})();</script>",
            ipv4_config.use_static_ip ? "checked" : "",
            escaped_ip, escaped_gateway, escaped_netmask, escaped_dns);
    }
    return err == ESP_OK ? end_page(req) : err;
}

static esp_err_t network_get_handler(httpd_req_t *req)
{
    return send_network_page(req, NULL, 0, ESP_OK);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    network_manager_wifi_ap_t networks[NETWORK_MANAGER_MAX_SCAN_RESULTS];
    size_t network_count = 0;
    esp_err_t scan_error = network_manager_scan_wifi(networks, NETWORK_MANAGER_MAX_SCAN_RESULTS, &network_count);
    return send_network_page(req, scan_error == ESP_OK ? networks : NULL, network_count, scan_error);
}

static esp_err_t invalid_network_form(httpd_req_t *req, char *body, const char *message)
{
    free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t network_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t err = receive_form_body(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    char ssid[33];
    char password[65];
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) ||
        !form_get_value(body, "password", password, sizeof(password)) || ssid[0] == '\0') {
        return invalid_network_form(req, body, "SSID or password is invalid");
    }

    network_manager_ipv4_config_t ipv4_config;
    if (network_manager_get_ipv4_config(&ipv4_config) != ESP_OK) {
        memset(&ipv4_config, 0, sizeof(ipv4_config));
    }
    char enabled[2];
    ipv4_config.use_static_ip = form_get_value(body, "use_static_ip", enabled, sizeof(enabled));
    if (ipv4_config.use_static_ip &&
        (!form_get_value(body, "ip", ipv4_config.ip, sizeof(ipv4_config.ip)) ||
         !form_get_value(body, "gateway", ipv4_config.gateway, sizeof(ipv4_config.gateway)) ||
         !form_get_value(body, "netmask", ipv4_config.netmask, sizeof(ipv4_config.netmask)) ||
         !form_get_value(body, "dns", ipv4_config.dns, sizeof(ipv4_config.dns)))) {
        return invalid_network_form(req, body, "All fixed IPv4 fields are required");
    }

    err = network_manager_set_ipv4_config(&ipv4_config);
    if (err == ESP_ERR_INVALID_ARG) {
        return invalid_network_form(req, body,
                                    "Invalid fixed IPv4 settings: check the address, gateway, subnet mask, and DNS server");
    }
    if (err != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save IPv4 settings");
        return err;
    }

    err = network_manager_set_wifi_credentials(ssid, password);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi credentials");
        return err;
    }

    err = begin_page(req, "Restarting", "/network");
    if (err == ESP_OK) {
        err = send_chunkf(req, "<div class='card'><h3>Network settings saved</h3>"
                               "<p>The device is restarting and will apply the configuration from NVS.</p></div>");
    }
    if (err == ESP_OK) {
        err = end_page(req);
    }
    if (err == ESP_OK) {
        schedule_restart();
    }
    return err;
}

static esp_err_t led_channels_get_handler(httpd_req_t *req)
{
    ultraled_manager_config_t config;
    ultraled_manager_get_config(&config);

    esp_err_t err = begin_page(req, "LED Channels", "/led-channels");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunkf(req,
        "<div class='card'><p class='muted'>Hardware settings are stored in NVS and applied after restart.</p>"
        "<form method='post' action='/led-channels' id='ultraled-form'>"
        "<label><input type='checkbox' name='enabled' value='1' %s>Enable UltraLED</label>"
        "<div class='grid'><div><label for='led_model'>LED model</label><select id='led_model' name='led_model'>",
        config.enabled ? "checked" : "");
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(s_model_options) / sizeof(s_model_options[0]); ++i) {
        err = send_chunkf(req, "<option value='%d' %s>%s</option>", s_model_options[i].value,
                          config.led_model == s_model_options[i].value ? "selected" : "", s_model_options[i].name);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = send_chunkf(req, "</select></div><div><label for='channel_count'>Active channels</label>"
                           "<select id='channel_count' name='channel_count'>");
    if (err != ESP_OK) {
        return err;
    }
    for (int count = 1; count <= ULTRALED_MAX_CHANNELS; ++count) {
        err = send_chunkf(req, "<option value='%d' %s>%d</option>", count,
                          config.channel_count == count ? "selected" : "", count);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = send_chunkf(req, "</select></div></div>");
    if (err != ESP_OK) {
        return err;
    }

    for (size_t channel = 0; channel < ULTRALED_MAX_CHANNELS; ++channel) {
        const ultraled_manager_channel_config_t *channel_config = &config.channels[channel];
        err = send_chunkf(req,
            "<div class='channel' data-channel='%u'><h3>Channel %u</h3><div class='grid'>"
            "<div><label for='ch%u_gpio'>Data GPIO</label><input id='ch%u_gpio' name='ch%u_gpio' type='number' value='%d' required></div>"
            "<div><label for='ch%u_count'>Pixels</label><input id='ch%u_count' name='ch%u_count' type='number' min='1' max='%d' value='%u' required></div>"
            "<div><label for='ch%u_order'>Color order</label><select id='ch%u_order' name='ch%u_order'>",
            (unsigned)channel, (unsigned)channel,
            (unsigned)channel, (unsigned)channel, (unsigned)channel, channel_config->gpio_num,
            (unsigned)channel, (unsigned)channel, (unsigned)channel,
            ULTRALED_MANAGER_MAX_PIXELS_PER_CHANNEL, channel_config->led_count,
            (unsigned)channel, (unsigned)channel, (unsigned)channel);
        if (err != ESP_OK) {
            return err;
        }

        for (size_t option = 0; option < sizeof(s_order_options) / sizeof(s_order_options[0]); ++option) {
            err = send_chunkf(req, "<option value='%d' %s>%s</option>", s_order_options[option].value,
                              channel_config->color_order == s_order_options[option].value ? "selected" : "",
                              s_order_options[option].name);
            if (err != ESP_OK) {
                return err;
            }
        }

        err = send_chunkf(req,
            "</select></div><div><label for='ch%u_brightness'>Brightness (0–100%%)</label>"
            "<input id='ch%u_brightness' name='ch%u_brightness' type='number' min='0' max='100' value='%u' required></div>"
            "</div><p class='warning'>Brightness must be limited according to the current available to this channel. "
            "At 100%%, the strip may draw its maximum current; 0%% turns the channel off. Actual current depends on the number of LEDs and their colors.</p></div>",
            (unsigned)channel, (unsigned)channel, (unsigned)channel, channel_config->brightness_percent);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = send_chunkf(req,
        "<button type='submit'>Save LED Settings and Restart</button></form></div>"
        "<script>(()=>{const count=document.getElementById('channel_count');const sync=()=>{const n=Number(count.value);"
        "document.querySelectorAll('[data-channel]').forEach((row,i)=>{const active=i<n;row.classList.toggle('hidden',!active);"
        "row.querySelectorAll('input,select').forEach(field=>field.disabled=!active);});};count.addEventListener('change',sync);sync();})();</script>");
    return err == ESP_OK ? end_page(req) : err;
}

static esp_err_t invalid_led_form(httpd_req_t *req, char *body, const char *message)
{
    free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
    return ESP_ERR_INVALID_ARG;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void schedule_restart(void)
{
    if (xTaskCreate(restart_task, "web_restart", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Restart task could not be created");
    }
}

static esp_err_t led_channels_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t err = receive_form_body(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    ultraled_manager_config_t config;
    ultraled_manager_get_config(&config);
    char enabled[2];
    config.enabled = form_get_value(body, "enabled", enabled, sizeof(enabled));

    long value;
    if (!form_get_long(body, "led_model", &value) || value < 0 || value >= ULTRALED_MODEL_MAX) {
        return invalid_led_form(req, body, "Invalid LED model");
    }
    config.led_model = (ultraled_model_t)value;

    if (!form_get_long(body, "channel_count", &value) || value < 1 || value > ULTRALED_MAX_CHANNELS) {
        return invalid_led_form(req, body, "Channel count must be between 1 and 8");
    }
    config.channel_count = (uint8_t)value;

    for (size_t channel = 0; channel < config.channel_count; ++channel) {
        char key[24];

        snprintf(key, sizeof(key), "ch%u_gpio", (unsigned)channel);
        if (!form_get_long(body, key, &value) || value < INT_MIN || value > INT_MAX) {
            return invalid_led_form(req, body, "Each active channel requires a valid GPIO");
        }
        config.channels[channel].gpio_num = (int)value;

        snprintf(key, sizeof(key), "ch%u_count", (unsigned)channel);
        if (!form_get_long(body, key, &value) || value < 1 || value > ULTRALED_MANAGER_MAX_PIXELS_PER_CHANNEL) {
            return invalid_led_form(req, body, "Pixels per channel must be between 1 and 960");
        }
        config.channels[channel].led_count = (uint16_t)value;

        snprintf(key, sizeof(key), "ch%u_order", (unsigned)channel);
        if (!form_get_long(body, key, &value) || value < ULTRALED_COLOR_ORDER_MODEL_DEFAULT ||
            value >= ULTRALED_COLOR_ORDER_MAX) {
            return invalid_led_form(req, body, "Invalid color order");
        }
        config.channels[channel].color_order = (ultraled_color_order_t)value;

        snprintf(key, sizeof(key), "ch%u_brightness", (unsigned)channel);
        if (!form_get_long(body, key, &value) || value < 0 || value > 100) {
            return invalid_led_form(req, body, "Brightness must be between 0 and 100 percent");
        }
        config.channels[channel].brightness_percent = (uint8_t)value;
    }

    free(body);
    err = ultraled_manager_validate_config(&config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid UltraLED configuration: GPIOs must be unique, output-capable, and not used by Ethernet");
        return err;
    }

    err = ultraled_manager_save_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save UltraLED configuration: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save UltraLED configuration");
        return err;
    }

    err = begin_page(req, "Restarting", "/led-channels");
    if (err == ESP_OK) {
        err = send_chunkf(req, "<div class='card'><h3>LED settings saved</h3>"
                               "<p>The device is restarting and will apply the configuration from NVS.</p></div>");
    }
    if (err == ESP_OK) {
        err = end_page(req);
    }
    if (err == ESP_OK) {
        schedule_restart();
    }
    return err;
}

static esp_err_t configuration_get_handler(httpd_req_t *req)
{
    ultraled_manager_config_t config;
    ultraled_manager_get_config(&config);
    network_manager_ipv4_config_t ipv4_config;
    if (network_manager_get_ipv4_config(&ipv4_config) != ESP_OK) {
        memset(&ipv4_config, 0, sizeof(ipv4_config));
    }

    esp_err_t err = begin_page(req, "Configuration", "/configuration");
    if (err == ESP_OK) {
        err = send_chunkf(req,
            "<div class='card'><h3>Runtime configuration</h3><div class='grid'>"
            "<div class='kv'><div class='muted'>HTTP port</div><div>%d</div></div>"
            "<div class='kv'><div class='muted'>IPv4 mode</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>Configured IP address</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>Gateway</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>Subnet mask</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>DNS server</div><div>%s</div></div>"
            "<div class='kv'><div class='muted'>LED configuration storage</div><div>NVS</div></div>"
            "<div class='kv'><div class='muted'>Maximum channels</div><div>%d</div></div>"
            "<div class='kv'><div class='muted'>Maximum pixels per channel</div><div>%d</div></div>"
            "<div class='kv'><div class='muted'>LED changes</div><div>Applied after restart</div></div>"
            "<div class='kv'><div class='muted'>UltraLED enabled</div><div>%s</div></div>"
            "</div></div>"
            "<div class='card'><p class='muted'>Editable network and LED hardware settings are available from their dedicated menu pages.</p></div>",
            CONFIG_WEB_INTERFACE_HTTP_PORT, ipv4_config.use_static_ip ? "Fixed IP" : "DHCP",
            ipv4_config.use_static_ip ? ipv4_config.ip : "Assigned automatically",
            ipv4_config.use_static_ip ? ipv4_config.gateway : "Assigned automatically",
            ipv4_config.use_static_ip ? ipv4_config.netmask : "Assigned automatically",
            ipv4_config.use_static_ip ? ipv4_config.dns : "Assigned automatically",
            ULTRALED_MAX_CHANNELS,
            ULTRALED_MANAGER_MAX_PIXELS_PER_CHANNEL, config.enabled ? "Yes" : "No");
    }
    return err == ESP_OK ? end_page(req) : err;
}

static esp_err_t reboot_get_handler(httpd_req_t *req)
{
    esp_err_t err = begin_page(req, "Reboot", "/reboot");
    if (err == ESP_OK) {
        err = send_chunkf(req,
            "<div class='card'><h3>Restart device</h3>"
            "<p>The network connection and web interface will be temporarily unavailable.</p>"
            "<form method='post' action='/reboot'><button class='danger' type='submit'>Reboot Aether Light</button></form></div>");
    }
    return err == ESP_OK ? end_page(req) : err;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    esp_err_t err = begin_page(req, "Restarting", "/reboot");
    if (err == ESP_OK) {
        err = send_chunkf(req, "<div class='card'><h3>Restart requested</h3>"
                               "<p>The device is restarting now.</p></div>");
    }
    if (err == ESP_OK) {
        err = end_page(req);
    }
    if (err == ESP_OK) {
        schedule_restart();
    }
    return err;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_INTERFACE_HTTP_PORT;
    config.max_uri_handlers = 12;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/logo.jpg", .method = HTTP_GET, .handler = logo_get_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = logo_get_handler },
        { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler },
        { .uri = "/network", .method = HTTP_GET, .handler = network_get_handler },
        { .uri = "/network", .method = HTTP_POST, .handler = network_post_handler },
        { .uri = "/wifi-scan", .method = HTTP_GET, .handler = wifi_scan_get_handler },
        { .uri = "/led-channels", .method = HTTP_GET, .handler = led_channels_get_handler },
        { .uri = "/led-channels", .method = HTTP_POST, .handler = led_channels_post_handler },
        { .uri = "/configuration", .method = HTTP_GET, .handler = configuration_get_handler },
        { .uri = "/reboot", .method = HTTP_GET, .handler = reboot_get_handler },
        { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(s_http_server, &routes[i]);
        if (err != ESP_OK) {
            httpd_stop(s_http_server);
            s_http_server = NULL;
            return err;
        }
    }

    char ip[16];
    get_active_ip(ip);
    ESP_LOGI(TAG, "Web interface available at http://%s:%u/", ip, config.server_port);
    return ESP_OK;
}

static void stop_http_server(void)
{
    if (s_http_server == NULL) {
        return;
    }

    httpd_stop(s_http_server);
    s_http_server = NULL;
    ESP_LOGI(TAG, "Web interface stopped because no valid network is up");
}

static void monitor_task(void *arg)
{
    (void)arg;
    bool was_ready = false;

    while (true) {
        bool ready = network_is_ready();
        if (ready && !was_ready) {
            if (start_http_server() == ESP_OK) {
                was_ready = true;
            }
        } else if (!ready && was_ready) {
            stop_http_server();
            was_ready = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void web_interface_start_when_network_ready(void)
{
    if (s_monitor_task != NULL) {
        return;
    }

    if (xTaskCreate(monitor_task, "web_interface", 6144, NULL, 5, &s_monitor_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create web interface monitor task");
    }
}
