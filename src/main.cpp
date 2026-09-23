#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "NimBLEDevice.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_mp3/mp3_decoder.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#include "nau8325.h"
#include "pn532.h"

namespace {

constexpr char kTag[] = "glyph";
constexpr char kVersion[] = "0.8.0-idf";
constexpr gpio_num_t kI2cSda = GPIO_NUM_4;
constexpr gpio_num_t kI2cScl = GPIO_NUM_5;
constexpr gpio_num_t kSdCs = GPIO_NUM_17;
constexpr gpio_num_t kSdSck = GPIO_NUM_21;
constexpr gpio_num_t kSdMosi = GPIO_NUM_22;
constexpr gpio_num_t kSdMiso = GPIO_NUM_23;
constexpr gpio_num_t kI2sMclk = GPIO_NUM_16;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_14;
constexpr gpio_num_t kI2sWs = GPIO_NUM_18;
constexpr gpio_num_t kI2sDout = GPIO_NUM_15;
constexpr size_t kMaxPlaylistTracks = 32;

constexpr char kBleDeviceName[] = "Glyph Soundbox";
constexpr char kServiceUuid[] = "7e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kCommandUuid[] = "7e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kEventsUuid[] = "7e400003-b5a3-f393-e0a9-e50e24dcca9e";

extern const uint8_t card_tap_start[] asm("_binary_card_tap_mp3_start");
extern const uint8_t card_tap_end[] asm("_binary_card_tap_mp3_end");

struct Playlist {
  uint16_t tracks[kMaxPlaylistTracks]{};
  uint8_t count = 0;
};

struct ResumeState {
  uint8_t playlist_position = 0;
  uint32_t seconds = 0;
};

enum class AudioAction : uint8_t { kStop, kFile, kMemory };
enum class TapMode : uint8_t { kPlayWhilePresent, kTogglePlayPause };

struct AudioRequest {
  AudioAction action = AudioAction::kStop;
  char path[192]{};
  const uint8_t* memory = nullptr;
  size_t memory_size = 0;
  uint32_t start_seconds = 0;
};

struct BlePacket {
  uint16_t length = 0;
  uint8_t data[184]{};
};

i2c_master_bus_handle_t g_i2c_bus = nullptr;
SemaphoreHandle_t g_i2c_mutex = nullptr;
QueueHandle_t g_audio_queue = nullptr;
QueueHandle_t g_ble_queue = nullptr;
i2s_chan_handle_t g_i2s_tx = nullptr;
Pn532* g_nfc = nullptr;
Nau8325* g_amplifier = nullptr;
nvs_handle_t g_nvs = 0;
sdmmc_card_t* g_sd_card = nullptr;
NimBLECharacteristic* g_events = nullptr;

std::vector<std::string> g_tracks;
std::string g_active_uid;
std::string g_present_uid;
Playlist g_active_playlist;
bool g_active_shuffle = false;
uint8_t g_playlist_position = 0;
uint32_t g_resume_base_seconds = 0;
int64_t g_track_started_us = 0;
std::atomic<bool> g_card_playback{false};
std::atomic<bool> g_audio_playing{false};
std::atomic<bool> g_upload_active{false};
std::atomic<bool> g_ble_connected{false};
std::atomic<bool> g_sd_ready{false};
std::atomic<bool> g_nfc_ready{false};
std::atomic<TapMode> g_tap_mode{TapMode::kPlayWhilePresent};
std::atomic<bool> g_resume_enabled{true};
std::atomic<bool> g_play_after_tap{false};
std::atomic<uint8_t> g_volume{75};
std::atomic<uint32_t> g_audio_work_us{0};
std::atomic<uint32_t> g_audio_pcm_us{0};
std::atomic<uint32_t> g_audio_max_frame_us{0};
std::atomic<uint32_t> g_audio_frames{0};
std::atomic<uint32_t> g_audio_short_writes{0};
bool g_session_paused = false;

FILE* g_upload_file = nullptr;
std::string g_upload_name;
uint32_t g_upload_expected = 0;
uint32_t g_upload_received = 0;

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return value;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return value;
}

uint32_t uid_hash(const std::string& uid) {
  uint32_t hash = 2166136261UL;
  for (const uint8_t c : uid) {
    hash ^= c;
    hash *= 16777619UL;
  }
  return hash;
}

std::string preference_key(char prefix, const std::string& uid) {
  char key[12];
  std::snprintf(key, sizeof(key), "%c%08lX", prefix,
                static_cast<unsigned long>(uid_hash(uid)));
  return key;
}

std::string nvs_get_string(const char* key) {
  size_t length = 0;
  if (nvs_get_str(g_nvs, key, nullptr, &length) != ESP_OK || length == 0) return {};
  std::vector<char> value(length);
  if (nvs_get_str(g_nvs, key, value.data(), &length) != ESP_OK) return {};
  return std::string(value.data());
}

void nvs_set_string(const char* key, const std::string& value) {
  if (nvs_set_str(g_nvs, key, value.c_str()) == ESP_OK) nvs_commit(g_nvs);
}

