#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <PCBCUPID_PLAYER.h>
#include <SD.h>

#include <algorithm>

#include "Pn532Mini.h"

namespace {

constexpr uint8_t kI2cSda = 4;
constexpr uint8_t kI2cScl = 5;
constexpr uint8_t kSdCs = 17;
constexpr uint8_t kSdMiso = 23;
constexpr uint8_t kSdMosi = 22;
constexpr uint8_t kSdSck = 21;
constexpr uint8_t kI2sMclk = 16;
// Upstream SoundPod sketch ordering (the prose documentation differs).
constexpr uint8_t kI2sBclk = 14;
constexpr uint8_t kI2sWs = 18;
constexpr uint8_t kI2sDout = 15;
constexpr uint32_t kNfcPollMs = 90;
constexpr uint32_t kCardRemovalMs = 450;
constexpr size_t kMaxPlaylistTracks = 32;

constexpr char kBleDeviceName[] = "Glyph Soundbox";
constexpr char kServiceUuid[] = "7e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kCommandUuid[] = "7e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kEventsUuid[] = "7e400003-b5a3-f393-e0a9-e50e24dcca9e";

AudioPlayerConfig playerConfig = {
    .sd_cs = kSdCs,
    .sd_miso = kSdMiso,
    .sd_mosi = kSdMosi,
    .sd_sck = kSdSck,
    .i2s_mclk = kI2sMclk,
    .i2s_bclk = kI2sBclk,
    .i2s_ws = kI2sWs,
    .i2s_dout = kI2sDout,
    .audio_start_path = "/",
};

AUDIO_PLAYER *player = nullptr;
Pn532Mini nfc(Wire);
Preferences prefs;
NimBLECharacteristic *eventsCharacteristic = nullptr;
SemaphoreHandle_t playerMutex = nullptr;

struct Playlist {
  uint16_t tracks[kMaxPlaylistTracks]{};
  uint8_t count = 0;
};

struct ResumeState {
  uint8_t playlistPosition = 0;
  uint32_t seconds = 0;
};

String activeUid;
Playlist activePlaylist;
uint8_t activePlaylistPosition = 0;
uint32_t resumeBaseSeconds = 0;
uint32_t lastCardSeenAt = 0;
bool cardPlaybackActive = false;
bool sdReady = false;
bool nfcReady = false;
bool bleConnected = false;

uint32_t hashUid(const String &uid) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < uid.length(); ++i) {
    hash ^= static_cast<uint8_t>(uid[i]);
    hash *= 16777619UL;
  }
  return hash;
}

String preferenceKey(char prefix, const String &uid) {
  char key[12];
  snprintf(key, sizeof(key), "%c%08lX", prefix,
           static_cast<unsigned long>(hashUid(uid)));
  return String(key);
}

String uidToString(const uint8_t *uid, uint8_t length) {
  String result;
  result.reserve(length * 2);
  for (uint8_t i = 0; i < length; ++i) {
    if (uid[i] < 0x10) result += '0';
    result += String(uid[i], HEX);
  }
  result.toUpperCase();
  return result;
}

bool uidIsKnown(const String &uid) {
  const String all = prefs.getString("uids", "");
  int start = 0;
  while (start < static_cast<int>(all.length())) {
    int end = all.indexOf(',', start);
    if (end < 0) end = all.length();
    if (all.substring(start, end) == uid) return true;
    start = end + 1;
  }
  return false;
}

void rememberUid(const String &uid) {
  if (uidIsKnown(uid)) return;
  String all = prefs.getString("uids", "");
  if (!all.isEmpty()) all += ',';
  all += uid;
  prefs.putString("uids", all);
  prefs.putString(preferenceKey('u', uid).c_str(), uid);
}

String playlistToCsv(const Playlist &playlist) {
  String csv;
  for (uint8_t i = 0; i < playlist.count; ++i) {
    if (i) csv += ',';
    csv += String(playlist.tracks[i]);
  }
  return csv;
}

