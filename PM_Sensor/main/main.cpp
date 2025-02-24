#include "pmsa003i.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include <string>
#include "esp_mac.h"
#include "esp_system.h"
#include <cstring>
#include <iostream>
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt.h"

#define LED_PIN GPIO_NUM_12        
#define BUTTON_PIN GPIO_NUM_9      
#define LED_ON_TIME 300000         
#define BUTTON_HOLD_TIME 5000      

#define I2C_PORT I2C_NUM_0
static const char *TAG = "IAQI_MAIN";

#define I2C_MASTER_SCL_IO 7       
#define I2C_MASTER_SDA_IO 6       
#define I2C_MASTER_FREQ_HZ 100000 
#define I2C_MASTER_PORT I2C_NUM_0 

#define WIFI_SSID "mariad05"
#define WIFI_PASS "ragus123"

#define MQTT_BROKER_URI "tcp://broker.hivemq.com:1883"  
#define MQTT_CLIENT_ID "ESP32_AQI_Client"
#define MQTT_TOPIC "sensor/iaqi"

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
            // Calculate the IAQI
            float iaqi = table[i].aqi_low + ((value - table[i].low) * (table[i].aqi_high - table[i].aqi_low)) / (table[i].high - table[i].low);
            return static_cast<int>(iaqi);
        }
    }
    return -1; // Return -1 if value is out of range
}

// Publish IAQI
void publish_iaqi(int overall_iaqi, esp_idf_cxx::mqtt::Client &mqtt_client) {
    std::string message = "Overall IAQI: " + std::to_string(overall_iaqi);
    mqtt_client.publish(MQTT_TOPIC, message);  // Use wrapper method to publish
}

// Retrieves sensor readings and calculates IAQI
void air_quality_task(void *pvParameter) {
    esp_idf_cxx::mqtt::Client *mqtt_client = static_cast<esp_idf_cxx::mqtt::Client *>(pvParameter);  // Cast back to the correct type
    PMSA003I sensor(I2C_PORT);

    while (1) {
        sensor.update();  
        float pm2_5 = sensor.get_pm2_5();   
        float pm10_0 = sensor.get_pm10_0(); 

        int iaqi_pm25 = calculate_iaqi(pm2_5, aqi_table_pm25, sizeof(aqi_table_pm25) / sizeof(AQI_Breakpoint));
        int iaqi_pm10 = calculate_iaqi(pm10_0, aqi_table_pm10, sizeof(aqi_table_pm10) / sizeof(AQI_Breakpoint));
        int overall_iaqi = (iaqi_pm25 > iaqi_pm10) ? iaqi_pm25 : iaqi_pm10;

        printf("PM2.5: %.1f µg/m³ | PM10: %.1f µg/m³\n", pm2_5, pm10_0);
        printf("IAQI PM2.5: %d | IAQI PM10: %d | Overall IAQI: %d\n", iaqi_pm25, iaqi_pm10, overall_iaqi);

        // Publish the IAQI to MQTT
        publish_iaqi(overall_iaqi, *mqtt_client);  // Use the wrapper to publish

        vTaskDelay(pdMS_TO_TICKS(5000)); // Delay for 5 seconds
    }
}

// Wi-Fi Initialization
void wifi_init() {
    wifi_config_t wifi_config = {};
    memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASS, strlen(WIFI_PASS));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Wi-Fi initialized successfully");
    }
}

// I2C Initialization
void i2c_master_init() {
    printf("I2C", "Configuring I2C parameters...");
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

    esp_err_t err = i2c_param_config(I2C_MASTER_PORT, &conf);
    if (err != ESP_OK) {
        printf("I2C", "Failed to configure I2C: %s", esp_err_to_name(err));
        return;
    }

    printf("I2C", "Installing I2C driver...");
    err = i2c_driver_install(I2C_MASTER_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        printf("I2C", "Failed to install I2C driver: %s", esp_err_to_name(err));
        return;
    }

    printf("I2C", "I2C driver installed successfully!");
}

// Button Debounce Function
bool is_button_pressed() {
    if (gpio_get_level(BUTTON_PIN) == 0) { 
        vTaskDelay(pdMS_TO_TICKS(50));  // Debounce delay
        return (gpio_get_level(BUTTON_PIN) == 0);  
    }
    return false;
}

// Function for LED
void led_task(void *pvParameter) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_ON_TIME));
        gpio_set_level(LED_PIN, 1);
        printf("LED", "ON");

        int held_time = 0;
        while (is_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            held_time += 100;

            if (held_time >= BUTTON_HOLD_TIME) {
                printf("LED", "Button held, turning LED OFF");
                gpio_set_level(LED_PIN, 0);
                break;
            }
        }
    }
}

extern "C" void app_main() {
    // Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();              
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Wi-Fi
    wifi_init();
    esp_event_loop_create_default();
    
     // Create the MQTT client wrapper and connect to the broker
     esp_idf_cxx::mqtt::Client mqtt_client(MQTT_BROKER_URI);
     if (mqtt_client.connect()) {
         mqtt_client.publish(MQTT_TOPIC, "Hello from ESP32 with MQTT!");
     }

    // Set GPIO for Button and LED
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);

    // Create tasks
    xTaskCreate(&air_quality_task, "air_quality_task", 4096, &mqtt_client, 5, NULL);
    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL);

    // Main loop
    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}