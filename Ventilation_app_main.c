#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#define RELAY_GPIO GPIO_NUM_0   // Relay control pin
#define SWITCH_GPIO GPIO_NUM_15 // Slide switch pin

static const char *TAG = "relay_control";
esp_mqtt_client_handle_t client;
static bool relay_state = false;
static QueueHandle_t switch_queue;
static bool wifi_connected = false;
static bool mqtt_connected = false;
static bool online_mode = false;

// Wi‑Fi credentials for your hotspot
#define WIFI_SSID "DAVIDGAYLE"
#define WIFI_PASS "12345678"

// Function to publish relay state over MQTT
void publish_relay_state(bool state) {
    if (mqtt_connected) {
        const char* state_topic = "relay/state";
        const char* payload = state ? "ON" : "OFF";
        esp_mqtt_client_publish(client, state_topic, payload, 0, 1, 0);
    }
}

// MQTT event handler
esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(client, "relay/set", 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGI(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (online_mode && strncmp(event->topic, "relay/set", event->topic_len) == 0) {
                relay_state = (strncmp(event->data, "ON", event->data_len) == 0);
                gpio_set_level(RELAY_GPIO, relay_state);
                publish_relay_state(relay_state);
            }
            break;
        default:
            ESP_LOGI(TAG, "Other MQTT event: %d", event->event_id);
            break;
    }
    return ESP_OK;
}

// Wi‑Fi event handler
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi‑Fi started, attempting to connect...");
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_connected = true;
        online_mode = true;
        ESP_LOGI(TAG, "Wi‑Fi connected, IP address obtained.");
        
        // Optionally print the IP address
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ESP_IF_WIFI_STA, &ip_info);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip_info.ip));
        
        // Start MQTT if not already started
        if (!mqtt_connected) {
            ESP_LOGI(TAG, "Starting MQTT...");
            esp_mqtt_client_config_t mqtt_cfg = {
                .broker.address = "mqtt.eclipseprojects.io",  // Broker address
                .broker.port = 1883,                          // Broker port
            };
            client = esp_mqtt_client_init(&mqtt_cfg);
            esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_mqtt_client_start(client);
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        mqtt_connected = false;
        online_mode = false;
        ESP_LOGI(TAG, "Wi‑Fi disconnected, attempting to reconnect...");
        vTaskDelay(pdMS_TO_TICKS(5000));  // Add a delay to avoid continuous reconnects
        esp_wifi_connect();
    }
}

// Wi‑Fi initialization (should only be done once)
void wifi_init() {
    ESP_LOGI(TAG, "Initializing Wi‑Fi...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());

    // Create the default event loop only if it has not been created yet.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE means it already exists.
        ESP_ERROR_CHECK(err);
    }
    
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Interrupt service routine (ISR) for the switch
static void IRAM_ATTR switch_isr_handler(void* arg) {
    int switch_state = gpio_get_level(SWITCH_GPIO);
    xQueueSendFromISR(switch_queue, &switch_state, NULL);
}

// Task to handle switch changes
void switch_task(void* arg) {
    int switch_state;
    while (1) {
        if (xQueueReceive(switch_queue, &switch_state, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce delay
            switch_state = gpio_get_level(SWITCH_GPIO); // Read again after debounce

            // Relay works in both Offline and Online mode
            relay_state = switch_state;
            gpio_set_level(RELAY_GPIO, relay_state);
            publish_relay_state(relay_state);
        }
    }
}

// Wi‑Fi reconnect task: now it simply calls esp_wifi_connect() if not connected.
void wifi_reconnect_task(void* arg) {
    while (1) {
        if (!wifi_connected) {
            ESP_LOGI(TAG, "Wi‑Fi not connected, trying to reconnect...");
            esp_wifi_connect();
        }

        // Check relay state while waiting for Wi‑Fi
        int switch_state = gpio_get_level(SWITCH_GPIO);
        relay_state = switch_state;
        gpio_set_level(RELAY_GPIO, relay_state);
        publish_relay_state(relay_state);

        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
    }
}

void app_main() {
    // Initialize GPIO
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(SWITCH_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SWITCH_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(SWITCH_GPIO, GPIO_INTR_ANYEDGE);

    // Create queue for switch state
    switch_queue = xQueueCreate(10, sizeof(int));

    // Install ISR for switch
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SWITCH_GPIO, switch_isr_handler, NULL);

    // Create switch monitoring task
    xTaskCreate(switch_task, "switch_task", 2048, NULL, 10, NULL);

    // Initialize Wi‑Fi once
    wifi_init();

    // Create Wi‑Fi reconnect task (attempts reconnection if disconnected)
    xTaskCreate(wifi_reconnect_task, "wifi_reconnect_task", 4096, NULL, 5, NULL);

    // Main loop: Log Wi‑Fi status every 10 seconds
    while (1) {
        if (wifi_connected) {
            ESP_LOGI(TAG, "Wi‑Fi is connected.");
        } else {
            ESP_LOGW(TAG, "Wi‑Fi is not connected.");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
