#include "pmsa003i.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "esp_event.h"

#define LED_PIN GPIO_NUM_12        // GPIO pin for LED
#define BUTTON_PIN GPIO_NUM_11     // Changed to a valid GPIO pin for button
#define LED_ON_TIME 300000         // 5 minutes in milliseconds
#define BUTTON_HOLD_TIME 5000      // 5 seconds in milliseconds

#define I2C_PORT I2C_NUM_0
static const char *TAG = "IAQI_MAIN";

#define I2C_MASTER_SCL_IO 7       /*!< GPIO number for I2C SCL */
#define I2C_MASTER_SDA_IO 6       /*!< GPIO number for I2C SDA */
#define I2C_MASTER_FREQ_HZ 100000 /*!< I2C clock frequency */
#define I2C_MASTER_PORT I2C_NUM_0 /*!< I2C port number */

#define WIFI_SSID "Citadel Guest"
#define WIFI_PASS ""

#define MQTT_BROKER_URI "mqtt://your-mqtt-broker"

//States Wi-Fi status
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI("WiFi", "Connected to WiFi");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI("WiFi", "Disconnected, retrying...");
        esp_wifi_connect();
    }
}

//Wi-Fi Initialization
void wifi_init() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN // Fixed authentication mode for open network
        }
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

//I2C Initialization
void i2c_master_init() {
    ESP_LOGI("I2C", "Configuring I2C parameters...");

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };

    esp_err_t err = i2c_param_config(I2C_MASTER_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE("I2C", "Failed to configure I2C: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI("I2C", "Installing I2C driver...");
    err = i2c_driver_install(I2C_MASTER_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE("I2C", "Failed to install I2C driver: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI("I2C", "I2C driver installed successfully!");
}

// AQI Breakpoints (Based on US EPA Standard)
typedef struct {
    float c_low, c_high;
    int i_low, i_high;
} AQI_Breakpoint;

const AQI_Breakpoint aqi_table_pm25[] = {
    {0.0, 12.0, 0, 50}, {12.1, 35.4, 51, 100}, {35.5, 55.4, 101, 150},
    {55.5, 150.4, 151, 200}, {150.5, 250.4, 201, 300}, {250.5, 500.4, 301, 500}
};

const AQI_Breakpoint aqi_table_pm10[] = {
    {0.0, 54.0, 0, 50}, {55.0, 154.0, 51, 100}, {155.0, 254.0, 101, 150},
    {255.0, 354.0, 151, 200}, {355.0, 424.0, 201, 300}, {425.0, 604.0, 301, 500}
};

// Function to calculate IAQI
int calculate_iaqi(float concentration, const AQI_Breakpoint table[], size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (concentration >= table[i].c_low && concentration <= table[i].c_high) {
            return ((table[i].i_high - table[i].i_low) / (table[i].c_high - table[i].c_low)) * 
                   (concentration - table[i].c_low) + table[i].i_low;
        }
    }
    return -1; // Error case
}

//Retrieves sensor readings and calculates AQI
void air_quality_task(void *pvParameter) {
    PMSA003I sensor(I2C_PORT);

    while (1) {
        sensor.update();  
        float pm2_5 = sensor.get_pm2_5();   
        float pm10_0 = sensor.get_pm10_0(); 

        int iaqi_pm25 = calculate_iaqi(pm2_5, aqi_table_pm25, sizeof(aqi_table_pm25) / sizeof(AQI_Breakpoint));
        int iaqi_pm10 = calculate_iaqi(pm10_0, aqi_table_pm10, sizeof(aqi_table_pm10) / sizeof(AQI_Breakpoint));
        int overall_iaqi = (iaqi_pm25 > iaqi_pm10) ? iaqi_pm25 : iaqi_pm10;

        ESP_LOGI(TAG, "PM2.5: %.1f µg/m³ | PM10: %.1f µg/m³", pm2_5, pm10_0);
        ESP_LOGI(TAG, "IAQI PM2.5: %d | IAQI PM10: %d | Overall IAQI: %d", iaqi_pm25, iaqi_pm10, overall_iaqi);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// MQTT Client Handle (Declare globally)
esp_mqtt_client_handle_t mqtt_client;

// MQTT Configuration
const esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri = MQTT_BROKER_URI
};

//Displays MQTT status
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Connected to MQTT broker");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI("MQTT", "Disconnected from MQTT broker");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI("MQTT", "Received Data: %.*s", event->data_len, event->data);
            break;
        default:
            ESP_LOGI("MQTT", "Other MQTT event occurred: %d", event->event_id);
            break;
    }
}

//Starts MQTT client
void mqtt_app_start() {
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

//Send PM data over MQTT
void send_aqi_data(float pm2_5, float pm10) {
    char payload[100];
    snprintf(payload, sizeof(payload), "{\"PM2.5\":%.1f,\"PM10\":%.1f}", pm2_5, pm10);
    esp_mqtt_client_publish(mqtt_client, "sensor/air_quality", payload, 0, 1, 0);
}

// Button Debounce Function
bool is_button_pressed() {
    if (gpio_get_level(BUTTON_PIN) == 0) { 
        vTaskDelay(pdMS_TO_TICKS(50)); 
        return (gpio_get_level(BUTTON_PIN) == 0);
    }
    return false;
}

//Function for LED
void led_task(void *pvParameter) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_ON_TIME));
        gpio_set_level(LED_PIN, 1);
        ESP_LOGI("LED", "ON");

        int held_time = 0;
        while (is_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            held_time += 100;

            if (held_time >= BUTTON_HOLD_TIME) {
                ESP_LOGI("LED", "Button held, turning LED OFF");
                gpio_set_level(LED_PIN, 0);
                break;
            }
        }
    }
}

extern "C" void app_main() {
    nvs_flash_init();
    wifi_init();
    mqtt_app_start();
    xTaskCreate(&air_quality_task, "air_quality_task", 4096, NULL, 5, NULL);
    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL);
}
