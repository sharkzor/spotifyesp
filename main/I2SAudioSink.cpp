#include "I2SAudioSink.h"

#include "esp_check.h"
#include "esp_log.h"

static const char* TAG = "i2s_sink";

I2SAudioSink::I2SAudioSink(int bclk, int lrclk, int dout) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;  // output silence on underrun
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_, nullptr));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = static_cast<gpio_num_t>(bclk),
              .ws = static_cast<gpio_num_t>(lrclk),
              .dout = static_cast<gpio_num_t>(dout),
              .din = I2S_GPIO_UNUSED,
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_));
  sampleRate_ = 44100;
}

I2SAudioSink::~I2SAudioSink() {
  i2s_channel_disable(tx_);
  i2s_del_channel(tx_);
}

void I2SAudioSink::feedPCMFrames(const uint8_t* buffer, size_t bytes) {
  size_t written = 0;
  while (written < bytes) {
    size_t chunk = 0;
    if (i2s_channel_write(tx_, buffer + written, bytes - written, &chunk,
                          1000) != ESP_OK) {
      ESP_LOGW(TAG, "i2s write timeout");
      return;
    }
    written += chunk;
  }
}

bool I2SAudioSink::setParams(uint32_t sampleRate, uint8_t channelCount,
                             uint8_t bitDepth) {
  if (bitDepth != 16 || channelCount != 2) {
    return false;
  }
  if (sampleRate == sampleRate_) {
    return true;
  }
  i2s_channel_disable(tx_);
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
  esp_err_t err = i2s_channel_reconfig_std_clock(tx_, &clk);
  i2s_channel_enable(tx_);
  if (err != ESP_OK) {
    return false;
  }
  sampleRate_ = sampleRate;
  return true;
}
