//MQTT functionality added with help of chatGPT
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
#include <cstring> 
#include "esp_tls.h"

//GPIO definitions
#define LED_PIN GPIO_NUM_12        // GPIO pin for LED
#define BUTTON_PIN GPIO_NUM_11     // GPIO pin for button
#define LED_ON_TIME 300000         // 5 minutes in milliseconds
#define BUTTON_HOLD_TIME 5000      // 5 seconds in milliseconds

//I2c definitions
#define I2C_PORT I2C_NUM_0
#define I2C_MASTER_SCL_IO 7       /*!< GPIO number for I2C SCL */
#define I2C_MASTER_SDA_IO 6       /*!< GPIO number for I2C SDA */
#define I2C_MASTER_FREQ_HZ 100000 /*!< I2C clock frequency */
#define I2C_MASTER_PORT I2C_NUM_0 /*!< I2C port number */

//Wi-Fi definitions
#define WIFI_SSID "Citadel Guest"
#define WIFI_PASS ""

//Server definitions
#define BROKER_URI "mqtts://7eeb00b921c44e3690599cd08fbc29a5.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define USERNAME "Freshair1"  
#define PASSWORD "Freshair1" 

// This is an example of a root certificate, need to replace with hivemq certificate data
extern const uint8_t mqtt_root_cert_start[] asm("_binary_mqtt_root_cert_pem_start");
extern const uint8_t mqtt_root_cert_end[] asm("_binary_mqtt_root_cert_pem_end");

//MQTT Client handle
static esp_mqtt_client_handle_t mqtt_client = NULL;

//Identifies LOG output 
static const char *TAG = "IAQI_MAIN";

// GPIO Initialization
void gpio_init() {
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0); // Ensure LED starts OFF
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY); // Enables internal pull-up resistor for active low
}

// I2C Initialization
void i2c_master_init() {
    ESP_LOGI(TAG, "Configuring I2C parameters...");
    i2c_config_t conf = {}; // Ensure struct is zero-initialized
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_PORT, I2C_MODE_MASTER, 0, 0, 0));
}

// AQI Breakpoints for PM2.5 and PM10 (example values)
typedef struct {
    int low;
    int high;
    int aqi_low;
    int aqi_high;
} AQI_Breakpoint;

AQI_Breakpoint aqi_table_pm25[] = {
    {0, 12, 0, 50},
    {12, 35, 51, 100},
    {35, 55, 101, 150},
    {55, 150, 151, 200},
    {150, 250, 201, 300},
    {250, 350, 301, 400},
    {350, 500, 401, 500},
};

AQI_Breakpoint aqi_table_pm10[] = {
    {0, 54, 0, 50},
    {54, 154, 51, 100},
    {154, 254, 101, 150},
    {254, 354, 151, 200},
    {354, 424, 201, 300},
    {424, 504, 301, 400},
    {504, 604, 401, 500},
};

// Function to calculate IAQI based on the sensor value and the AQI breakpoints
int calculate_iaqi(float value, AQI_Breakpoint* table, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (value >= table[i].low && value < table[i].high) {
            float iaqi = table[i].aqi_low + ((value - table[i].low) * (table[i].aqi_high - table[i].aqi_low)) / (table[i].high - table[i].low);
            return static_cast<int>(iaqi);
        }
    }
    return -1; // Error case, return -1 if value is out of range
}

//Function Prototype for send_aqi_data
void send_aqi_data(int iaqi_pm25, int iaqi_pm10, int overall_iaqi);

// Air Quality Sensor Task
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

        send_aqi_data(iaqi_pm25, iaqi_pm10, overall_iaqi);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Button Debounce Function
bool is_button_pressed() {
    if (gpio_get_level(BUTTON_PIN) == 0) { 
        vTaskDelay(pdMS_TO_TICKS(50));  // Debounce
        return (gpio_get_level(BUTTON_PIN) == 0);
    }
    return false;
}

// LED Control Task
void led_task(void *pvParameter) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_ON_TIME)); // Wait 5 minutes
        gpio_set_level(LED_PIN, 1); // Turn LED ON
        ESP_LOGI("LED", "ON");

        int held_time = 0;
        while (1) {
            if (is_button_pressed()) {
                vTaskDelay(pdMS_TO_TICKS(100)); // Button debounce check
                held_time += 100;

                if (held_time >= BUTTON_HOLD_TIME) { // Button held for 5 seconds
                    ESP_LOGI("LED", "Button held for 5s, turning LED OFF");
                    gpio_set_level(LED_PIN, 0);
                    vTaskDelay(pdMS_TO_TICKS(LED_ON_TIME)); // Wait 5 minutes before reactivating LED
                    break;
                }
            } else {
                held_time = 0; // Reset hold timer if button is released
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to prevent busy loop
        }
    }
}

// Wi-Fi Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, retrying...");
        esp_wifi_connect();
    }
}

// Wi-Fi Initialization
void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    std::memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    std::memcpy(wifi_config.sta.password, WIFI_PASS, strlen(WIFI_PASS));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Send AQI Data over MQTT
void send_aqi_data(int iaqi_pm25, int iaqi_pm10, int overall_iaqi) {
    char payload[100];
    snprintf(payload, sizeof(payload), "{\"IAQI_PM2.5\":%d,\"IAQI_PM10\":%d,\"Overall_IAQI\":%d}", iaqi_pm25, iaqi_pm10, overall_iaqi);
    esp_mqtt_client_publish(mqtt_client, "sensor/air_quality", payload, 0, 1, 0);
}

// Main Application Entry
extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    i2c_master_init();
    gpio_init(); // Initialize GPIOs

    // Initialize MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = BROKER_URI;   // Broker URI (mqtt:// or mqtts://)
    mqtt_cfg.broker.address.port = MQTT_PORT;   //
    mqtt_cfg.credentials.username = USERNAME;   // MQTT username
    mqtt_cfg.credentials.password = PASSWORD;   // MQTT password
    mqtt_cfg.broker.verification.certificate = mqtt_root_cert_start;    // Root CA certificate for secure connection

    // Initialize the MQTT client with the configuration
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    // Check for successful initialization
    if (mqtt_client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    // Start the MQTT client connection
    if (esp_mqtt_client_start(mqtt_client) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
    }

    // Create tasks
    xTaskCreate(&air_quality_task, "air_quality_task", 4096, &mqtt_client, 5, NULL);
    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL); // Add LED task

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
