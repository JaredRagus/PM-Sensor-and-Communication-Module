#include "pmsa003i.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PMSA003I_ADDRESS 0x12  // Replace with actual I2C address if different
#define I2C_PORT I2C_NUM_0
static const char* TAG = "PMSA003I";

PMSA003I::PMSA003I(i2c_port_t i2c_port) : i2c_port_(i2c_port) {}

esp_err_t PMSA003I::read_bytes(uint8_t *buffer, size_t length) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PMSA003I_ADDRESS << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buffer, length, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(i2c_port_, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return err;
}

void PMSA003I::update() {
    uint8_t buffer[32];
    if (read_bytes(buffer, sizeof(buffer)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read from sensor!");
        return;
    }
    parse_data_(buffer);
}

void PMSA003I::parse_data_(uint8_t *buffer) {
    // Process buffer data and extract PM1.0, PM2.5, PM10 values
    ESP_LOGI(TAG, "Sensor data processed");
}

void pmsa003i_task(void *pvParameter) {
    PMSA003I sensor(I2C_PORT);
    while (1) {
        sensor.update();
        vTaskDelay(pdMS_TO_TICKS(60000)); // 60s delay
    }
}

extern "C" void app_main() {
    xTaskCreate(&pmsa003i_task, "pmsa003i_task", 4096, NULL, 5, NULL);
}