Playlist parsePlaylist(const String &csv) {
  Playlist playlist;
  int start = 0;
  while (start < static_cast<int>(csv.length()) &&
         playlist.count < kMaxPlaylistTracks) {
    int end = csv.indexOf(',', start);
    if (end < 0) end = csv.length();
    const String item = csv.substring(start, end);
    const int index = item.toInt();
    if (!item.isEmpty() && index >= 0 && index < player->trackCount()) {
      playlist.tracks[playlist.count++] = static_cast<uint16_t>(index);
    }
    start = end + 1;
  }
  return playlist;
}

Playlist loadPlaylist(const String &uid) {
  return parsePlaylist(prefs.getString(preferenceKey('m', uid).c_str(), ""));
}

void savePlaylist(const String &uid, const Playlist &playlist) {
  prefs.putString(preferenceKey('m', uid).c_str(), playlistToCsv(playlist));
  rememberUid(uid);
}

ResumeState loadResume(const String &uid) {
  ResumeState state;
  const String value = prefs.getString(preferenceKey('r', uid).c_str(), "");
  const int comma = value.indexOf(',');
  if (comma >= 0) {
    state.playlistPosition = value.substring(0, comma).toInt();
    state.seconds = value.substring(comma + 1).toInt();
  }
  return state;
}

void saveResume(const String &uid, uint8_t playlistPosition, uint32_t seconds) {
  prefs.putString(preferenceKey('r', uid).c_str(),
                  String(playlistPosition) + ',' + String(seconds));
}

void sendBle(const String &message) {
  Serial.printf("BLE> %s\n", message.c_str());
  if (!bleConnected || eventsCharacteristic == nullptr) return;
  eventsCharacteristic->setValue(message.c_str());
  eventsCharacteristic->notify();
  delay(12);
}

void sendStatus() {
  String status = "STATUS|";
  status += sdReady ? "SD_OK" : "SD_ERROR";
  status += '|';
  status += nfcReady ? "NFC_OK" : "NFC_ERROR";
  status += '|';
  status += String(player->trackCount());
  status += '|';
  status += activeUid.isEmpty() ? "NO_CARD" : activeUid;
  sendBle(status);
}

void sendTracks() {
  sendBle("TRACKS_BEGIN|" + String(player->trackCount()));
  for (int i = 0; i < player->trackCount(); ++i) {
    sendBle("TRACK|" + String(i) + '|' + player->getTrackNameAt(i));
  }
  sendBle("TRACKS_END");
}

void sendMappings() {
  sendBle("MAPS_BEGIN");
  const String all = prefs.getString("uids", "");
  int start = 0;
  while (start < static_cast<int>(all.length())) {
    int end = all.indexOf(',', start);
    if (end < 0) end = all.length();
    const String uid = all.substring(start, end);
    if (!uid.isEmpty()) {
      sendBle("MAP|" + uid + '|' +
              prefs.getString(preferenceKey('m', uid).c_str(), ""));
    }
    start = end + 1;
  }
  sendBle("MAPS_END");
}

void handleBleCommand(String command) {
  command.trim();
  Serial.printf("BLE< %s\n", command.c_str());
  if (command == "HELLO") {
    sendBle("INFO|Glyph Soundbox|0.1.0");
  } else if (command == "STATUS") {
    sendStatus();
  } else if (command == "TRACKS") {
    sendTracks();
  } else if (command == "MAPS") {
    sendMappings();
  } else if (command.startsWith("MAP|")) {
    const int secondPipe = command.indexOf('|', 4);
    if (secondPipe < 0) return sendBle("ERROR|MAP_FORMAT");
    String uid = command.substring(4, secondPipe);
    uid.toUpperCase();
    const Playlist playlist = parsePlaylist(command.substring(secondPipe + 1));
    if (uid.isEmpty() || playlist.count == 0) return sendBle("ERROR|EMPTY_MAP");
    savePlaylist(uid, playlist);
    sendBle("SAVED|" + uid + '|' + playlistToCsv(playlist));
  } else if (command.startsWith("CLEAR|")) {
    String uid = command.substring(6);
    uid.toUpperCase();
    prefs.remove(preferenceKey('m', uid).c_str());
    prefs.remove(preferenceKey('r', uid).c_str());
    sendBle("CLEARED|" + uid);
  } else {
    sendBle("ERROR|UNKNOWN_COMMAND");
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    bleConnected = true;
    Serial.println("BLE client connected");
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &, int) override {
    bleConnected = false;
    Serial.println("BLE client disconnected");
    server->startAdvertising();
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    handleBleCommand(String(characteristic->getValue().c_str()));
  }
};

