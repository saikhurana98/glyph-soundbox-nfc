#include <Arduino.h>
#include <Wire.h>

#include "Pn532Mini.h"

namespace {

constexpr uint8_t kSda = 4;
constexpr uint8_t kScl = 5;
constexpr uint8_t kPn532Address = 0x24;
Pn532Mini nfc(Wire);
bool readerReady = false;
uint32_t lastScanAt = 0;
uint32_t readCount = 0;

String uidText(const uint8_t *uid, uint8_t length) {
  String result;
  for (uint8_t i = 0; i < length; ++i) {
    if (uid[i] < 0x10) result += '0';
    result += String(uid[i], HEX);
  }
  result.toUpperCase();
  return result;
}

bool addressResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool probeOrientation(uint8_t sda, uint8_t scl, const char *label) {
  Wire.end();
  delay(20);
  Wire.begin(sda, scl, 100000);
  delay(20);
  const int sdaLevel = digitalRead(sda);
  const int sclLevel = digitalRead(scl);
  const bool acknowledged = addressResponds(kPn532Address);
  Serial.printf("BUS %s | SDA GPIO%u=%s | SCL GPIO%u=%s | 0x24=%s\n",
                label, sda, sdaLevel ? "HIGH" : "LOW", scl,
                sclLevel ? "HIGH" : "LOW", acknowledged ? "ACK" : "NO ACK");
  return acknowledged;
}

void scanAndInitialize() {
  bool acknowledged = probeOrientation(kSda, kScl, "normal");
  if (!acknowledged) acknowledged = probeOrientation(kScl, kSda, "swapped");
  if (!acknowledged) {
    readerReady = false;
    // Leave the bus in the documented orientation for the next retry.
    Wire.end();
    Wire.begin(kSda, kScl, 100000);
    Serial.println("PN532 FAIL: powered but no I2C ACK in either wire orientation; verify I2C mode selection.");
    return;
  }

  const uint32_t version = nfc.getFirmwareVersion();
  if (version == 0) {
    readerReady = false;
    Serial.println("PN532 FAIL: I2C ACK received, but firmware command failed.");
    return;
  }
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(0x01);
  readerReady = true;
  Serial.printf("PN532 READY: firmware %lu.%lu — tap cards now\n",
                (version >> 16) & 0xFF, (version >> 8) & 0xFF);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== GLYPH PN532 CARD DIAGNOSTIC ===");
  Serial.println("Pins: SDA=4 SCL=5, expected PN532 I2C address=0x24");
  Wire.setBufferSize(300);
  Wire.begin(kSda, kScl, 100000);
  nfc.begin();
  scanAndInitialize();
}

void loop() {
  const uint32_t now = millis();
  if (!readerReady) {
    if (now - lastScanAt >= 1000) {
      lastScanAt = now;
      scanAndInitialize();
    }
    delay(10);
    return;
  }

  uint8_t uid[10]{};
  uint8_t uidLength = 0;
  if (nfc.readPassiveTargetID(0x00, uid, &uidLength, 250)) {
    ++readCount;
    Serial.printf("CARD #%lu | UID=%s | bytes=%u\n",
                  static_cast<unsigned long>(readCount),
                  uidText(uid, uidLength).c_str(), uidLength);
  }

  // Confirm that the module has not fallen off the bus during testing.
  if (now - lastScanAt >= 2000) {
    lastScanAt = now;
    if (!addressResponds(kPn532Address)) {
      readerReady = false;
      Serial.println("PN532 LOST: address 0x24 stopped acknowledging.");
    }
  }
  delay(20);
}
