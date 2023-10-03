#include "net.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

// Most of this logic is adapted from esp-idf/examples/protocols/http_request/
// Secrets are set by `idf.py menuconfig` and defined in build/config/sdkconfig.h:
// CONFIG_WIFI_SSID
// CONFIG_WIFI_PASSWORD
// CONFIG_PI_HOLE_TOKEN

#define WEB_SERVER "pi.hole"
#define WEB_PORT "80"
#define WEB_PATH "/admin/api.php?disable=60&auth="
#define WIFI_CONN_MAX_RETRY 6
#define NETIF_DESC_STA "example_netif_sta"

static const char* PI_HOLE_RESPONSE = "{\"status\":\"disabled\"}";
static const int PI_HOLE_RESPONSE_LEN = 21;
static const char* REQUEST = "GET " WEB_PATH CONFIG_PI_HOLE_TOKEN " HTTP/1.0\r\n"
    "Host: "WEB_SERVER":"WEB_PORT"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

static const char* TAG  = "net";
static esp_netif_t* s_sta_netif = NULL;
static SemaphoreHandle_t s_semph_get_ip_addrs = NULL;
static SemaphoreHandle_t s_semph_get_ip6_addrs = NULL;
static int s_retry_num = 0;

static const char* example_ipv6_addr_types_to_str[6] = {
    "ESP_IP6_ADDR_IS_UNKNOWN",
    "ESP_IP6_ADDR_IS_GLOBAL",
    "ESP_IP6_ADDR_IS_LINK_LOCAL",
    "ESP_IP6_ADDR_IS_SITE_LOCAL",
    "ESP_IP6_ADDR_IS_UNIQUE_LOCAL",
    "ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6"
};

static void handler_on_wifi_connect(void* esp_netif, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    esp_netif_create_ip6_linklocal(esp_netif);
}

static void handler_on_wifi_disconnect(void* arg, esp_event_base_t event_base,
                                       int32_t event_id, void* event_data) {
    s_retry_num++;
    if (s_retry_num > WIFI_CONN_MAX_RETRY) {
        ESP_LOGE(TAG, "WiFi Connect failed %d times, stop reconnect.", s_retry_num);
        if (s_semph_get_ip_addrs) {
            xSemaphoreGive(s_semph_get_ip_addrs);
        }
        if (s_semph_get_ip6_addrs) {
            xSemaphoreGive(s_semph_get_ip6_addrs);
        }
        return;
    }

    ESP_LOGW(TAG, "Wi-Fi disconnected, trying to reconnect...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return;
    }
    ESP_ERROR_CHECK(err);
}

static bool is_our_netif(const char* prefix, esp_netif_t* netif) {
    return strncmp(prefix, esp_netif_get_desc(netif), strlen(prefix) - 1) == 0;
}

static void handler_on_sta_got_ip(void* arg, esp_event_base_t event_base,
                                          int32_t event_id, void* event_data) {
    s_retry_num = 0;
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    if (!is_our_netif(NETIF_DESC_STA, event->esp_netif)) {
        return;
    }

    ESP_LOGI(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR,
        esp_netif_get_desc(event->esp_netif),
        IP2STR(&event->ip_info.ip));

    if (s_semph_get_ip_addrs) {
        xSemaphoreGive(s_semph_get_ip_addrs);
    }
}

static void handler_on_sta_got_ipv6(void* arg, esp_event_base_t event_base,
                                            int32_t event_id, void* event_data) {
    ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
    if (!is_our_netif(NETIF_DESC_STA, event->esp_netif)) {
        return;
    }

    esp_ip6_addr_type_t ipv6_type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);

    ESP_LOGI(TAG, "Got IPv6 event: Interface \"%s\" address: " IPV6STR ", type: %s",
        esp_netif_get_desc(event->esp_netif),
        IPV62STR(event->ip6_info.ip),
        example_ipv6_addr_types_to_str[ipv6_type]);

    if (ipv6_type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
        if (s_semph_get_ip6_addrs) {
            xSemaphoreGive(s_semph_get_ip6_addrs);
        }
    }
}

