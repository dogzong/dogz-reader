#include "HalAudioOutput.h"

#include "AudioResampler.h"

#if CROSSPOINT_CAP_SOUND_FEEDBACK

#include <AudioManager.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <decoder/impl/esp_mp3_dec.h>
#include <simple_dec/esp_audio_simple_dec.h>

#include <algorithm>
#include <cstring>

#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
#include "Waveshare397Power.h"
#endif

namespace HalAudioOutput {
namespace {

constexpr unsigned long AUDIO_RAIL_SETTLE_MS = 10;
constexpr size_t MP3_INPUT_BYTES = 4096;
constexpr size_t MP3_PCM_BYTES = 8192;

AudioManager audio;
bool initialized = false;
bool mp3DecoderRegistered = false;

struct Mp3Playback {
  ReadCallback sourceRead = nullptr;
  SeekCallback sourceSeek = nullptr;
  esp_audio_simple_dec_handle_t decoder = nullptr;
  uint8_t compressed[MP3_INPUT_BYTES]{};
  size_t compressedLength = 0;
  size_t compressedOffset = 0;
  uint8_t pcm[MP3_PCM_BYTES]{};
  size_t pcmLength = 0;
  size_t pcmOffset = 0;
  bool endOfInput = false;
  bool failed = false;
  esp_audio_simple_dec_info_t info{};
  AudioResampler resampler;
};

Mp3Playback mp3;

void closeMp3Decoder() {
  if (mp3.decoder) esp_audio_simple_dec_close(mp3.decoder);
  mp3 = Mp3Playback{};
}

bool prepareMp3Source(const ReadCallback read, const SeekCallback seek) {
  if (!read || !seek) return false;
  if (!mp3DecoderRegistered) {
    if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK) return false;
    mp3DecoderRegistered = true;
  }
  mp3.sourceRead = read;
  mp3.sourceSeek = seek;

  // Skip an ID3v2 tag without buffering its contents or artwork. Invalid tags
  // are rejected rather than handed to the decoder as potentially huge input.
  uint8_t id3[10];
  const int headerBytes = read(id3, sizeof(id3));
  if (headerBytes < 0) return false;
  size_t audioStart = 0;
  if (headerBytes == static_cast<int>(sizeof(id3)) && std::memcmp(id3, "ID3", 3) == 0) {
    if ((id3[6] | id3[7] | id3[8] | id3[9]) & 0x80) return false;
    const uint32_t tagBytes = (static_cast<uint32_t>(id3[6]) << 21) | (static_cast<uint32_t>(id3[7]) << 14) |
                              (static_cast<uint32_t>(id3[8]) << 7) | static_cast<uint32_t>(id3[9]);
    audioStart = 10u + tagBytes + ((id3[5] & 0x10) ? 10u : 0u);
  }
  if (!seek(audioStart)) return false;