std::vector<std::string> split(const std::string& text, char delimiter) {
  std::vector<std::string> values;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find(delimiter, start);
    values.emplace_back(text.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return values;
}

bool uid_known(const std::string& uid) {
  const auto ids = split(nvs_get_string("uids"), ',');
  return std::find(ids.begin(), ids.end(), uid) != ids.end();
}

void remember_uid(const std::string& uid) {
  if (uid.empty() || uid_known(uid)) return;
  std::string ids = nvs_get_string("uids");
  if (!ids.empty()) ids += ',';
  ids += uid;
  nvs_set_string("uids", ids);
  nvs_set_string(preference_key('u', uid).c_str(), uid);
}

std::string card_label(const std::string& uid) {
  const std::string custom = nvs_get_string(preference_key('n', uid).c_str());
  if (!custom.empty()) return custom;
  int number = 1;
  for (const auto& candidate : split(nvs_get_string("uids"), ',')) {
    if (candidate == uid) return "Card " + std::to_string(number);
    ++number;
  }
  return "Card " + std::to_string(number);
}

std::string playlist_csv(const Playlist& playlist) {
  std::string csv;
  for (uint8_t i = 0; i < playlist.count; ++i) {
    if (i) csv += ',';
    csv += std::to_string(playlist.tracks[i]);
  }
  return csv;
}

Playlist parse_playlist(const std::string& csv) {
  Playlist playlist;
  for (const auto& item : split(csv, ',')) {
    if (item.empty() || playlist.count >= kMaxPlaylistTracks) continue;
    char* end = nullptr;
    const long index = std::strtol(item.c_str(), &end, 10);
    if (*end == '\0' && index >= 0 && index < static_cast<long>(g_tracks.size())) {
      playlist.tracks[playlist.count++] = static_cast<uint16_t>(index);
    }
  }
  return playlist;
}

Playlist load_playlist(const std::string& uid) {
  return parse_playlist(nvs_get_string(preference_key('m', uid).c_str()));
}

void save_playlist(const std::string& uid, const Playlist& playlist) {
  nvs_set_string(preference_key('m', uid).c_str(), playlist_csv(playlist));
  remember_uid(uid);
}

bool load_shuffle(const std::string& uid) {
  uint8_t shuffle = 0;
  nvs_get_u8(g_nvs, preference_key('s', uid).c_str(), &shuffle);
  return shuffle != 0;
}

void save_shuffle(const std::string& uid, bool shuffle) {
  nvs_set_u8(g_nvs, preference_key('s', uid).c_str(), shuffle ? 1 : 0);
  if (shuffle) nvs_erase_key(g_nvs, preference_key('r', uid).c_str());
  nvs_commit(g_nvs);
}

ResumeState load_resume(const std::string& uid) {
  ResumeState state;
  const auto parts = split(nvs_get_string(preference_key('r', uid).c_str()), ',');
  if (parts.size() == 2) {
    state.playlist_position = static_cast<uint8_t>(std::strtoul(parts[0].c_str(), nullptr, 10));
    state.seconds = std::strtoul(parts[1].c_str(), nullptr, 10);
  }
  return state;
}

void save_resume(const std::string& uid, uint8_t position, uint32_t seconds) {
  nvs_set_string(preference_key('r', uid).c_str(),
                 std::to_string(position) + ',' + std::to_string(seconds));
}

bool hidden_path(const std::string& path) {
  for (auto part : split(lower(path), '/')) {
    if (!part.empty() && (part.front() == '.' || part.front() == '_' || part == "meta" ||
                          part == "metadata" || part == "__macosx" || part == "sound-effects" ||
                          part == "sound_effects" || part == "sound effects" || part == "sfx" ||
                          part == "card-tap.mp3")) return true;
  }
  return false;
}

std::string safe_upload_name(std::string name) {
  std::replace(name.begin(), name.end(), '\\', '/');
  if (const auto slash = name.find_last_of('/'); slash != std::string::npos) name.erase(0, slash + 1);
  const bool is_mp3 = lower(name).ends_with(".mp3");
  std::string safe;
  for (const unsigned char c : name) {
    if (safe.size() == 96) break;
    safe += std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ' ' ? c : '_';
  }
  if (is_mp3 && !lower(safe).ends_with(".mp3")) {
    if (safe.size() > 92) safe.resize(92);
    safe += ".mp3";
  }
  if (!safe.empty() && safe.front() == '.') safe = "track" + safe;
  return safe;
}

void send_ble(const std::string& message) {
  ESP_LOGI(kTag, "BLE> %s", message.c_str());
  if (!g_ble_connected || !g_events) return;
  g_events->setValue(message);
  if (!g_events->notify()) ESP_LOGW(kTag, "notification failed: %s", message.c_str());
  vTaskDelay(pdMS_TO_TICKS(45));
}

void send_status() {
  send_ble(std::string("STATUS|") + (g_sd_ready ? "SD_OK" : "SD_ERROR") + '|' +
           (g_nfc_ready ? "NFC_OK" : "NFC_ERROR") + '|' +
           std::to_string(g_tracks.size()) + '|' +
           (g_active_uid.empty() ? "NO_CARD" : g_active_uid));
}

void send_volume() {
  send_ble("VOLUME|" + std::to_string(g_volume.load()));
}

void send_tap_mode() {
  send_ble(std::string("TAP_MODE|") +
           (g_tap_mode == TapMode::kTogglePlayPause ? "toggle" : "presence"));
}

void send_resume_setting() {
  send_ble(std::string("RESUME|") + (g_resume_enabled ? "1" : "0"));
}

void send_performance() {
  const uint32_t work_us = g_audio_work_us.load();
  const uint32_t pcm_us = g_audio_pcm_us.load();
  const uint32_t load_x10 = pcm_us
      ? static_cast<uint32_t>((static_cast<uint64_t>(work_us) * 1000) / pcm_us)
      : 0;
  send_ble("PERF|" + std::to_string(load_x10) + '|' +
           std::to_string(g_audio_max_frame_us.load()) + '|' +
           std::to_string(g_audio_short_writes.load()) + '|' +
           std::to_string(g_audio_frames.load()));
}

void send_tracks() {
  send_ble("TRACKS_BEGIN|" + std::to_string(g_tracks.size()));
  for (size_t i = 0; i < g_tracks.size(); ++i) {
    send_ble("TRACK|" + std::to_string(i) + '|' + g_tracks[i]);
  }
  send_ble("TRACKS_END");
}

void send_mappings() {
  send_ble("MAPS_BEGIN");
  for (const auto& uid : split(nvs_get_string("uids"), ',')) {
    if (!uid.empty()) {
      send_ble("MAP|" + uid + '|' + nvs_get_string(preference_key('m', uid).c_str()) +
               '|' + card_label(uid) + '|' + (load_shuffle(uid) ? "1" : "0"));
    }
  }
  send_ble("MAPS_END");
}

void scan_tracks() {
  g_tracks.clear();
  DIR* directory = opendir("/sdcard");
  if (!directory) return;
  while (dirent* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (hidden_path(name)) continue;
    const std::string lowered = lower(name);
    if (lowered.size() >= 4 && lowered.ends_with(".mp3")) g_tracks.push_back('/' + name);
  }
  closedir(directory);
}

bool configure_i2s(uint32_t sample_rate) {
  i2s_channel_disable(g_i2s_tx);
  i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  clock.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_channel_reconfig_std_clock(g_i2s_tx, &clock) != ESP_OK) return false;
  // The I2S data plane is independent of the amplifier's I2C control plane.
  // Keep DMA running if the shared I2C rail is temporarily unavailable; the
  // codec may already retain a valid configuration from an earlier start.
  if (!g_amplifier->begin(sample_rate)) ESP_LOGW(kTag, "amplifier control unavailable");
  return i2s_channel_enable(g_i2s_tx) == ESP_OK;
}

