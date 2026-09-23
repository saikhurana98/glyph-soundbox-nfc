#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Pn532 {
 public:
  Pn532(i2c_master_bus_handle_t bus, SemaphoreHandle_t bus_mutex);
  ~Pn532();

  bool begin();
  uint32_t firmware_version();
  bool read_uid(uint8_t* uid, uint8_t* uid_length, uint32_t timeout_ms);

 private:
  bool ready();
  bool wait_ready(uint32_t timeout_ms);
  bool write_command(uint8_t command, const uint8_t* params, size_t params_length);
  int read_response(uint8_t* output, size_t capacity, uint32_t timeout_ms);
  int transact(uint8_t command, const uint8_t* params, size_t params_length,
               uint8_t* output, size_t capacity, uint32_t timeout_ms);

  i2c_master_dev_handle_t device_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  uint8_t command_ = 0;
};
