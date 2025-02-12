#include "pmsa003i.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_PORT I2C_NUM_0
static const char *TAG = "IAQI_MAIN";

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

void air_quality_task(void *pvParameter) {
    PMSA003I sensor(I2C_PORT);

    while (1) {
        sensor.update();
        
        // Simulated sensor readings (replace with actual sensor data retrieval)
        float pm1_0 = 10.0;   // Example PM1.0 value
        float pm2_5 = 25.0;   // Example PM2.5 value
        float pm10_0 = 80.0;  // Example PM10 value

        int iaqi_pm25 = calculate_iaqi(pm2_5, aqi_table_pm25, sizeof(aqi_table_pm25) / sizeof(AQI_Breakpoint));
        int iaqi_pm10 = calculate_iaqi(pm10_0, aqi_table_pm10, sizeof(aqi_table_pm10) / sizeof(AQI_Breakpoint));

        int overall_iaqi = (iaqi_pm25 > iaqi_pm10) ? iaqi_pm25 : iaqi_pm10;

        ESP_LOGI(TAG, "PM1.0: %.1f µg/m³ | PM2.5: %.1f µg/m³ | PM10: %.1f µg/m³", pm1_0, pm2_5, pm10_0);
        ESP_LOGI(TAG, "IAQI PM2.5: %d | IAQI PM10: %d | Overall IAQI: %d", iaqi_pm25, iaqi_pm10, overall_iaqi);

        vTaskDelay(pdMS_TO_TICKS(60000)); // Wait 60 seconds
    }
}

extern "C" void app_main() {
    xTaskCreate(&air_quality_task, "air_quality_task", 4096, NULL, 5, NULL);
}

