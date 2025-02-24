#ifndef PMSA003I_H
#define PMSA003I_H

#include "esp_err.h"
#include "driver/i2c.h"

class PMSA003I {
public:
    explicit PMSA003I(i2c_port_t i2c_port);
    void update();
    
    // Getter functions for sensor values
    float get_pm1_0() const { return pm1_0_; }
    float get_pm2_5() const { return pm2_5_; }
    float get_pm10_0() const { return pm10_0_; }

private:
    i2c_port_t i2c_port_;
    float pm1_0_, pm2_5_, pm10_0_;  // Store sensor values

    esp_err_t read_bytes(uint8_t *buffer, size_t length);
    void parse_data_(uint8_t *buffer);
};

#endif // PMSA003I_H
