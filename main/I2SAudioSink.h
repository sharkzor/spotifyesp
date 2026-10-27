#pragma once

#include <AudioSink.h>

#include "driver/i2s_std.h"

// Plays stereo PCM through an I2S DAC (e.g. PCM5102) using the ESP-IDF 5.x I2S driver.
class I2SAudioSink : public AudioSink {
 public:
  I2SAudioSink(int bclk, int lrclk, int dout);
  ~I2SAudioSink() override;

  void feedPCMFrames(const uint8_t* buffer, size_t bytes) override;
  void volumeChanged(uint16_t volume) override { volume_ = volume; }
  bool setParams(uint32_t sampleRate, uint8_t channelCount,
                 uint8_t bitDepth) override;

 private:
  i2s_chan_handle_t tx_ = nullptr;
  uint32_t sampleRate_ = 0;
  uint16_t volume_ = 65535;  // 0..65535, applied in software
};
