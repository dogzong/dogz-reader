#pragma once

#include <cstddef>
#include <cstdint>

namespace HalAudioOutput {

using ReadCallback = int (*)(uint8_t* dst, size_t length);
using SeekCallback = bool (*)(size_t position);

bool playPcm(ReadCallback read, SeekCallback seek, uint32_t sampleRate, uint8_t channels, uint8_t volume);
bool playWav(ReadCallback read, SeekCallback seek, bool loop, uint8_t volume);
bool playMp3(ReadCallback read, SeekCallback seek, uint8_t volume);
bool isPlaying();
bool setPaused(bool paused);
bool isPaused();
uint32_t positionMs();
bool beginCapture(uint32_t sampleRate = 16000);
int readCapture(int16_t* samples, size_t capacity, uint32_t timeoutMs = 100);
void endCapture();
bool restart(uint8_t volume);
void setVolume(uint8_t volume);
void shutdown();

}  // namespace HalAudioOutput
