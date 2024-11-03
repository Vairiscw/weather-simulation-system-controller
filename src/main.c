#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "driver/gpio.h"

#define FAN_GPIO_PIN  2 // Пин для управления реле
#define HUMID_GPIO_PIN  4 // Пин для управления реле
#define FRESH_GPIO_PIN  16 // Пин для управления реле

static const char *TAG = "ESP32_HTTP_CLIENT";

#define WIFI_SSID      "ssid"
#define WIFI_PASS      "pass"
#define MAX_RETRY      5
static char* SERVER_URL  =   "server_url";

#define FULL_URL_SIZE 128

static char response_buffer[1024]; // Буфер для хранения ответа
static bool response_received = false;

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &event_handler,
                                        NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &event_handler,
                                        NULL,
                                        &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    vEventGroupDelete(s_wifi_event_group);
}

static void fan_control(bool state) {
    gpio_set_level(FAN_GPIO_PIN, state ? 1 : 0);
}

static void humid_control(bool state) {
    gpio_set_level(HUMID_GPIO_PIN, state ? 1 : 0);
}

static void fresh_control(bool state) {
    gpio_set_level(FRESH_GPIO_PIN, state ? 1 : 0);
}


esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data_len < sizeof(response_buffer) - 1) {
            strncpy(response_buffer, (char*)evt->data, evt->data_len);
            response_buffer[evt->data_len] = 0; // Завершаем строку нулевым символом
            response_received = true;
        }
    }
    return ESP_OK;
}

static void get_fan_status_from_server(void *pvParameters) {
    char url_buffer[FULL_URL_SIZE];  // Создаем отдельный буфер для полного URL

    // Формируем полный URL для запроса состояния реле
    snprintf(url_buffer, sizeof(url_buffer), "%s/devices/fan", SERVER_URL);
    esp_http_client_config_t config = {
        .url = url_buffer,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    while (1) {
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK && response_received) {
            ESP_LOGI(TAG, "Fan: Response: %s", response_buffer);
            bool relay_state = strcmp(response_buffer, "1") == 0;
            fan_control(relay_state);
        } else {
            ESP_LOGE(TAG, "Fan: HTTP GET request failed: %s", esp_err_to_name(err));
        }
        
        response_received = false;
        memset(response_buffer, 0, sizeof(response_buffer)); // Очистка буфера
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    esp_http_client_cleanup(client);
}

static void get_humid_status_from_server(void *pvParameters) {
    char url_buffer[FULL_URL_SIZE];  // Создаем отдельный буфер для полного URL

    // Формируем полный URL для запроса состояния реле
    snprintf(url_buffer, sizeof(url_buffer), "%s/devices/humidifier", SERVER_URL);
    esp_http_client_config_t config = {
        .url = url_buffer,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    while (1) {
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK && response_received) {
            ESP_LOGI(TAG, "Humid: Response: %s", response_buffer);
            bool relay_state = strcmp(response_buffer, "1") == 0;
            humid_control(relay_state);
        } else {
            ESP_LOGE(TAG, "Humid: HTTP GET request failed: %s", esp_err_to_name(err));
        }
        
        response_received = false;
        memset(response_buffer, 0, sizeof(response_buffer)); // Очистка буфера
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    esp_http_client_cleanup(client);
}

static void get_fresh_status_from_server(void *pvParameters) {
    char url_buffer[FULL_URL_SIZE];  // Создаем отдельный буфер для полного URL

    // Формируем полный URL для запроса состояния реле
    snprintf(url_buffer, sizeof(url_buffer), "%s/devices/freshener", SERVER_URL);
    esp_http_client_config_t config = {
        .url = url_buffer,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    while (1) {
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK && response_received) {
            ESP_LOGI(TAG, "Fresh: Response: %s", response_buffer);
            bool relay_state = strcmp(response_buffer, "1") == 0;
            fresh_control(relay_state);
        } else {
            ESP_LOGE(TAG, "Fresh: HTTP GET request failed: %s", esp_err_to_name(err));
        }
        
        response_received = false;
        memset(response_buffer, 0, sizeof(response_buffer)); // Очистка буфера
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    esp_http_client_cleanup(client);
}

void app_main() {
    nvs_flash_init();
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_sta(); // Функция для инициализации Wi-Fi

    esp_rom_gpio_pad_select_gpio(FAN_GPIO_PIN);
    gpio_set_direction(FAN_GPIO_PIN, GPIO_MODE_OUTPUT);
    esp_rom_gpio_pad_select_gpio(HUMID_GPIO_PIN);
    gpio_set_direction(HUMID_GPIO_PIN, GPIO_MODE_OUTPUT);
    esp_rom_gpio_pad_select_gpio(FRESH_GPIO_PIN);
    gpio_set_direction(FRESH_GPIO_PIN, GPIO_MODE_OUTPUT);

    xTaskCreate(&get_fan_status_from_server, "http_request_task", 8192, NULL, 5, NULL);
    xTaskCreate(&get_humid_status_from_server, "http_request_task", 8192, NULL, 5, NULL);
    xTaskCreate(&get_fresh_status_from_server, "http_request_task", 8192, NULL, 5, NULL);
}