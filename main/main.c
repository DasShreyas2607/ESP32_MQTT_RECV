#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#include "tm1637.h"

#define WIFI_SSID "Note 12"
#define WIFI_PASS "Olymp1ad"
#define BLINK_GPIO GPIO_NUM_2

#define MQTT_URL "mqtt://pi:pass@192.168.150.177:1883"
#define MQTT_TOPIC "mqtt/parking/display"

#define LOOP false

// Choose bw 0 - 34
const gpio_num_t ROW_1[2] = {22, 23};
const gpio_num_t ROW_2[2] = {19, 21};
const gpio_num_t ROW_3[2] = {5, 18};

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

static uint8_t s_led_state = true;

struct Payload
{
    int bay_no;
    int isParked[9];
    time_t timeStamp;
};
struct Payload msg;

static const char *TAG = "MQTT_SUBS";
tm1637_led_t *lcd[3];

void led_driver()
{
    do
    {
        #if (LOOP==true) 
        vTaskDelay(pdMS_TO_TICKS(1500));
        #endif
        ESP_LOGI(TAG, " ==================> Start %d <==================\n", s_led_state);
        for (int i = 0; i < 3; i++)
        {
            ESP_LOGI(TAG, "%d\t%d\t%d\t%d\n", msg.bay_no + i, msg.isParked[i * 2] ? msg.isParked[i * 2] : 0, msg.isParked[i * 2 + 1] ? msg.isParked[i * 2 + 1] : 0, msg.isParked[i * 2 + 2] ? msg.isParked[i * 2 + 2] : 0);
        }
        ESP_LOGI(TAG, " ===================> END %d <===================\n", s_led_state);

        gpio_set_level(BLINK_GPIO, s_led_state);
        s_led_state = !s_led_state;

        for (int i = 0; i < 3; i++) {
            tm1637_set_segment_number(lcd[i], 0, msg.bay_no + i, false);
            for (int j = 1; j < 4; j++) {
                tm1637_set_segment_number(lcd[i], j, msg.isParked[i * 2 + j - 1] ? msg.isParked[i * 2 + j - 1] : 16, false);
            }
        }

    } while (LOOP);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, MQTT_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        memcpy(&msg, event->data, event->data_len);
        #if (LOOP==false) 
        led_driver(); 
        #endif
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URL,
        
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

static void wifi_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "==================> CONNECTING <==================");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "==================> CONNECTING <==================");
            esp_wifi_connect();
            break;
        default:
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "==================> CONNECTING <==================");
            esp_wifi_connect();
            break;
        }
    }
    else if (base == IP_EVENT)
    {
        switch (id)
        {
        case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            mqtt_init();
        }
        }
    }
}

void wifi_init()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "==================> BOOTING <==================");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    
    lcd[0] = tm1637_init(ROW_1[0], ROW_1[1]);
    lcd[1] = tm1637_init(ROW_2[0], ROW_2[1]);
    lcd[2] = tm1637_init(ROW_3[0], ROW_3[1]);

    wifi_init();
    #if (LOOP==true) 
    xTaskCreate(&led_driver, "led_driver", 9216, NULL, 5, NULL);
    #endif
    return;
}