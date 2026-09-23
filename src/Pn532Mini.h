#pragma once

#include <Arduino.h>
#include <Wire.h>

class Pn532Mini {
 public:
  explicit Pn532Mini(TwoWire &wire) : wire_(wire) {}
  void begin() {}

  uint32_t getFirmwareVersion() {
    uint8_t reply[8]{};
    const int length = transact(0x02, nullptr, 0, reply, sizeof(reply), 150);
    if (length < 4) return 0;
    return (uint32_t(reply[0]) << 24) | (uint32_t(reply[1]) << 16) |
           (uint32_t(reply[2]) << 8) | reply[3];
  }

  bool SAMConfig() {
    const uint8_t params[] = {0x01, 0x14, 0x01};
    uint8_t reply[4]{};
    return transact(0x14, params, sizeof(params), reply, sizeof(reply), 150) >= 0;
  }

  bool setPassiveActivationRetries(uint8_t retries) {
    const uint8_t params[] = {0x05, 0xFF, retries, retries};
    uint8_t reply[4]{};
    return transact(0x32, params, sizeof(params), reply, sizeof(reply), 150) >= 0;
  }

  bool readPassiveTargetID(uint8_t cardBaudRate, uint8_t *uid,
                           uint8_t *uidLength, uint16_t timeout = 300) {
    const uint8_t params[] = {0x01, cardBaudRate};
    uint8_t reply[24]{};
    const int length = transact(0x4A, params, sizeof(params), reply,
                                sizeof(reply), timeout);
    if (length < 7 || reply[0] != 1) return false;
    const uint8_t lengthFromCard = reply[5];
    if (lengthFromCard == 0 || lengthFromCard > 10 ||
        6 + lengthFromCard > length) return false;
    *uidLength = lengthFromCard;
    memcpy(uid, reply + 6, lengthFromCard);
    return true;
  }

 private:
  static constexpr uint8_t kAddress = 0x24;
  TwoWire &wire_;
  uint8_t command_ = 0;

  bool ready() {
    const int received = wire_.requestFrom(kAddress, uint8_t(1));
    return received == 1 && wire_.available() && (wire_.read() & 0x01);
  }

  bool waitReady(uint16_t timeout) {
    const uint32_t started = millis();
    while (millis() - started <= timeout) {
      if (ready()) return true;
      delay(2);
    }
    return false;
  }

  bool writeCommand(uint8_t command, const uint8_t *params, size_t paramsLength) {
    command_ = command;
    const uint8_t dataLength = uint8_t(paramsLength + 2);
    uint8_t checksum = uint8_t(0xD4 + command);
    wire_.beginTransmission(kAddress);
    wire_.write(uint8_t(0x00));
    wire_.write(uint8_t(0x00));
    wire_.write(uint8_t(0xFF));
    wire_.write(dataLength);
    wire_.write(uint8_t(~dataLength + 1));
    wire_.write(uint8_t(0xD4));
    wire_.write(command);
    for (size_t i = 0; i < paramsLength; ++i) {
      wire_.write(params[i]);
      checksum = uint8_t(checksum + params[i]);
    }
    wire_.write(uint8_t(~checksum + 1));
    wire_.write(uint8_t(0x00));
    if (wire_.endTransmission() != 0 || !waitReady(120)) return false;
    uint8_t ack[7]{};
    if (wire_.requestFrom(kAddress, uint8_t(sizeof(ack))) != sizeof(ack)) return false;
    for (uint8_t &value : ack) value = wire_.read();
    return ack[0] == 0x01 && ack[1] == 0x00 && ack[2] == 0x00 &&
           ack[3] == 0xFF && ack[4] == 0x00 && ack[5] == 0xFF;
  }

  int readResponse(uint8_t *out, size_t capacity, uint16_t timeout) {
    if (!waitReady(timeout)) return -1;
    constexpr uint8_t kFrameReadSize = 40;
    uint8_t frame[kFrameReadSize]{};
    const int received = wire_.requestFrom(kAddress, kFrameReadSize);
    if (received < 9) return -2;
    const int count = min(received, int(kFrameReadSize));
    for (int i = 0; i < count; ++i) frame[i] = wire_.read();
    if (!(frame[0] & 1) || frame[1] != 0x00 || frame[2] != 0x00 ||
        frame[3] != 0xFF) return -3;
    const uint8_t payloadLength = frame[4];
    if (uint8_t(payloadLength + frame[5]) != 0 || payloadLength < 2 ||
        payloadLength + 8 > count || frame[6] != 0xD5 ||
        frame[7] != uint8_t(command_ + 1)) return -4;
    const size_t dataLength = payloadLength - 2;
    if (dataLength > capacity) return -5;
    memcpy(out, frame + 8, dataLength);
    return int(dataLength);
  }

  int transact(uint8_t command, const uint8_t *params, size_t paramsLength,
               uint8_t *reply, size_t replyCapacity, uint16_t timeout) {
    if (!writeCommand(command, params, paramsLength)) return -1;
    return readResponse(reply, replyCapacity, timeout);
  }
};