void beginBle() {
  NimBLEDevice::init(kBleDeviceName);
  NimBLEDevice::setMTU(185);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *service = server->createService(kServiceUuid);
  NimBLECharacteristic *commands = service->createCharacteristic(
      kCommandUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  commands->setCallbacks(new CommandCallbacks());
  eventsCharacteristic = service->createCharacteristic(
      kEventsUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  eventsCharacteristic->setValue("BOOTING");
  service->start();
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setName(kBleDeviceName);
  advertising->start();
  Serial.println("BLE advertising as 'Glyph Soundbox'");
}

Playlist defaultPlaylist() {
  Playlist playlist;
  const int count = std::min(player->trackCount(), static_cast<int>(kMaxPlaylistTracks));
  for (int i = 0; i < count; ++i) playlist.tracks[playlist.count++] = i;
  return playlist;
}

bool seekPlayerToSecondsLocked(uint32_t seconds) {
  if (seconds == 0 || player->audioPlayer() == nullptr) return true;
  const uint32_t duration = player->currentTrackDurationSeconds();
  if (duration == 0 || seconds >= duration) return false;
  Stream *stream = player->audioPlayer()->getStream();
  if (stream == nullptr) return false;
  File *file = static_cast<File *>(stream);
  const size_t size = file->size();
  if (size < 2) return false;
  const size_t offset = static_cast<size_t>(
      (static_cast<uint64_t>(size) * seconds) / duration);
  player->audioPlayer()->stop();
  if (!file->seek(std::min(offset, size - 1))) return false;
  player->audioPlayer()->setStream(file);
  player->audioPlayer()->play();
  Serial.printf("Resume seek: %lu sec -> byte %u/%u\n",
                static_cast<unsigned long>(seconds), static_cast<unsigned>(offset),
                static_cast<unsigned>(size));
  return true;
}

bool playPlaylistPositionLocked(uint8_t position, uint32_t seconds) {
  if (activePlaylist.count == 0) return false;
  activePlaylistPosition = position % activePlaylist.count;
  const int globalTrack = activePlaylist.tracks[activePlaylistPosition];
  if (!player->playTrackIndex(globalTrack)) return false;
  resumeBaseSeconds = seconds;
  if (!seekPlayerToSecondsLocked(seconds)) resumeBaseSeconds = 0;
  Serial.printf("Playing card %s, item %u/%u: %s\n", activeUid.c_str(),
                activePlaylistPosition + 1, activePlaylist.count,
                player->currentTrackName().c_str());
  sendBle("PLAYING|" + activeUid + '|' + String(activePlaylistPosition) + '|' +
          String(globalTrack) + '|' + String(resumeBaseSeconds));
  return true;
}

void advancePlaylistLocked() {
  if (!cardPlaybackActive || activePlaylist.count == 0) return;
  activePlaylistPosition = (activePlaylistPosition + 1) % activePlaylist.count;
  resumeBaseSeconds = 0;
  saveResume(activeUid, activePlaylistPosition, 0);
  playPlaylistPositionLocked(activePlaylistPosition, 0);
}

void startCard(const String &uid) {
  activeUid = uid;
  activePlaylist = loadPlaylist(uid);
  if (activePlaylist.count == 0 && player->trackCount() > 0) {
    activePlaylist = defaultPlaylist();
    savePlaylist(uid, activePlaylist);
    sendBle("AUTO_MAPPED|" + uid + '|' + playlistToCsv(activePlaylist));
  }
  sendBle("CARD|" + uid);
  Serial.printf("Card present: %s\n", uid.c_str());
  if (activePlaylist.count == 0) {
    sendBle("UNMAPPED|" + uid);
    return;
  }
  ResumeState resume = loadResume(uid);
  if (resume.playlistPosition >= activePlaylist.count) resume = {};
  if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    cardPlaybackActive = true;
    if (!playPlaylistPositionLocked(resume.playlistPosition, resume.seconds)) {
      cardPlaybackActive = false;
    }
    xSemaphoreGive(playerMutex);
  }
}

void stopCard() {
  if (activeUid.isEmpty()) return;
  const String removedUid = activeUid;
  if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    cardPlaybackActive = false;
    const uint32_t elapsed = resumeBaseSeconds + player->currentTrackElapsedSeconds();
    saveResume(removedUid, activePlaylistPosition, elapsed);
    player->pause();
    xSemaphoreGive(playerMutex);
    Serial.printf("Card removed: %s, saved item %u at %lu sec\n",
                  removedUid.c_str(), activePlaylistPosition + 1,
                  static_cast<unsigned long>(elapsed));
    sendBle("STOPPED|" + removedUid + '|' + String(activePlaylistPosition) + '|' +
            String(elapsed));
  }
  activeUid = "";
  activePlaylist = {};
  activePlaylistPosition = 0;
  resumeBaseSeconds = 0;
}

