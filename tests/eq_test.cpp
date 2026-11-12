#include "Equalizer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                      \
  do {                                        \
    if (!(cond)) {                            \
      std::printf("FAIL: " __VA_ARGS__);      \
      std::printf("\n");                      \
      failures++;                             \
    }                                         \
  } while (0)

// Returns the output level in dB relative to a sine of the given frequency and amplitude.
static double gainAt(eq::Equalizer& e, double freq, double amp = 8000.0) {
  const double fs = 44100;
  const int frames = 44100;
  std::vector<int16_t> buf(frames * 2);
  for (int i = 0; i < frames; i++) {
    int16_t v = static_cast<int16_t>(std::lrint(amp * std::sin(2 * M_PI * freq * i / fs)));
    buf[2 * i] = buf[2 * i + 1] = v;
  }
  e.process(buf.data(), buf.size());
  double sum = 0;
  for (int i = frames / 2; i < frames; i++) sum += double(buf[2 * i]) * buf[2 * i];
  double rms = std::sqrt(sum / (frames - frames / 2));
  return 20 * std::log10(rms / (amp / std::sqrt(2.0)));
}

static void setup(eq::Equalizer& e, const char* text) {
  eq::Config c;
  if (!eq::parse(text, c)) {
    std::printf("FAIL: could not parse %s\n", text);
    std::exit(1);
  }
  e.setConfig(c);
}

int main() {
  {  // peak
    eq::Equalizer e; setup(e, "1;0;P,1000,1.0,6.0;P,1000,1.0,0.0;P,1000,1.0,0.0;P,1000,1.0,0.0;P,1000,1.0,0.0;P,1000,1.0,0.0");
    double at = gainAt(e, 1000), far = gainAt(e, 60);
    CHECK(std::fabs(at - 6.0) < 0.2, "peak centre %.2f dB", at);
    CHECK(std::fabs(far) < 0.3, "peak far %.2f dB", far);
  }
  {  // low shelf cut
    eq::Equalizer e; setup(e, "1;0;L,200,0.7,-9.0");
    double lo = gainAt(e, 40), hi = gainAt(e, 5000);
    CHECK(std::fabs(lo + 9.0) < 0.5, "shelf low %.2f dB", lo);
    CHECK(std::fabs(hi) < 0.3, "shelf high %.2f dB", hi);
  }
  {  // high shelf boost
    eq::Equalizer e; setup(e, "1;0;H,4000,0.7,5.0");
    double hi = gainAt(e, 16000, 4000), lo = gainAt(e, 100);
    CHECK(std::fabs(hi - 5.0) < 0.5, "hishelf high %.2f dB", hi);
    CHECK(std::fabs(lo) < 0.3, "hishelf low %.2f dB", lo);
  }
  {  // preamp only
    eq::Equalizer e; setup(e, "1;-6.0");
    double g = gainAt(e, 440);
    CHECK(std::fabs(g + 6.0) < 0.1, "preamp %.2f dB", g);
  }
  {  // disabled is a bit-exact bypass
    eq::Equalizer e; setup(e, "0;-6.0;P,1000,1.0,6.0");
    int16_t buf[8] = {100, -100, 32767, -32768, 5, 6, 7, 8};
    int16_t ref[8];
    for (int i = 0; i < 8; i++) ref[i] = buf[i];
    e.process(buf, 8);
    for (int i = 0; i < 8; i++) CHECK(buf[i] == ref[i], "bypass sample %d", i);
  }
  {  // boost clips instead of wrapping
    eq::Equalizer e; setup(e, "1;0;P,1000,1.0,15.0");
    std::vector<int16_t> buf(4096);
    for (size_t i = 0; i < buf.size(); i += 2) buf[i] = buf[i + 1] = (i / 2) % 2 ? 30000 : -30000;
    e.process(buf.data(), buf.size());
    for (size_t i = 0; i < buf.size(); i++) CHECK(buf[i] >= -32768 && buf[i] <= 32767, "range");
  }
  {  // sample rate change keeps the centre gain
    eq::Equalizer e; setup(e, "1;0;P,1000,1.0,6.0");
    e.setSampleRate(48000);
    const double fs = 48000;
    std::vector<int16_t> buf(48000 * 2);
    for (int i = 0; i < 48000; i++) buf[2 * i] = buf[2 * i + 1] = (int16_t)std::lrint(8000 * std::sin(2 * M_PI * 1000 * i / fs));
    e.process(buf.data(), buf.size());
    double sum = 0;
    for (int i = 24000; i < 48000; i++) sum += double(buf[2 * i]) * buf[2 * i];
    double g = 20 * std::log10(std::sqrt(sum / 24000) / (8000 / std::sqrt(2.0)));
    CHECK(std::fabs(g - 6.0) < 0.2, "48k centre %.2f dB", g);
  }
  {  // text round trip and rejection of bad input
    eq::Config c;
    CHECK(eq::parse("1;-3.5;P,250,1.50,4.0;L,80,0.70,-2.0", c), "parse ok");
    CHECK(c.enabled && c.bands[1].type == eq::Type::LowShelf, "parsed fields");
    eq::Config d;
    CHECK(eq::parse(eq::serialize(c), d), "reparse");
    CHECK(eq::serialize(d) == eq::serialize(c), "round trip");
    CHECK(!eq::parse("", c), "empty");
    CHECK(!eq::parse("2;0", c), "bad flag");
    CHECK(!eq::parse("1;abc", c), "bad preamp");
    CHECK(!eq::parse("1;0;X,1000,1,1", c), "bad type");
    CHECK(!eq::parse("1;0;P,1000,1", c), "short band");
    CHECK(!eq::parse("1;0;P,nan,1,1", c), "nan");
    CHECK(eq::parse("1;99;P,1,99,99", c), "clamp parse");
    CHECK(c.preampDb == 6.0f && c.bands[0].freq == 20.0f && c.bands[0].q == 10.0f &&
              c.bands[0].gainDb == 15.0f, "clamped");
  }
  std::printf(failures ? "EQ tests FAILED\n" : "EQ tests passed\n");
  return failures ? 1 : 0;
}
