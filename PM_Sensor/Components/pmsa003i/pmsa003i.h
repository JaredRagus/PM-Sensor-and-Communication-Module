#ifndef PMSA003I_H
#define PMSA003I_H

#include "esp_err.h"
#include "driver/i2c.h"

class PMSA003I {
public:
    explicit PMSA003I(i2c_port_t i2c_port);
    void update();

private:
    i2c_port_t i2c_port_;
    esp_err_t read_bytes(uint8_t *buffer, size_t length);
    void parse_data_(uint8_t *buffer);
};

#endif // PMSA003I_H