void queue_audio(const AudioRequest& request) {
  xQueueOverwrite(g_audio_queue, &request);
}

void stop_audio() {
  AudioRequest request;
  request.action = AudioAction::kStop;
  queue_audio(request);
  g_audio_playing = false;
}

void play_file(const std::string& path, uint32_t start_seconds = 0) {
  AudioRequest request;
  request.action = AudioAction::kFile;
  std::snprintf(request.path, sizeof(request.path), "/sdcard%s", path.c_str());
  request.start_seconds = start_seconds;
  queue_audio(request);
}

void play_tap_sound() {
  AudioRequest request;
  request.action = AudioAction::kMemory;
  request.memory = card_tap_start;
  request.memory_size = card_tap_end - card_tap_start;
  queue_audio(request);
  send_ble("PLAYING_TAP_SOUND");
}

void advance_playlist() {
  if (!g_card_playback || g_active_playlist.count == 0) return;
  if (g_active_shuffle && g_active_playlist.count > 1) {
    const uint8_t previous = g_playlist_position;
    do {
      g_playlist_position = esp_random() % g_active_playlist.count;
    } while (g_playlist_position == previous);
  } else {
    g_playlist_position = (g_playlist_position + 1) % g_active_playlist.count;
  }
  g_resume_base_seconds = 0;
  g_session_paused = false;
  if (!g_active_shuffle && g_resume_enabled) save_resume(g_active_uid, g_playlist_position, 0);
  const int track = g_active_playlist.tracks[g_playlist_position];
  if (track >= 0 && track < static_cast<int>(g_tracks.size())) {
    play_file(g_tracks[track]);
    g_track_started_us = esp_timer_get_time();
    send_ble("PLAYING|" + g_active_uid + '|' + std::to_string(g_playlist_position) +
             '|' + std::to_string(track) + "|0");
  }
}

void play_selected_card();