void audioTask(void *) {
  while (true) {
    if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      player->loop();
      if (cardPlaybackActive && player->audioPlayer() != nullptr &&
          !player->audioPlayer()->isActive()) {
        advancePlaylistLocked();
      }
      xSemaphoreGive(playerMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void pollNfc() {
  static uint32_t lastPollAt = 0;
  const uint32_t now = millis();
  if (!nfcReady || now - lastPollAt < kNfcPollMs) return;
  lastPollAt = now;
  uint8_t uid[10]{};
  uint8_t uidLength = 0;
  const bool found = nfc.readPassiveTargetID(0x00, uid, &uidLength, 250);
  if (found) {
    const String seenUid = uidToString(uid, uidLength);
    lastCardSeenAt = now;
    if (activeUid != seenUid) {
      if (!activeUid.isEmpty()) stopCard();
      startCard(seenUid);
    }
  } else if (!activeUid.isEmpty() && now - lastCardSeenAt >= kCardRemovalMs) {
    stopCard();
  }
}

void scanI2c() {
  Serial.println("I2C devices:");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X%s\n", address,
                    address == 0x24 ? " (PN532)" : "");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(3500);  // Leave time for USB CDC and a serial monitor to attach.
  Serial.println("\n=== Glyph Soundbox NFC + BLE POC 0.1.0 ===");
  prefs.begin("yotopoc", false);
  playerMutex = xSemaphoreCreateMutex();
  Wire.setBufferSize(300);
  Wire.begin(kI2cSda, kI2cScl, 100000);
  nfc.begin();
  scanI2c();
  const uint32_t pn532Version = nfc.getFirmwareVersion();
  nfcReady = pn532Version != 0;
  if (nfcReady) {
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0x01);
    Serial.printf("PN532 ready, firmware %lu.%lu\n", (pn532Version >> 16) & 0xFF,
                  (pn532Version >> 8) & 0xFF);
  } else {
    Serial.println("ERROR: PN532 not found at I2C address 0x24");
  }
  beginBle();
  Serial.println("Starting Soundbox audio player...");
  player = new AUDIO_PLAYER(Wire, playerConfig);
  player->begin("mp3");
  player->stop();
  player->setVolume(0.55f);
  sdReady = player->trackCount() > 0 || SD.cardType() != CARD_NONE;
  Serial.printf("SD %s; %d MP3 track(s) found\n", sdReady ? "ready" : "not available",
                player->trackCount());
  xTaskCreate(audioTask, "audio", 6144, nullptr, 3, nullptr);
  Serial.println("READY: tap a card or connect the PWA over Bluetooth.");
  sendStatus();
}

void loop() {
  pollNfc();
  delay(2);
}