  esp_audio_simple_dec_cfg_t config{};
  config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
  config.use_frame_dec = false;
  if (esp_audio_simple_dec_open(&config, &mp3.decoder) != ESP_AUDIO_ERR_OK) {
    closeMp3Decoder();
    return false;
  }
  return true;
}

bool decodeNextMp3Frame() {
  while (!mp3.failed) {
    if (mp3.compressedOffset >= mp3.compressedLength && !mp3.endOfInput) {
      mp3.compressedOffset = 0;
      const int n = mp3.sourceRead(mp3.compressed, sizeof(mp3.compressed));
      if (n < 0 || n > static_cast<int>(sizeof(mp3.compressed))) {
        mp3.failed = true;
        return false;
      }
      mp3.compressedLength = static_cast<size_t>(n);
      mp3.endOfInput = n == 0;
    }

    esp_audio_simple_dec_raw_t raw{};
    raw.buffer = mp3.compressed + mp3.compressedOffset;
    raw.len = static_cast<uint32_t>(mp3.compressedLength - mp3.compressedOffset);
    raw.eos = mp3.endOfInput;
    esp_audio_simple_dec_out_t frame{};
    frame.buffer = mp3.pcm;
    frame.len = sizeof(mp3.pcm);
    const esp_audio_err_t result = esp_audio_simple_dec_process(mp3.decoder, &raw, &frame);
    if (raw.consumed > raw.len) {
      mp3.failed = true;
      return false;
    }
    mp3.compressedOffset += raw.consumed;
    if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH || frame.needed_size > sizeof(mp3.pcm)) {
      mp3.failed = true;
      return false;
    }
    if (result != ESP_AUDIO_ERR_OK && result != ESP_AUDIO_ERR_DATA_LACK) {
      mp3.failed = true;
      return false;
    }
    if (frame.decoded_size > sizeof(mp3.pcm)) {
      mp3.failed = true;
      return false;
    }
    if (frame.decoded_size) {
      mp3.pcmLength = frame.decoded_size;
      mp3.pcmOffset = 0;
      if (esp_audio_simple_dec_get_info(mp3.decoder, &mp3.info) != ESP_AUDIO_ERR_OK || mp3.info.bits_per_sample != 16 ||
          (mp3.info.channel != 1 && mp3.info.channel != 2) || mp3.info.sample_rate < 8000 ||
          mp3.info.sample_rate > 48000 || mp3.pcmLength % (mp3.info.channel * sizeof(int16_t)) != 0) {
        mp3.failed = true;
        return false;
      }
      return true;
    }

    if (mp3.endOfInput && mp3.compressedOffset >= mp3.compressedLength) return false;
    if (raw.consumed == 0) {
      if (mp3.endOfInput) return false;
      // Preserve a partial frame and append more input. The parser must make
      // progress once the bounded input buffer is full.
      const size_t remaining = mp3.compressedLength - mp3.compressedOffset;
      if (remaining == sizeof(mp3.compressed)) {
        mp3.failed = true;
        return false;
      }
      std::memmove(mp3.compressed, mp3.compressed + mp3.compressedOffset, remaining);
      const int n = mp3.sourceRead(mp3.compressed + remaining, sizeof(mp3.compressed) - remaining);
      if (n <= 0 || static_cast<size_t>(n) > sizeof(mp3.compressed) - remaining) {
        mp3.endOfInput = true;
        mp3.compressedLength = remaining;
        mp3.compressedOffset = 0;
      } else {
        mp3.compressedLength = remaining + static_cast<size_t>(n);
        mp3.compressedOffset = 0;
      }
    }
  }
  return false;
}

int readMp3PcmNative(uint8_t* dst, const size_t length) {
  if (!dst || mp3.failed || !mp3.decoder) return -1;
  size_t copied = 0;
  while (copied < length) {
    if (mp3.pcmOffset >= mp3.pcmLength && !decodeNextMp3Frame()) break;
    const size_t available = mp3.pcmLength - mp3.pcmOffset;
    const size_t n = std::min(available, length - copied);
    std::memcpy(dst + copied, mp3.pcm + mp3.pcmOffset, n);
    copied += n;
    mp3.pcmOffset += n;
  }
  return copied ? static_cast<int>(copied) : (mp3.failed ? -1 : 0);
}

// Streaming linear interpolation keeps only two mono frames. The S3 codec
// is configured for 16 kHz; do not buffer whole songs or pass 44.1 kHz to it.
bool nextMonoFrame(int16_t& sample) {
  int16_t pair[2]{};
  const size_t bytes = mp3.info.channel * sizeof(int16_t);
  if (readMp3PcmNative(reinterpret_cast<uint8_t*>(pair), bytes) != static_cast<int>(bytes)) return false;
  sample = mp3.info.channel == 2 ? static_cast<int16_t>((static_cast<int32_t>(pair[0]) + pair[1]) / 2) : pair[0];
  return true;
}
int readMp3Pcm(uint8_t* dst, const size_t length) {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  size_t copied = 0;
  int16_t sample;
  while (copied + 2 <= length && mp3.resampler.read(nextMonoFrame, sample)) {
    std::memcpy(dst + copied, &sample, 2);
    copied += 2;
  }
  return copied ? static_cast<int>(copied) : (mp3.failed ? -1 : 0);
#else
  return readMp3PcmNative(dst, length);
#endif
}

bool setBoardPower(const bool enabled) {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  return Waveshare397Power::setAudioPower(enabled);
#else
  (void)enabled;
  return true;
#endif
}