void audio_task(void*) {
  auto* pcm = static_cast<int16_t*>(heap_caps_malloc(
      micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto* input = static_cast<uint8_t*>(heap_caps_malloc(
      4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!pcm || !input) {
    ESP_LOGE(kTag, "audio buffers allocation failed");
    vTaskDelete(nullptr);
  }

  AudioRequest request;
  while (xQueueReceive(g_audio_queue, &request, portMAX_DELAY) == pdTRUE) {
    if (request.action == AudioAction::kStop) {
      g_audio_playing = false;
      i2s_channel_disable(g_i2s_tx);
      i2s_channel_enable(g_i2s_tx);
      continue;
    }

    FILE* file = nullptr;
    const uint8_t* memory = request.memory;
    size_t memory_offset = 0;
    const size_t memory_size = request.memory_size;
    if (request.action == AudioAction::kFile) {
      file = std::fopen(request.path, "rb");
      if (!file) {
        ESP_LOGE(kTag, "cannot open %s: %s", request.path, std::strerror(errno));
        send_ble("ERROR|OPEN_TRACK");
        continue;
      }
    }

    micro_mp3::Mp3Decoder decoder;
    size_t valid = 0;
    size_t offset = 0;
    bool configured = false;
    bool interrupted = false;
    g_audio_playing = true;
    g_audio_work_us = 0;
    g_audio_pcm_us = 0;
    g_audio_max_frame_us = 0;
    g_audio_frames = 0;
    g_audio_short_writes = 0;

    while (g_audio_playing) {
      AudioRequest replacement;
      if (xQueueReceive(g_audio_queue, &replacement, 0) == pdTRUE) {
        request = replacement;
        interrupted = true;
        break;
      }
      if (offset == valid) {
        offset = 0;
        valid = file ? std::fread(input, 1, 4096, file)
                     : std::min<size_t>(4096, memory_size - memory_offset);
        if (!file && valid) {
          std::memcpy(input, memory + memory_offset, valid);
          memory_offset += valid;
        }
        if (valid == 0) break;
      }

      size_t consumed = 0;
      size_t samples = 0;
      const int64_t frame_started_us = esp_timer_get_time();
      const auto result = decoder.decode(input + offset, valid - offset,
                                         reinterpret_cast<uint8_t*>(pcm),
                                         micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES,
                                         consumed, samples);
      offset += consumed;
      if (result == micro_mp3::MP3_STREAM_INFO_READY) {
        if (!configure_i2s(decoder.get_sample_rate())) {
          send_ble("ERROR|AUDIO_CONFIG");
          break;
        }
        configured = true;
        if (file && request.start_seconds && decoder.get_bitrate()) {
          const long byte_offset = static_cast<long>(
              (uint64_t(request.start_seconds) * decoder.get_bitrate() * 1000) / 8);
          std::fseek(file, byte_offset, SEEK_SET);
          decoder.reset();
          valid = offset = 0;
          request.start_seconds = 0;
        }
        continue;
      }
      if (result == micro_mp3::MP3_NEED_MORE_DATA) continue;
      if (result < 0) {
        if (consumed == 0 && offset < valid) ++offset;
        continue;
      }
      if (!configured && decoder.get_sample_rate()) configured = configure_i2s(decoder.get_sample_rate());
      if (samples) {
        size_t values = samples * decoder.get_channels();
        if (decoder.get_channels() == 1) {
          for (size_t i = samples; i-- > 0;) {
            pcm[i * 2] = pcm[i];
            pcm[i * 2 + 1] = pcm[i];
          }
          values = samples * 2;
        }
        const uint8_t volume = g_volume.load(std::memory_order_relaxed);
        if (volume == 0) {
          std::memset(pcm, 0, values * sizeof(int16_t));
        } else if (volume < 100) {
          for (size_t i = 0; i < values; ++i) {
            pcm[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * volume) / 100);
          }
        }
        const uint32_t frame_work_us = static_cast<uint32_t>(
            esp_timer_get_time() - frame_started_us);
        const uint32_t frame_pcm_us = decoder.get_sample_rate()
            ? static_cast<uint32_t>((static_cast<uint64_t>(samples) * 1000000) /
                                    decoder.get_sample_rate())
            : 0;
        g_audio_work_us.fetch_add(frame_work_us, std::memory_order_relaxed);
        g_audio_pcm_us.fetch_add(frame_pcm_us, std::memory_order_relaxed);
        g_audio_frames.fetch_add(1, std::memory_order_relaxed);
        uint32_t previous_max = g_audio_max_frame_us.load(std::memory_order_relaxed);
        while (frame_work_us > previous_max &&
               !g_audio_max_frame_us.compare_exchange_weak(previous_max, frame_work_us,
                                                            std::memory_order_relaxed)) {}
        size_t written = 0;
        const size_t bytes = values * sizeof(int16_t);
        const esp_err_t write_result = i2s_channel_write(
            g_i2s_tx, pcm, bytes, &written, pdMS_TO_TICKS(100));
        if (write_result != ESP_OK || written != bytes) {
          g_audio_short_writes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    if (file) std::fclose(file);
    g_audio_playing = false;
    const uint32_t pcm_us = g_audio_pcm_us.load();
    const uint32_t load_x10 = pcm_us
        ? static_cast<uint32_t>((static_cast<uint64_t>(g_audio_work_us.load()) * 1000) / pcm_us)
        : 0;
    ESP_LOGI(kTag, "audio perf: load=%lu.%lu%% max_frame=%lu us short_writes=%lu frames=%lu",
             static_cast<unsigned long>(load_x10 / 10),
             static_cast<unsigned long>(load_x10 % 10),
             static_cast<unsigned long>(g_audio_max_frame_us.load()),
             static_cast<unsigned long>(g_audio_short_writes.load()),
             static_cast<unsigned long>(g_audio_frames.load()));
    if (interrupted) {
      xQueueOverwrite(g_audio_queue, &request);
    } else if (request.action == AudioAction::kMemory && g_play_after_tap.exchange(false)) {
      play_selected_card();
    } else if (g_card_playback) {
      advance_playlist();
    }
  }
}

void pause_playback() {
  uint32_t elapsed = g_resume_base_seconds;
  if (g_card_playback && !g_active_uid.empty() && !g_active_shuffle) {
    elapsed += static_cast<uint32_t>((esp_timer_get_time() - g_track_started_us) / 1000000);
    g_resume_base_seconds = elapsed;
    g_session_paused = true;
    if (g_resume_enabled) save_resume(g_active_uid, g_playlist_position, elapsed);
  }
  g_play_after_tap = false;
  g_card_playback = false;
  stop_audio();
  send_ble("PAUSED|" + g_active_uid + '|' + std::to_string(g_playlist_position) +
           '|' + std::to_string(elapsed));
}

void select_card(const std::string& uid) {
  if (uid.empty()) return;
  const bool preserve_session = g_tap_mode == TapMode::kTogglePlayPause &&
                                uid == g_active_uid && g_session_paused && !g_active_shuffle;
  const bool toggle_pause = g_tap_mode == TapMode::kTogglePlayPause &&
                            uid == g_active_uid && g_card_playback;
  if (g_card_playback) pause_playback();
  remember_uid(uid);
  g_active_uid = uid;
  g_active_playlist = load_playlist(uid);
  g_active_shuffle = load_shuffle(uid);
  if (!preserve_session && !toggle_pause) {
    g_playlist_position = 0;
    g_resume_base_seconds = 0;
    g_session_paused = false;
  }
  ESP_LOGI(kTag, "selected %s: %s", card_label(uid).c_str(), uid.c_str());
  send_ble("CARD|" + uid + '|' + card_label(uid));
  if (g_active_playlist.count == 0) send_ble("UNMAPPED|" + uid);
  g_card_playback = false;
  g_play_after_tap = !toggle_pause && g_active_playlist.count > 0;
  play_tap_sound();
}

void select_saved_card(const std::string& uid) {
  if (uid.empty()) return;
  if (g_card_playback || g_play_after_tap) pause_playback();
  remember_uid(uid);
  g_active_uid = uid;
  g_active_playlist = load_playlist(uid);
  g_active_shuffle = load_shuffle(uid);
  g_playlist_position = 0;
  g_resume_base_seconds = 0;
  g_session_paused = false;
  send_ble("CARD|" + uid + '|' + card_label(uid));
  if (g_active_playlist.count == 0) send_ble("UNMAPPED|" + uid);
}

void play_selected_card() {
  g_play_after_tap = false;
  if (g_active_uid.empty()) return send_ble("ERROR|SELECT_A_CARD");
  g_active_playlist = load_playlist(g_active_uid);
  g_active_shuffle = load_shuffle(g_active_uid);
  if (g_active_playlist.count == 0) return send_ble("ERROR|MAP_A_PLAYLIST");
  ResumeState resume;
  if (g_session_paused && !g_active_shuffle) {
    resume.playlist_position = g_playlist_position;
    resume.seconds = g_resume_base_seconds;
  } else if (!g_active_shuffle && g_resume_enabled) {
    resume = load_resume(g_active_uid);
  }
  if (resume.playlist_position >= g_active_playlist.count) resume = {};
  if (g_active_shuffle) resume.playlist_position = esp_random() % g_active_playlist.count;
  g_playlist_position = resume.playlist_position;
  g_resume_base_seconds = resume.seconds;
  g_session_paused = false;
  const int track = g_active_playlist.tracks[g_playlist_position];
  if (track >= static_cast<int>(g_tracks.size())) return send_ble("ERROR|TRACK_INDEX");
  g_card_playback = true;
  g_track_started_us = esp_timer_get_time();
  play_file(g_tracks[track], resume.seconds);
  send_ble("PLAYING|" + g_active_uid + '|' + std::to_string(g_playlist_position) +
           '|' + std::to_string(track) + '|' + std::to_string(resume.seconds));
}

void play_track(int index) {
  if (index < 0 || index >= static_cast<int>(g_tracks.size())) return send_ble("ERROR|TRACK_INDEX");
  g_play_after_tap = false;
  g_card_playback = false;
  g_session_paused = false;
  play_file(g_tracks[index]);
  send_ble("PLAYING_TRACK|" + std::to_string(index));
}

void stop_and_clear_active_playlist(bool forget_card) {
  g_play_after_tap = false;
  g_card_playback = false;
  g_session_paused = false;
  stop_audio();
  g_active_playlist = {};
  g_active_shuffle = false;
  g_playlist_position = 0;
  g_resume_base_seconds = 0;
  if (forget_card) {
    g_active_uid.clear();
  }
}

void erase_card_keys(char prefix) {
  for (const auto& uid : split(nvs_get_string("uids"), ',')) {
    if (!uid.empty()) nvs_erase_key(g_nvs, preference_key(prefix, uid).c_str());
  }
}

void finish_upload() {
  if (!g_upload_active || !g_upload_file) return send_ble("ERROR|NO_UPLOAD");
  std::fflush(g_upload_file);
  std::fclose(g_upload_file);
  g_upload_file = nullptr;
  g_upload_active = false;
  const std::string path = "/" + g_upload_name;
  if (g_upload_received != g_upload_expected) {
    std::remove(("/sdcard" + path).c_str());
    return send_ble("ERROR|UPLOAD_SIZE|" + std::to_string(g_upload_received) +
                    '|' + std::to_string(g_upload_expected));
  }
  if (std::find(g_tracks.begin(), g_tracks.end(), path) == g_tracks.end() &&
      !hidden_path(path)) g_tracks.push_back(path);
  ESP_LOGI(kTag, "upload complete: %s (%lu bytes)", path.c_str(),
           static_cast<unsigned long>(g_upload_received));
  send_ble("UPLOAD_DONE|" + g_upload_name + '|' + std::to_string(g_upload_received));
  send_tracks();
  send_status();
}

void cancel_upload() {
  if (g_upload_file) std::fclose(g_upload_file);
  g_upload_file = nullptr;
  if (!g_upload_name.empty()) std::remove(("/sdcard/" + g_upload_name).c_str());
  g_upload_active = false;
  send_ble("UPLOAD_CANCELLED");
}

void upload_chunk(const uint8_t* bytes, size_t length) {
  if (!g_upload_active || !g_upload_file || !length) return;
  const size_t remaining = g_upload_expected - g_upload_received;
  const size_t count = std::min(length, remaining);
  const size_t written = std::fwrite(bytes, 1, count, g_upload_file);
  g_upload_received += written;
  if (written != count) {
    cancel_upload();
    send_ble("ERROR|UPLOAD_WRITE");
  }
}

void handle_command(std::string command) {
  command = trim(command);
  ESP_LOGI(kTag, "BLE< %s", command.c_str());
  if (command == "HELLO") {
    send_ble(std::string("INFO|Glyph Soundbox|") + kVersion);
  } else if (command == "STATUS") {
    send_status();
  } else if (command == "TRACKS") {
    send_tracks();
  } else if (command == "MAPS") {
    send_mappings();
  } else if (command == "VOLUME") {
    send_volume();
  } else if (command == "TAP_MODE") {
    send_tap_mode();
  } else if (command == "RESUME") {
    send_resume_setting();
  } else if (command == "PERF") {
    send_performance();
  } else if (starts_with(command, "VOLUME|")) {
    char* end = nullptr;
    const long requested = std::strtol(command.substr(7).c_str(), &end, 10);
    if (*end != '\0' || requested < 0 || requested > 100) {
      return send_ble("ERROR|VOLUME_RANGE");
    }
    g_volume = static_cast<uint8_t>(requested);
    nvs_set_u8(g_nvs, "volume", g_volume.load());
    nvs_commit(g_nvs);
    send_volume();
  } else if (starts_with(command, "TAP_MODE|")) {
    const std::string mode = command.substr(9);
    if (mode != "presence" && mode != "toggle") {
      return send_ble("ERROR|TAP_MODE_VALUE");
    }
    g_tap_mode = mode == "toggle" ? TapMode::kTogglePlayPause : TapMode::kPlayWhilePresent;
    nvs_set_u8(g_nvs, "tap_mode", static_cast<uint8_t>(g_tap_mode.load()));
    nvs_commit(g_nvs);
    send_tap_mode();
  } else if (starts_with(command, "RESUME|")) {
    const std::string value = command.substr(7);
    if (value != "0" && value != "1") return send_ble("ERROR|RESUME_VALUE");
    g_resume_enabled = value == "1";
    nvs_set_u8(g_nvs, "resume", g_resume_enabled ? 1 : 0);
    nvs_commit(g_nvs);
    send_resume_setting();
  } else if (starts_with(command, "UPLOAD_BEGIN|")) {
    const size_t pipe = command.find_last_of('|');
    if (pipe <= 13) return send_ble("ERROR|UPLOAD_FORMAT");
    const std::string name = safe_upload_name(command.substr(13, pipe - 13));
    const uint32_t expected = std::strtoul(command.substr(pipe + 1).c_str(), nullptr, 10);
    if (name.empty() || !lower(name).ends_with(".mp3") || expected == 0) return send_ble("ERROR|UPLOAD_FILE");
    if (g_upload_active) cancel_upload();
    g_play_after_tap = false;
    g_card_playback = false;
    stop_audio();
    g_upload_name = name;
    g_upload_expected = expected;
    g_upload_received = 0;
    const std::string path = "/sdcard/" + name;
    std::remove(path.c_str());
    g_upload_file = std::fopen(path.c_str(), "wb");
    g_upload_active = g_upload_file != nullptr;
    send_ble(g_upload_active ? "UPLOAD_READY|" + name : "ERROR|UPLOAD_OPEN");
  } else if (starts_with(command, "UPLOAD_SYNC|")) {
    const uint32_t expected = std::strtoul(command.substr(12).c_str(), nullptr, 10);
    if (!g_upload_active) return send_ble("ERROR|NO_UPLOAD");
    std::fflush(g_upload_file);
    if (expected != g_upload_received) {
      send_ble("ERROR|UPLOAD_SIZE|" + std::to_string(g_upload_received) + '|' + std::to_string(expected));
    } else {
      send_ble("UPLOAD_ACK|" + std::to_string(g_upload_received));
    }
  } else if (command == "UPLOAD_END") {
    finish_upload();
  } else if (command == "UPLOAD_CANCEL") {
    cancel_upload();
  } else if (command == "PLAY") {
    play_selected_card();
  } else if (command == "PAUSE") {
    pause_playback();
  } else if (starts_with(command, "PLAY_TRACK|")) {
    play_track(std::strtol(command.substr(11).c_str(), nullptr, 10));
  } else if (starts_with(command, "SELECT|")) {
    const std::string uid = upper(command.substr(7));
    if (uid_known(uid)) select_saved_card(uid);
  } else if (starts_with(command, "MAP|")) {
    const size_t first_pipe = command.find('|', 4);
    if (first_pipe == std::string::npos) return send_ble("ERROR|MAP_FORMAT");
    const size_t second_pipe = command.find('|', first_pipe + 1);
    const std::string uid = upper(command.substr(4, first_pipe - 4));
    const std::string csv = command.substr(first_pipe + 1,
        second_pipe == std::string::npos ? std::string::npos : second_pipe - first_pipe - 1);
    const Playlist playlist = parse_playlist(csv);
    const bool shuffle = second_pipe != std::string::npos && command.substr(second_pipe + 1) == "1";
    if (uid.empty() || playlist.count == 0) return send_ble("ERROR|EMPTY_MAP");
    save_playlist(uid, playlist);
    save_shuffle(uid, shuffle);
    if (uid == g_active_uid) {
      g_active_playlist = playlist;
      g_active_shuffle = shuffle;
    }
    send_ble("SAVED|" + uid + '|' + playlist_csv(playlist) + '|' + (shuffle ? "1" : "0"));
  } else if (starts_with(command, "RENAME|")) {
    const size_t pipe = command.find('|', 7);
    if (pipe == std::string::npos) return send_ble("ERROR|RENAME_FORMAT");
    const std::string uid = upper(command.substr(7, pipe - 7));
    std::string name = trim(command.substr(pipe + 1));
    std::replace(name.begin(), name.end(), '|', ' ');
    if (!uid_known(uid) || name.empty() || name.size() > 48) return send_ble("ERROR|CARD_NAME");
    nvs_set_string(preference_key('n', uid).c_str(), name);
    send_ble("RENAMED|" + uid + '|' + name);
  } else if (starts_with(command, "SHUFFLE|")) {
    const size_t pipe = command.find('|', 8);
    if (pipe == std::string::npos) return send_ble("ERROR|SHUFFLE_FORMAT");
    const std::string uid = upper(command.substr(8, pipe - 8));
    const std::string value = command.substr(pipe + 1);
    if (!uid_known(uid) || (value != "0" && value != "1")) {
      return send_ble("ERROR|SHUFFLE_VALUE");
    }
    const bool shuffle = value == "1";
    save_shuffle(uid, shuffle);
    if (uid == g_active_uid) g_active_shuffle = shuffle;
    send_ble("SHUFFLE|" + uid + '|' + value);
  } else if (command == "CLEAR_CHECKPOINTS") {
    erase_card_keys('r');
    nvs_commit(g_nvs);
    g_session_paused = false;
    g_resume_base_seconds = 0;
    send_ble("CLEARED_CHECKPOINTS");
  } else if (command == "CLEAR_MAPPINGS") {
    stop_and_clear_active_playlist(false);
    erase_card_keys('m');
    nvs_commit(g_nvs);
    send_ble("CLEARED_MAPPINGS");
  } else if (command == "CLEAR_USER_DATA") {
    stop_and_clear_active_playlist(true);
    nvs_erase_all(g_nvs);
    nvs_commit(g_nvs);
    g_tap_mode = TapMode::kPlayWhilePresent;
    g_resume_enabled = true;
    g_volume = 75;
    send_tap_mode();
    send_resume_setting();
    send_volume();
    send_ble("CLEARED_USER_DATA");
  } else if (starts_with(command, "CLEAR|")) {
    const std::string uid = upper(command.substr(6));
    nvs_erase_key(g_nvs, preference_key('m', uid).c_str());
    nvs_erase_key(g_nvs, preference_key('r', uid).c_str());
    nvs_erase_key(g_nvs, preference_key('s', uid).c_str());
    nvs_commit(g_nvs);
    send_ble("CLEARED|" + uid);
  } else {
    send_ble("ERROR|UNKNOWN_COMMAND");
  }
}

void ble_task(void*) {
  BlePacket packet;
  while (xQueueReceive(g_ble_queue, &packet, portMAX_DELAY) == pdTRUE) {
    if (packet.length && packet.data[0] == 1) upload_chunk(packet.data + 1, packet.length - 1);
    else handle_command(std::string(reinterpret_cast<char*>(packet.data), packet.length));
  }
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    g_ble_connected = true;
    ESP_LOGI(kTag, "BLE client connected");
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int) override {
    g_ble_connected = false;
    ESP_LOGI(kTag, "BLE client disconnected");
    server->startAdvertising();
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const auto value = characteristic->getValue();
    BlePacket packet;
    packet.length = std::min<size_t>(value.size(), sizeof(packet.data));
    std::memcpy(packet.data, value.data(), packet.length);
    if (xQueueSend(g_ble_queue, &packet, 0) != pdPASS) ESP_LOGW(kTag, "BLE queue full");
  }
};

void begin_ble() {
  NimBLEDevice::init(kBleDeviceName);
  NimBLEDevice::setMTU(185);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* service = server->createService(kServiceUuid);
  auto* commands = service->createCharacteristic(kCommandUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  commands->setCallbacks(new CommandCallbacks());
  g_events = service->createCharacteristic(kEventsUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_events->setValue("BOOTING");
  server->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setName(kBleDeviceName);
  advertising->start();
  ESP_LOGI(kTag, "BLE advertising as '%s'", kBleDeviceName);
}

std::string uid_string(const uint8_t* uid, uint8_t length) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(length * 2);
  for (uint8_t i = 0; i < length; ++i) {
    result += hex[uid[i] >> 4];
    result += hex[uid[i] & 0x0F];
  }
  return result;
}

void nfc_task(void*) {
  std::string candidate;
  uint8_t candidate_reads = 0;
  int64_t candidate_seen_us = 0;
  int64_t present_seen_us = 0;
  while (true) {
    if (g_upload_active) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!g_nfc_ready) {
      g_nfc_ready = g_nfc->begin();
      if (!g_nfc_ready) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      send_status();
    }
    uint8_t uid[10]{};
    uint8_t length = 0;
    const int64_t now = esp_timer_get_time();
    if (g_nfc->read_uid(uid, &length, 250)) {
      const std::string seen = uid_string(uid, length);
      present_seen_us = now;
      if (seen == g_present_uid) {
        candidate.clear();
        candidate_reads = 0;
      } else {
        if (seen == candidate && now - candidate_seen_us <= 700000) ++candidate_reads;
        else { candidate = seen; candidate_reads = 1; }
        candidate_seen_us = now;
        if (candidate_reads >= 2) {
          g_present_uid = candidate;
          candidate.clear();
          candidate_reads = 0;
          ESP_LOGI(kTag, "NFC stable read: %s", g_present_uid.c_str());
          select_card(g_present_uid);
        }
      }
    } else {
      if (!candidate.empty() && now - candidate_seen_us > 700000) {
        candidate.clear();
        candidate_reads = 0;
      }
      if (!g_present_uid.empty() && now - present_seen_us > 700000) {
        ESP_LOGI(kTag, "NFC card removed: %s", g_present_uid.c_str());
        if (g_tap_mode == TapMode::kPlayWhilePresent && g_present_uid == g_active_uid &&
            (g_card_playback || g_play_after_tap)) {
          pause_playback();
        }
        g_present_uid.clear();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(90));
  }
}

void init_i2c() {
  // A USB reset can interrupt a PN532 response while it is driving SDA low.
  // Clock the slave to the end of that byte, then generate a STOP before the
  // ESP-IDF controller claims the pins.
  const gpio_config_t recovery = {
      .pin_bit_mask = (1ULL << kI2cSda) | (1ULL << kI2cScl),
      .mode = GPIO_MODE_INPUT_OUTPUT_OD,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&recovery));
  gpio_set_level(kI2cSda, 1);
  gpio_set_level(kI2cScl, 1);
  esp_rom_delay_us(10);
  if (!gpio_get_level(kI2cSda)) {
    for (int pulse = 0; pulse < 9 && !gpio_get_level(kI2cSda); ++pulse) {
      gpio_set_level(kI2cScl, 0);
      esp_rom_delay_us(10);
      gpio_set_level(kI2cScl, 1);
      esp_rom_delay_us(10);
    }
  }
  gpio_set_level(kI2cSda, 0);
  esp_rom_delay_us(10);
  gpio_set_level(kI2cScl, 1);
  esp_rom_delay_us(10);
  gpio_set_level(kI2cSda, 1);
  esp_rom_delay_us(10);
  gpio_reset_pin(kI2cSda);
  gpio_reset_pin(kI2cScl);

  const i2c_master_bus_config_t config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = kI2cSda,
      .scl_io_num = kI2cScl,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags = {.enable_internal_pullup = true, .allow_pd = false},
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &g_i2c_bus));
  g_i2c_mutex = xSemaphoreCreateMutex();
  g_nfc = new Pn532(g_i2c_bus, g_i2c_mutex);
  g_amplifier = new Nau8325(g_i2c_bus, g_i2c_mutex);
}

void init_sd() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = kSdMosi;
  bus.miso_io_num = kSdMiso;
  bus.sclk_io_num = kSdSck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 16 * 1024;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = kSdCs;
  slot.host_id = SPI2_HOST;
  esp_vfs_fat_sdmmc_mount_config_t mount = {
      .format_if_mount_failed = false,
      .max_files = 6,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // Start conservatively: this board's long shared rail is reliable at 4 MHz.
  // Playback remains DMA-fed and MP3 bitrates are far below this bus rate.
  host.max_freq_khz = 4000;
  esp_err_t result = ESP_FAIL;
  for (int attempt = 1; attempt <= 8; ++attempt) {
    g_sd_card = nullptr;
    result = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &g_sd_card);
    if (result == ESP_OK) break;
    ESP_LOGW(kTag, "SD mount attempt %d failed: %s", attempt, esp_err_to_name(result));
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  g_sd_ready = result == ESP_OK;
  if (!g_sd_ready) {
    ESP_LOGE(kTag, "SD mount failed: %s", esp_err_to_name(result));
    return;
  }
  scan_tracks();
  ESP_LOGI(kTag, "SD ready; %u MP3 track(s) found", static_cast<unsigned>(g_tracks.size()));
}

void init_i2s() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = 8;
  channel.dma_frame_num = 256;
  channel.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&channel, &g_i2s_tx, nullptr));
  i2s_std_config_t standard = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = kI2sMclk,
          .bclk = kI2sBclk,
          .ws = kI2sWs,
          .dout = kI2sDout,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {},
      },
  };
  standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_i2s_tx, &standard));
  ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_tx));
  g_amplifier->begin(44100);
}

}  // namespace

