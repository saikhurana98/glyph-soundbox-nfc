#include "pn532.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr uint8_t kAddress = 0x24;
constexpr char kTag[] = "pn532";
}

Pn532::Pn532(i2c_master_bus_handle_t bus, SemaphoreHandle_t bus_mutex)
    : mutex_(bus_mutex) {
  const i2c_device_config_t config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = kAddress,
      .scl_speed_hz = 100000,
      .scl_wait_us = 0,
      .flags = {},
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device_));
}

Pn532::~Pn532() {
  if (device_) i2c_master_bus_rm_device(device_);
}

bool Pn532::ready() {
  uint8_t status = 0;
  return i2c_master_receive(device_, &status, 1, 20) == ESP_OK && (status & 1);
}

bool Pn532::wait_ready(uint32_t timeout_ms) {
  const TickType_t started = xTaskGetTickCount();
  do {
    if (ready()) return true;
    vTaskDelay(pdMS_TO_TICKS(2));
  } while ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(timeout_ms));
  return false;
}

bool Pn532::write_command(uint8_t command, const uint8_t* params,
                          size_t params_length) {
  command_ = command;
  uint8_t frame[32]{};
  if (params_length + 9 > sizeof(frame)) return false;
  const uint8_t length = static_cast<uint8_t>(params_length + 2);
  uint8_t checksum = static_cast<uint8_t>(0xD4 + command);
  size_t offset = 0;
  frame[offset++] = 0x00;
  frame[offset++] = 0x00;
  frame[offset++] = 0xFF;
  frame[offset++] = length;
  frame[offset++] = static_cast<uint8_t>(~length + 1);
  frame[offset++] = 0xD4;
  frame[offset++] = command;
  for (size_t i = 0; i < params_length; ++i) {
    frame[offset++] = params[i];
    checksum = static_cast<uint8_t>(checksum + params[i]);
  }
  frame[offset++] = static_cast<uint8_t>(~checksum + 1);
  frame[offset++] = 0x00;
  if (i2c_master_transmit(device_, frame, offset, 100) != ESP_OK ||
      !wait_ready(120)) {
    return false;
  }
  uint8_t ack[7]{};
  if (i2c_master_receive(device_, ack, sizeof(ack), 100) != ESP_OK) return false;
  static constexpr uint8_t expected[] = {0x01, 0x00, 0x00, 0xFF, 0x00, 0xFF};
  return std::memcmp(ack, expected, sizeof(expected)) == 0;
}

int Pn532::read_response(uint8_t* output, size_t capacity, uint32_t timeout_ms) {
  if (!wait_ready(timeout_ms)) return -1;
  uint8_t frame[40]{};
  if (i2c_master_receive(device_, frame, sizeof(frame), 100) != ESP_OK) return -2;
  if (!(frame[0] & 1) || frame[1] != 0x00 || frame[2] != 0x00 ||
      frame[3] != 0xFF) return -3;
  const uint8_t payload_length = frame[4];
  if (static_cast<uint8_t>(payload_length + frame[5]) != 0 ||
      payload_length < 2 || payload_length + 8 > sizeof(frame) ||
      frame[6] != 0xD5 || frame[7] != static_cast<uint8_t>(command_ + 1)) {
    return -4;
  }
  const size_t data_length = payload_length - 2;
  if (data_length > capacity) return -5;
  std::memcpy(output, frame + 8, data_length);
  return static_cast<int>(data_length);
}

int Pn532::transact(uint8_t command, const uint8_t* params,
                    size_t params_length, uint8_t* output, size_t capacity,
                    uint32_t timeout_ms) {
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms + 300)) != pdTRUE) return -6;
  const bool wrote = write_command(command, params, params_length);
  const int result = wrote ? read_response(output, capacity, timeout_ms) : -1;
  xSemaphoreGive(mutex_);
  return result;
}

uint32_t Pn532::firmware_version() {
  uint8_t reply[8]{};
  const int length = transact(0x02, nullptr, 0, reply, sizeof(reply), 150);
  if (length < 4) return 0;
  return (uint32_t(reply[0]) << 24) | (uint32_t(reply[1]) << 16) |
         (uint32_t(reply[2]) << 8) | reply[3];
}

bool Pn532::begin() {
  const uint32_t version = firmware_version();
  if (!version) return false;
  const uint8_t sam[] = {0x01, 0x14, 0x01};
  uint8_t reply[4]{};
  if (transact(0x14, sam, sizeof(sam), reply, sizeof(reply), 150) < 0) return false;
  const uint8_t retries[] = {0x05, 0xFF, 0x01, 0x01};
  if (transact(0x32, retries, sizeof(retries), reply, sizeof(reply), 150) < 0) {
    return false;
  }
  ESP_LOGI(kTag, "ready, firmware %lu.%lu", (version >> 16) & 0xFF,
           (version >> 8) & 0xFF);
  return true;
}

bool Pn532::read_uid(uint8_t* uid, uint8_t* uid_length, uint32_t timeout_ms) {
  const uint8_t params[] = {0x01, 0x00};
  uint8_t reply[24]{};
  const int length = transact(0x4A, params, sizeof(params), reply, sizeof(reply),
                              timeout_ms);
  if (length < 7 || reply[0] != 1) return false;
  const uint8_t card_length = reply[5];
  if (card_length < 3 || card_length > 10 || 6 + card_length > length) return false;
  *uid_length = card_length;
  std::memcpy(uid, reply + 6, card_length);
  return true;
}
