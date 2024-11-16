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
#define FRESH2_GPIO_PIN 17
#define LIMIT_SWITCH_PIN 5 // Пин для концевика

static const char *TAG = "ESP32_HTTP_CLIENT";

#define WIFI_SSID      "SamsungA422_2G"
#define WIFI_PASS      "samsunghack"
#define MAX_RETRY      5
// static char* SERVER_URL  =   "http://192.168.1.46:9898";

#define FULL_URL_SIZE 128

typedef struct {
    char url[FULL_URL_SIZE];
    char response_buffer[1024]; // Буфер для хранения ответа
    bool response_received;
} http_task;

static bool limit_status;

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void freshener_timer() {
    vTaskDelay(1800000 / portTICK_PERIOD_MS);
}

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

static void fresh2_control(bool state) {
    gpio_set_level(FRESH2_GPIO_PIN, state ? 1 : 0);
}


static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    http_task *task_data = (http_task *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len < sizeof(task_data->response_buffer) - 1) {
        strncpy(task_data->response_buffer, (char*)evt->data, evt->data_len);
        task_data->response_buffer[evt->data_len] = 0; // Завершаем строку
        task_data->response_received = true;
    }
    return ESP_OK;
}

static void get_device_status_from_server(void *pvParameters) {
    http_task *task_data = (http_task *) pvParameters;
    esp_http_client_config_t config = {
        .url = task_data->url,
        .event_handler = _http_event_handler,
        .user_data = task_data
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    while (1) {
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK && task_data->response_received) {
            ESP_LOGI(TAG, "Response from %s: %s", task_data->url, task_data->response_buffer);
            bool relay_state = strcmp(task_data->response_buffer, "1") == 0;

            // Управляем устройствами на основе URL и состояния
            if (strstr(task_data->url, "fan") != NULL && !limit_status) fan_control(relay_state);
            else if (strstr(task_data->url, "humidifier") != NULL && !limit_status) humid_control(relay_state);
            else if (strstr(task_data->url, "freshener") != NULL && !limit_status) {
                fresh_control(relay_state);
                if (relay_state) freshener_timer();
            }
            else if (strstr(task_data->url, "freshener2") != NULL && !limit_status) {
                fresh2_control(relay_state);
                if (relay_state) freshener_timer();
            }
            
        } else {
            ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        }

        task_data->response_received = false;
        memset(task_data->response_buffer, 0, sizeof(task_data->response_buffer));
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    esp_http_client_cleanup(client);
}

void init_limit_switch() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LIMIT_SWITCH_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  
        .pull_down_en = GPIO_PULLDOWN_DISABLE, 
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

static void read_limit_switch() {
    while (1) {
        limit_status = gpio_get_level(LIMIT_SWITCH_PIN);
        if (!limit_status) {
            fresh_control(false);
            fan_control(false);
            humid_control(false);
        }
    }
}

void app_main() {
    nvs_flash_init();
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_sta(); // Функция для инициализации Wi-Fi
    init_limit_switch();

    esp_rom_gpio_pad_select_gpio(FAN_GPIO_PIN);
    gpio_set_direction(FAN_GPIO_PIN, GPIO_MODE_OUTPUT);
    esp_rom_gpio_pad_select_gpio(HUMID_GPIO_PIN);
    gpio_set_direction(HUMID_GPIO_PIN, GPIO_MODE_OUTPUT);
    esp_rom_gpio_pad_select_gpio(FRESH_GPIO_PIN);
    gpio_set_direction(FRESH_GPIO_PIN, GPIO_MODE_OUTPUT);
    esp_rom_gpio_pad_select_gpio(FRESH_GPIO_PIN);
    gpio_set_direction(FRESH2_GPIO_PIN, GPIO_MODE_OUTPUT);

    static http_task fan_data = {.url = "http://192.168.1.46:9898/devices/fan"}; // изменить url
    static http_task humid_data = {.url = "http://192.168.1.46:9898/devices/humidifier"};
    static http_task fresh_data = {.url = "http://192.168.1.46:9898/devices/freshener"};
    static http_task fresh2_data = {.url = "http://192.168.1.46:9898/devices/freshener2"};

    xTaskCreate(&get_device_status_from_server, "fan_task", 4096, &fan_data, 5, NULL);
    xTaskCreate(&get_device_status_from_server, "humid_task", 4096, &humid_data, 5, NULL);
    xTaskCreate(&get_device_status_from_server, "fresh_task", 4096, &fresh_data, 5, NULL);
    xTaskCreate(&get_device_status_from_server, "fresh2_task", 4096, &fresh2_data, 5, NULL);
    xTaskCreate(&read_limit_switch, "monitor_limit_switch_task", 2048, NULL, 5, NULL);
    xTaskCreate(&read_limit_switch, "monitor_limit_switch_task", 2048, NULL, 5, NULL);
}