extern "C" void app_main() {
  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP_LOGI(kTag, "=== Glyph Soundbox native ESP-IDF %s ===", kVersion);
  ESP_LOGI(kTag, "chip: ESP32-C6 single-core; audio transport: I2S DMA");

  esp_err_t nvs_result = nvs_flash_init();
  if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_result);
  ESP_ERROR_CHECK(nvs_open("yotopoc", NVS_READWRITE, &g_nvs));
  uint8_t saved_volume = 75;
  if (nvs_get_u8(g_nvs, "volume", &saved_volume) == ESP_OK && saved_volume <= 100) {
    g_volume = saved_volume;
  }
  uint8_t saved_tap_mode = 0;
  if (nvs_get_u8(g_nvs, "tap_mode", &saved_tap_mode) == ESP_OK && saved_tap_mode <= 1) {
    g_tap_mode = static_cast<TapMode>(saved_tap_mode);
  }
  uint8_t saved_resume = 1;
  if (nvs_get_u8(g_nvs, "resume", &saved_resume) == ESP_OK && saved_resume <= 1) {
    g_resume_enabled = saved_resume != 0;
  }

  g_audio_queue = xQueueCreate(1, sizeof(AudioRequest));
  g_ble_queue = xQueueCreate(32, sizeof(BlePacket));
  init_i2c();
  g_nfc_ready = g_nfc->begin();
  if (!g_nfc_ready) ESP_LOGW(kTag, "PN532 not found; retrying in NFC task");
  init_sd();
  init_i2s();
  begin_ble();

  xTaskCreate(audio_task, "audio_decode", 8192, nullptr, 7, nullptr);
  xTaskCreate(ble_task, "ble_commands", 6144, nullptr, 5, nullptr);
  xTaskCreate(nfc_task, "nfc", 5120, nullptr, 3, nullptr);
  ESP_LOGI(kTag, "READY: tap a card or connect the PWA over Bluetooth");
  send_status();
}