bool begin() {
  if (initialized) return true;
  if (!setBoardPower(true)) {
    LOG_ERR("SND", "Failed to enable board audio power");
    return false;
  }
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  delay(AUDIO_RAIL_SETTLE_MS);
#endif
  if (!audio.begin()) {
    LOG_ERR("SND", "Failed to initialize audio output");
    setBoardPower(false);
    return false;
  }
  initialized = true;
  return true;
}

}  // namespace

bool playPcm(const ReadCallback read, const SeekCallback seek, const uint32_t sampleRate, const uint8_t channels,
             const uint8_t volume) {
  if (!read || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::PcmSource source = {read, seek};
  if (audio.playPcm(source, sampleRate, channels)) return true;
  LOG_ERR("SND", "Failed to play feedback PCM");
  shutdown();
  return false;
}

bool playWav(const ReadCallback read, const SeekCallback seek, const bool loop, const uint8_t volume) {
  if (!read || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::WavSource source = {read, seek};
  return audio.play(source, loop);
}

bool playMp3(const ReadCallback read, const SeekCallback seek, const uint8_t volume) {
  if (!read || !seek || !begin()) return false;
  audio.stop();
  closeMp3Decoder();
  if (!prepareMp3Source(read, seek) || !decodeNextMp3Frame()) {
    LOG_ERR("SND", "Failed to initialize MP3 decoder or decode first frame");
    closeMp3Decoder();
    return false;
  }
  mp3.resampler.reset(mp3.info.sample_rate);
  audio.setVolume(volume);
  AudioManager::PcmSource source = {readMp3Pcm, nullptr};
  const uint32_t outputRate = FREEINK_DEVICE_WAVESHARE_EPAPER_397 ? 16000 : mp3.info.sample_rate;
  const uint8_t outputChannels = FREEINK_DEVICE_WAVESHARE_EPAPER_397 ? 1 : mp3.info.channel;
  if (!audio.playPcm(source, outputRate, outputChannels)) {
    closeMp3Decoder();
    LOG_ERR("SND", "Failed to start MP3 PCM output");
    return false;
  }
  return true;
}

bool isPlaying() { return initialized && audio.isPlaying(); }
bool setPaused(const bool paused) { return initialized && audio.setPaused(paused); }
bool isPaused() { return initialized && audio.isPaused(); }
uint32_t positionMs() { return initialized ? audio.positionMs() : 0; }

bool beginCapture(const uint32_t sampleRate) {
  if (!begin() || audio.isPlaying()) return false;
  return audio.beginCapture(sampleRate);
}

int readCapture(int16_t* samples, const size_t capacity, const uint32_t timeoutMs) {
  return audio.readCapture(samples, capacity, timeoutMs);
}

void endCapture() { audio.endCapture(); }

bool restart(const uint8_t volume) {
  if (!initialized) return false;
  audio.setVolume(volume);
  return audio.requestRestart();
}

void setVolume(const uint8_t volume) {
  if (initialized) audio.setVolume(volume);
}

void shutdown() {
  if (initialized) {
    audio.powerDown();
    if (!setBoardPower(false)) LOG_ERR("SND", "Failed to disable board audio power");
    initialized = false;
  }
  closeMp3Decoder();
}

}  // namespace HalAudioOutput

#endif

#if !CROSSPOINT_CAP_SOUND_FEEDBACK
namespace HalAudioOutput {
bool playPcm(const ReadCallback, const SeekCallback, uint32_t, uint8_t, uint8_t) { return false; }
bool playWav(const ReadCallback, const SeekCallback, bool, uint8_t) { return false; }
bool playMp3(const ReadCallback, const SeekCallback, uint8_t) { return false; }
bool isPlaying() { return false; }
bool setPaused(bool) { return false; }
bool isPaused() { return false; }
uint32_t positionMs() { return 0; }
bool beginCapture(uint32_t) { return false; }
int readCapture(int16_t*, size_t, uint32_t) { return -1; }
void endCapture() {}
bool restart(uint8_t) { return false; }
void setVolume(uint8_t) {}
void shutdown() {}
}  // namespace HalAudioOutput
#endif
