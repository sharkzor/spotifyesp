#include "Equalizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace eq {
namespace {

float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

char typeChar(Type t) {
  return t == Type::LowShelf ? 'L' : t == Type::HighShelf ? 'H' : 'P';
}

bool parseFloat(const std::string& s, float& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  float v = std::strtof(s.c_str(), &end);
  if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
  out = v;
  return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      return parts;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

}  // namespace

bool parse(const std::string& text, Config& out) {
  auto fields = split(text, ';');
  while (!fields.empty() && fields.back().empty()) fields.pop_back();
  if (fields.size() < 2 || fields.size() > 2 + MAX_BANDS) return false;
  if (fields[0] != "0" && fields[0] != "1") return false;

  Config c;
  c.enabled = fields[0] == "1";
  if (!parseFloat(fields[1], c.preampDb)) return false;
  c.preampDb = clampf(c.preampDb, -24.0f, 6.0f);

  for (size_t i = 2; i < fields.size(); i++) {
    auto v = split(fields[i], ',');
    if (v.size() != 4 || v[0].size() != 1) return false;
    Band& b = c.bands[i - 2];
    switch (v[0][0]) {
      case 'P': b.type = Type::Peak; break;
      case 'L': b.type = Type::LowShelf; break;
      case 'H': b.type = Type::HighShelf; break;
      default: return false;
    }
    if (!parseFloat(v[1], b.freq) || !parseFloat(v[2], b.q) ||
        !parseFloat(v[3], b.gainDb)) {
      return false;
    }
    b.freq = clampf(b.freq, 20.0f, 20000.0f);
    b.q = clampf(b.q, 0.1f, 10.0f);
    b.gainDb = clampf(b.gainDb, -15.0f, 15.0f);
  }
  out = c;
  return true;
}

std::string serialize(const Config& c) {
  char buf[48];
  std::string s = c.enabled ? "1;" : "0;";
  snprintf(buf, sizeof(buf), "%.1f", c.preampDb);
  s += buf;
  for (const Band& b : c.bands) {
    snprintf(buf, sizeof(buf), ";%c,%.0f,%.2f,%.1f", typeChar(b.type), b.freq, b.q,
             b.gainDb);
    s += buf;
  }
  return s;
}

Equalizer::Plan Equalizer::makePlan() const {
  Plan p;
  const double fs = sampleRate_;
  p.preamp = std::pow(10.0f, config_.preampDb / 20.0f);
  for (const Band& band : config_.bands) {
    if (std::fabs(band.gainDb) < 0.05f) continue;
    // Audio EQ Cookbook (R. Bristow-Johnson) coefficients.
    double f = std::min<double>(band.freq, fs * 0.45);
    double A = std::pow(10.0, band.gainDb / 40.0);
    double w0 = 2.0 * M_PI * f / fs;
    double cs = std::cos(w0);
    double alpha = std::sin(w0) / (2.0 * band.q);
    double b0, b1, b2, a0, a1, a2;
    if (band.type == Type::Peak) {
      b0 = 1 + alpha * A;
      b1 = -2 * cs;
      b2 = 1 - alpha * A;
      a0 = 1 + alpha / A;
      a1 = -2 * cs;
      a2 = 1 - alpha / A;
    } else {
      double t = 2.0 * std::sqrt(A) * alpha;
      if (band.type == Type::LowShelf) {
        b0 = A * ((A + 1) - (A - 1) * cs + t);
        b1 = 2 * A * ((A - 1) - (A + 1) * cs);
        b2 = A * ((A + 1) - (A - 1) * cs - t);
        a0 = (A + 1) + (A - 1) * cs + t;
        a1 = -2 * ((A - 1) + (A + 1) * cs);
        a2 = (A + 1) + (A - 1) * cs - t;
      } else {
        b0 = A * ((A + 1) + (A - 1) * cs + t);
        b1 = -2 * A * ((A - 1) + (A + 1) * cs);
        b2 = A * ((A + 1) + (A - 1) * cs - t);
        a0 = (A + 1) - (A - 1) * cs + t;
        a1 = 2 * ((A - 1) - (A + 1) * cs);
        a2 = (A + 1) - (A - 1) * cs - t;
      }
    }
    Coef& c = p.coef[p.count++];
    c.b0 = static_cast<float>(b0 / a0);
    c.b1 = static_cast<float>(b1 / a0);
    c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0);
    c.a2 = static_cast<float>(a2 / a0);
  }
  p.active = config_.enabled && (p.count > 0 || std::fabs(config_.preampDb) >= 0.05f);
  return p;
}

void Equalizer::setConfig(const Config& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  pending_ = makePlan();
  dirty_ = true;
}

Config Equalizer::config() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void Equalizer::setSampleRate(uint32_t sampleRate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sampleRate == sampleRate_) return;
  sampleRate_ = sampleRate;
  pending_ = makePlan();
  dirty_ = true;
}

void Equalizer::process(int16_t* data, size_t samples) {
  if (mutex_.try_lock()) {
    if (dirty_) {
      plan_ = pending_;
      dirty_ = false;
    }
    mutex_.unlock();
  }
  if (!plan_.active) return;

  for (size_t i = 0; i + 1 < samples; i += 2) {
    for (int ch = 0; ch < 2; ch++) {
      float x = static_cast<float>(data[i + ch]) * plan_.preamp;
      for (int b = 0; b < plan_.count; b++) {
        const Coef& c = plan_.coef[b];
        State& s = state_[b];
        float y = c.b0 * x + s.z1[ch];  // transposed direct form II
        s.z1[ch] = c.b1 * x - c.a1 * y + s.z2[ch];
        s.z2[ch] = c.b2 * x - c.a2 * y;
        x = y;
      }
      long v = std::lrintf(x);
      data[i + ch] = static_cast<int16_t>(std::min(32767L, std::max(-32768L, v)));
    }
  }
}

Equalizer& instance() {
  static Equalizer equalizer;
  return equalizer;
}

}  // namespace eq
