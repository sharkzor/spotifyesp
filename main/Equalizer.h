#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

// Parametric equalizer: a pre-gain followed by up to six biquad bands, applied to
// interleaved 16-bit stereo. The settings can be changed from any thread while audio
// is playing; the audio thread picks them up at the start of its next buffer.
namespace eq {

constexpr int MAX_BANDS = 6;

enum class Type : uint8_t { Peak, LowShelf, HighShelf };

struct Band {
  Type type = Type::Peak;
  float freq = 1000.0f;  // Hz
  float q = 1.0f;
  float gainDb = 0.0f;  // 0 dB means the band is skipped
};

struct Config {
  bool enabled = false;
  float preampDb = 0.0f;
  Band bands[MAX_BANDS];
};

// Text form used for storage and for the web page:
//   "<enabled 0|1>;<preamp dB>;<P|L|H>,<Hz>,<Q>,<dB>;" and then the other bands.
// parse() rejects malformed text and clamps values to sane ranges.
bool parse(const std::string& text, Config& out);
std::string serialize(const Config& config);

class Equalizer {
 public:
  void setConfig(const Config& config);
  Config config() const;
  void setSampleRate(uint32_t sampleRate);

  // Filters `samples` int16 values in place (two per frame). Audio thread only.
  void process(int16_t* interleavedStereo, size_t samples);

 private:
  struct Coef {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  struct State {
    float z1[2] = {0, 0};
    float z2[2] = {0, 0};
  };
  struct Plan {
    bool active = false;
    float preamp = 1.0f;
    int count = 0;
    Coef coef[MAX_BANDS];
  };

  Plan makePlan() const;  // call with mutex_ held

  mutable std::mutex mutex_;
  Config config_;
  uint32_t sampleRate_ = 44100;
  Plan pending_;
  bool dirty_ = false;

  Plan plan_;
  State state_[MAX_BANDS];
};

Equalizer& instance();

}  // namespace eq
