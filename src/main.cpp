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
constexpr uint32_t kNfcReconnectMs = 2000;
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
uint32_t trackStartedAt = 0;
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

String cardLabel(const String &uid) {
  const String all = prefs.getString("uids", "");
  int start = 0;
  int number = 1;
  while (start < static_cast<int>(all.length())) {
    int end = all.indexOf(',', start);
    if (end < 0) end = all.length();
    if (all.substring(start, end) == uid) return "Card " + String(number);
    start = end + 1;
    ++number;
  }
  return "Card " + String(number);
}

void selectCard(const String &uid);
void playSelectedCard();
void pausePlayback();
void playTrackFromWeb(int trackIndex);

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
              prefs.getString(preferenceKey('m', uid).c_str(), "") + '|' +
              cardLabel(uid));
    }
    start = end + 1;
  }
  sendBle("MAPS_END");
}

void handleBleCommand(String command) {
  command.trim();
  Serial.printf("BLE< %s\n", command.c_str());
  if (command == "HELLO") {
    sendBle("INFO|Glyph Soundbox|0.2.0");
  } else if (command == "STATUS") {
    sendStatus();
  } else if (command == "TRACKS") {
    sendTracks();
  } else if (command == "MAPS") {
    sendMappings();
  } else if (command == "PLAY") {
    playSelectedCard();
  } else if (command == "PAUSE") {
    pausePlayback();
  } else if (command.startsWith("PLAY_TRACK|")) {
    playTrackFromWeb(command.substring(11).toInt());
  } else if (command.startsWith("SELECT|")) {
    String uid = command.substring(7);
    uid.toUpperCase();
    if (uidIsKnown(uid)) selectCard(uid);
  } else if (command.startsWith("MAP|")) {
    const int secondPipe = command.indexOf('|', 4);
    if (secondPipe < 0) return sendBle("ERROR|MAP_FORMAT");
    String uid = command.substring(4, secondPipe);
    uid.toUpperCase();
    const Playlist playlist = parsePlaylist(command.substring(secondPipe + 1));
    if (uid.isEmpty() || playlist.count == 0) return sendBle("ERROR|EMPTY_MAP");
    savePlaylist(uid, playlist);
    if (uid == activeUid) activePlaylist = playlist;
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
  AudioPlayer *core = player->audioPlayer();
  if (core == nullptr) return false;
  String path = player->getTrackNameAt(globalTrack);
  while (path.startsWith("//")) path.remove(0, 1);
  player->trackIndex = globalTrack;
  core->stop();
  if (!core->setPath(path.c_str())) {
    Serial.printf("ERROR: failed to open track %s\n", path.c_str());
    return false;
  }
  core->play();
  trackStartedAt = millis();
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

void pausePlayback() {
  if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    uint32_t elapsed = resumeBaseSeconds;
    if (cardPlaybackActive && !activeUid.isEmpty()) {
      elapsed += (millis() - trackStartedAt) / 1000;
      saveResume(activeUid, activePlaylistPosition, elapsed);
    }
    if (player->audioPlayer() != nullptr) player->audioPlayer()->stop();
    cardPlaybackActive = false;
    xSemaphoreGive(playerMutex);
    Serial.printf("Playback paused at item %u, %lu sec\n",
                  activePlaylistPosition + 1, static_cast<unsigned long>(elapsed));
    sendBle("PAUSED|" + activeUid + '|' + String(activePlaylistPosition) + '|' +
            String(elapsed));
  }
}

void selectCard(const String &uid) {
  if (uid.isEmpty()) return;
  if (player->audioPlayer() != nullptr && player->audioPlayer()->isActive()) {
    pausePlayback();
  }
  rememberUid(uid);
  activeUid = uid;
  activePlaylist = loadPlaylist(uid);
  activePlaylistPosition = 0;
  resumeBaseSeconds = 0;
  Serial.printf("Selected %s: %s\n", cardLabel(uid).c_str(), uid.c_str());
  sendBle("CARD|" + uid + '|' + cardLabel(uid));
  if (activePlaylist.count == 0) sendBle("UNMAPPED|" + uid);
}

void playSelectedCard() {
  if (activeUid.isEmpty()) return sendBle("ERROR|SELECT_A_CARD");
  activePlaylist = loadPlaylist(activeUid);
  if (activePlaylist.count == 0) return sendBle("ERROR|MAP_A_PLAYLIST");
  ResumeState resume = loadResume(activeUid);
  if (resume.playlistPosition >= activePlaylist.count) resume = {};
  if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    cardPlaybackActive = true;
    if (!playPlaylistPositionLocked(resume.playlistPosition, resume.seconds)) {
      cardPlaybackActive = false;
    }
    xSemaphoreGive(playerMutex);
  }
}

