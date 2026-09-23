#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Nau8325 {
 public:
  Nau8325(i2c_master_bus_handle_t bus, SemaphoreHandle_t bus_mutex);
  ~Nau8325();

  bool begin(uint32_t sample_rate);
  void mute(bool enabled);

 private:
  bool write(uint16_t reg, uint16_t value);
  bool read(uint16_t reg, uint16_t& value);
  bool update(uint16_t reg, uint16_t mask, uint16_t value);

  i2c_master_dev_handle_t device_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
};
