#include "nau8325.h"

#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr uint8_t kAddress = 0x21;
constexpr char kTag[] = "nau8325";
constexpr uint16_t kSoftMute = 1u << 15;
}

Nau8325::Nau8325(i2c_master_bus_handle_t bus, SemaphoreHandle_t bus_mutex)
    : mutex_(bus_mutex) {
  const i2c_device_config_t config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = kAddress,
      .scl_speed_hz = 400000,
      .scl_wait_us = 0,
      .flags = {},
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device_));
}

Nau8325::~Nau8325() {
  if (device_) i2c_master_bus_rm_device(device_);
}

bool Nau8325::write(uint16_t reg, uint16_t value) {
  const uint8_t bytes[] = {static_cast<uint8_t>(reg >> 8),
                           static_cast<uint8_t>(reg),
                           static_cast<uint8_t>(value >> 8),
                           static_cast<uint8_t>(value)};
  return i2c_master_transmit(device_, bytes, sizeof(bytes), 100) == ESP_OK;
}

bool Nau8325::read(uint16_t reg, uint16_t& value) {
  const uint8_t address[] = {static_cast<uint8_t>(reg >> 8),
                             static_cast<uint8_t>(reg)};
  uint8_t bytes[2]{};
  if (i2c_master_transmit_receive(device_, address, sizeof(address), bytes,
                                  sizeof(bytes), 100) != ESP_OK) return false;
  value = (uint16_t(bytes[0]) << 8) | bytes[1];
  return true;
}

bool Nau8325::update(uint16_t reg, uint16_t mask, uint16_t value) {
  uint16_t current = 0;
  return read(reg, current) && write(reg, (current & ~mask) | (value & mask));
}

void Nau8325::mute(bool enabled) {
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return;
  update(0x12, kSoftMute, enabled ? kSoftMute : 0);
  xSemaphoreGive(mutex_);
}

bool Nau8325::begin(uint32_t sample_rate) {
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  bool ok = true;
  ok &= write(0x00, 0x0001);
  vTaskDelay(pdMS_TO_TICKS(2));
  ok &= write(0x00, 0x0000);
  vTaskDelay(pdMS_TO_TICKS(2));

  uint16_t rate_bits = 0;
  bool max_div = false;
  switch (sample_rate) {
    case 8000: rate_bits = 0; break;
    case 12000: rate_bits = 0; max_div = true; break;
    case 16000: rate_bits = 1; break;
    case 24000: rate_bits = 1; max_div = true; break;
    case 32000: rate_bits = 2; break;
    case 44100: rate_bits = 2; max_div = true; break;
    case 48000: rate_bits = 2; max_div = true; break;
    case 64000: rate_bits = 3; break;
    case 96000: rate_bits = 3; max_div = true; break;
    default:
      ESP_LOGE(kTag, "unsupported sample rate: %lu", sample_rate);
      xSemaphoreGive(mutex_);
      return false;
  }

  // 256*Fs MCLK, 128x DAC oversampling, 16-bit Philips I2S.
  ok &= update(0x40, (0x7u << 10) | 1u,
               (rate_bits << 10) | (max_div ? 1u : 0u));
  ok &= write(0x03, 0x1000);
  ok &= update(0x29, 0x0007, 0x0002);
  ok &= update(0x0D, (0x3u << 2) | 0x00C3, 0x0002);

  // Analog path configuration from PCB Cupid's NAU8325 reference driver.
  ok &= update(0x2C, 0x00E0, 0x00E0);
  ok &= update(0x2D, 0xFFF0, 0x5350);
  ok &= update(0x2E, 0x8000, 0x8000);
  ok &= update(0x40, 0xE000, 0x8000);
  ok &= update(0x73, 0x000C, 0x0008);
  ok &= update(0x63, 0x000F, 0x0004);
  ok &= update(0x64, 0x00FF, 0x0007);
  ok &= update(0x60, 0x0030, 0x0020);
  ok &= update(0x61, 0xFFFF, 0x5555);
  ok &= update(0x04, 0x000C, 0x000C);
  ok &= update(0x61, 0x0003, 0x0003);
  ok &= update(0x66, 0x0030, 0x0030);
  ok &= write(0x13, 0xFDFD);
  ok &= update(0x12, kSoftMute, 0);
  ok &= update(0x61, 0xFFFF, 0xFFFF);
  ok &= update(0x40, 0x2000, 0x2000);
  xSemaphoreGive(mutex_);
  vTaskDelay(pdMS_TO_TICKS(50));
  ESP_LOGI(kTag, "configured at %lu Hz (%s)", sample_rate, ok ? "ok" : "failed");
  return ok;
}