void playTrackFromWeb(int trackIndex) {
  if (trackIndex < 0 || trackIndex >= player->trackCount()) {
    return sendBle("ERROR|TRACK_INDEX");
  }
  if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  cardPlaybackActive = false;
  AudioPlayer *core = player->audioPlayer();
  if (core == nullptr) {
    xSemaphoreGive(playerMutex);
    return sendBle("ERROR|AUDIO_NOT_READY");
  }
  String path = player->getTrackNameAt(trackIndex);
  while (path.startsWith("//")) path.remove(0, 1);
  player->trackIndex = trackIndex;
  core->stop();
  const bool opened = core->setPath(path.c_str());
  if (opened) core->play();
  xSemaphoreGive(playerMutex);
  if (opened) {
    Serial.printf("Web preview: %s\n", path.c_str());
    sendBle("PLAYING_TRACK|" + String(trackIndex));
  } else {
    sendBle("ERROR|OPEN_TRACK");
  }
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

bool initializeNfc() {
  const uint32_t version = nfc.getFirmwareVersion();
  if (version == 0) return false;
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(0x01);
  Serial.printf("PN532 ready, firmware %lu.%lu\n", (version >> 16) & 0xFF,
                (version >> 8) & 0xFF);
  return true;
}

void pollNfc() {
  static uint32_t lastPollAt = 0;
  static uint32_t lastReconnectAt = 0;
  const uint32_t now = millis();
  if (!nfcReady) {
    if (now - lastReconnectAt >= kNfcReconnectMs) {
      lastReconnectAt = now;
      nfcReady = initializeNfc();
      if (nfcReady) {
        Serial.println("PN532 recovered after startup");
        sendStatus();
      }
    }
    return;
  }
  if (now - lastPollAt < kNfcPollMs) return;
  lastPollAt = now;
  uint8_t uid[10]{};
  uint8_t uidLength = 0;
  const bool found = nfc.readPassiveTargetID(0x00, uid, &uidLength, 250);
  if (found) {
    const String seenUid = uidToString(uid, uidLength);
    if (seenUid != activeUid) selectCard(seenUid);
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
  Serial.println("\n=== Glyph Soundbox NFC + BLE POC 0.2.0 ===");
  prefs.begin("yotopoc", false);
  playerMutex = xSemaphoreCreateMutex();
  Wire.setBufferSize(300);
  Wire.begin(kI2cSda, kI2cScl, 100000);
  nfc.begin();
  scanI2c();
  nfcReady = initializeNfc();
  if (!nfcReady) Serial.println("ERROR: PN532 not found; retrying every 2 seconds");
  beginBle();
  Serial.println("Starting Soundbox audio player...");
  player = new AUDIO_PLAYER(Wire, playerConfig);
  player->begin("mp3");
  player->stop();
  player->setVolume(0.55f);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Error);
  for (String &path : player->trackList) {
    while (path.startsWith("//")) path.remove(0, 1);
  }
  player->trackList.erase(
      std::remove_if(player->trackList.begin(), player->trackList.end(),
                     [](const String &path) {
                       const int slash = path.lastIndexOf('/');
                       return path.substring(slash + 1).startsWith("._");
                     }),
      player->trackList.end());
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