esp_err_t connect_to_wifi(void) {
    // wifi_connect.c:135 example_wifi_start()
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    esp_netif_config.if_desc = NETIF_DESC_STA;
    esp_netif_config.route_prio = 128;
    s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);
    esp_wifi_set_default_wifi_sta_handlers();

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());


    // wifi_connect.c:237 example_wifi_connect()
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };


    // wifi_connect.c:167 example_wifi_sta_do_connect()
    s_semph_get_ip_addrs = xSemaphoreCreateBinary();
    if (s_semph_get_ip_addrs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_semph_get_ip6_addrs = xSemaphoreCreateBinary();
    if (s_semph_get_ip6_addrs == NULL) {
        vSemaphoreDelete(s_semph_get_ip_addrs);  // I wish I could use Zig...
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &handler_on_wifi_connect, s_sta_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &handler_on_wifi_disconnect, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &handler_on_sta_got_ipv6, NULL));

    ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed! ret:%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Waiting for IP(s)");
    xSemaphoreTake(s_semph_get_ip_addrs, portMAX_DELAY);
    xSemaphoreTake(s_semph_get_ip6_addrs, portMAX_DELAY);
    if (s_retry_num > WIFI_CONN_MAX_RETRY) {
        ESP_LOGE(TAG, "WiFi connect failed! Too many connection attempts.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t shutdown_wifi(void) {
    // wifi_connect.c:212 example_wifi_sta_do_disconnect()
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &handler_on_wifi_connect));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &handler_on_wifi_disconnect));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6, &handler_on_sta_got_ipv6));

    if (s_semph_get_ip_addrs) {
        vSemaphoreDelete(s_semph_get_ip_addrs);
        s_semph_get_ip_addrs = NULL;
    }

    if (s_semph_get_ip6_addrs) {
        vSemaphoreDelete(s_semph_get_ip6_addrs);
        s_semph_get_ip6_addrs = NULL;
    }

    s_retry_num = 0;

    esp_wifi_disconnect();


    // wifi_connect.c:153 example_wifi_stop()
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        // It's okay to call this function if the WiFi is already shutdown.
        return ESP_OK;
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_wifi_deinit());
    ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif));
    esp_netif_destroy(s_sta_netif);
    s_sta_netif = NULL;

    return ESP_OK;
}

esp_err_t post_request(void) {
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_addr = NULL,
        .ai_canonname = NULL,
        .ai_next = NULL,
    };

    struct addrinfo* res = NULL;

    int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        return ESP_FAIL;
    }


    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to allocate socket.");
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Allocated socket.");

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Socket connect failed errno=%d", errno);
        close(sock);
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connected");
    freeaddrinfo(res);

    if (write(sock, REQUEST, strlen(REQUEST)) < 0) {
        ESP_LOGE(TAG, "Socket send failed");
        close(sock);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Socket send success");

    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout)) < 0) {
        ESP_LOGE(TAG, "Failed to set socket receiving timeout");
        close(sock);
        return ESP_FAIL;
    }

    // Once I got -1 bytes_read from read(), so I'm just guessing that waiting a little will help...
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Read the HTTP response from the Pi-Hole.
    // The response is typically ~600 bytes, and ends with a json response.
    // When the request is successful, the response ends like this: {"status":"disabled"}
    // If the API token isn't correct, the response ends like this: []
    // We need to parse this response data as the return code is 200 in either case.

    int bytes_read = 0;
    int total_bytes_read = 0;
    char recv_buf[1024];
    memset(recv_buf, 0x00, sizeof(recv_buf));
    do {
        bytes_read = read(sock, &(recv_buf[total_bytes_read]), sizeof(recv_buf) - total_bytes_read - 1);

        ESP_LOGI(TAG, "read(sock, &(recv_buf[%d]), %d) = %d",
            total_bytes_read,
            sizeof(recv_buf) - total_bytes_read - 1,
            bytes_read);

        total_bytes_read += bytes_read;
    } while (bytes_read > 0);


    ESP_LOGI(TAG, "Pi-Hole server response:\n%s", recv_buf);
    close(sock);

    if (total_bytes_read < PI_HOLE_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Read too few bytes to have succeeded.");
        return ESP_FAIL;
    }

    // God, I wish C was Zig... or that I had a better string library...
    for (int i = 0; i < PI_HOLE_RESPONSE_LEN; i++) {
        char recvd = recv_buf[total_bytes_read - PI_HOLE_RESPONSE_LEN + i];
        char expected = PI_HOLE_RESPONSE[i];
        if (recvd != expected) {
            ESP_LOGE(TAG, "Mismatch in response i = %d, '%c' != '%c'", i, recvd, expected);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t net_disable_add_blocking(void) {
    ESP_LOGI(TAG, "Disabling Pi-Hole add blocking.");

    esp_err_t err = ESP_OK;

    // http_request_example_main.c:125
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    err = connect_to_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to the WiFi network!");
        return err;
    }

    err = post_request();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post the request to pi.hole!");
        return err;
    }

    err = shutdown_wifi();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disconnect from the WiFi network!");
        return err;
    }

    return err;
}
