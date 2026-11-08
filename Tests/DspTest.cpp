// Exercise the same double-precision buffer contract as the score executor.
// Keep the factory defaults: silence and noise generation are valid effects, and
// arbitrary parameter extremes can deliberately produce unstable feedback.
#include <ossia/math/safe_math.hpp>

#include <Airwindows/Registry.hpp>
#include <catch2/catch_test_macros.hpp>

#include <AirwinRegistry.h>
#include <airwin_consolidated_base.h>
#include <cmath>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace
{
constexpr int guardFrames = 16;
constexpr int maxFrames = 512;
constexpr double guardSample = 0x1.23456789abcp+20;

struct GuardedChannel
{
  std::array<double, guardFrames + maxFrames + guardFrames> samples;

  GuardedChannel() { samples.fill(guardSample); }

  double* data() { return samples.data() + guardFrames; }

  bool prefixIntact() const
  {
    return std::all_of(samples.begin(), samples.begin() + guardFrames, [](double v) {
      return v == guardSample;
    });
  }

  bool suffixIntact(int frames) const
  {
    // Include the unused part of a normal-size buffer, not just its final guard:
    // score also processes short slices at a nonzero tick_start.
    return std::all_of(
        samples.begin() + guardFrames + frames, samples.end(),
        [](double v) { return v == guardSample; });
  }
};

enum class Processing
{
  StereoOutOfPlace,
  StereoInPlace,
  MonoInPlace
};

const char* name(Processing processing)
{
  switch(processing)
  {
    case Processing::StereoOutOfPlace:
      return "stereo out of place";
    case Processing::StereoInPlace:
      return "stereo in place";
    case Processing::MonoInPlace:
      return "mono in place with silent second channel";
  }
  return "unknown";
}
}

TEST_CASE(
    "Every airwindows effect processes bounded double buffers safely",
    "[airwindows][dsp]")
{
  Airwindows::initializeRegistry();
  REQUIRE(!AirwinRegistry::registry.empty());

  for(const auto& effect : AirwinRegistry::registry)
  {
    CAPTURE(effect.name);
    for(const float sampleRate : {44100.f, 48000.f, 96000.f})
    {
      CAPTURE(sampleRate);
      for(const auto processing :
          {Processing::StereoOutOfPlace, Processing::StereoInPlace,
           Processing::MonoInPlace})
      {
        INFO(name(processing));
        // Upstream constructors seed their dither with rand(). This executable
        // runs serially; reset before each fresh instance so order is irrelevant.
        std::srand(0);
        auto plugin = effect.generator();
        REQUIRE(plugin);
        plugin->setSampleRate(sampleRate);

        int sampleOffset = 0;
        // Test both a fresh silent instance and its tail after excitation, without
        // resetting its filters, delay lines or feedback between process calls.
        for(const bool signal : {false, true, false})
        {
          CAPTURE(signal, sampleOffset);
          // Odd and single-frame slices also follow normal blocks, exercising
          // persistent state when score's execution slice changes size.
          for(const int frames : {1, 17, 64, 256, maxFrames, 17, 1})
          {
            CAPTURE(frames);
            std::array<GuardedChannel, 2> inputs;
            std::array<GuardedChannel, 2> outputs;
            for(int channel = 0; channel < 2; ++channel)
            {
              for(int frame = 0; frame < frames; ++frame)
              {
                const double time = (sampleOffset + frame) / double(sampleRate);
                // Different, bounded signals on each side expose stereo indexing
                // bugs. Mono execution gives the effect a silent scratch channel.
                const bool active
                    = signal && !(processing == Processing::MonoInPlace && channel == 1);
                inputs[channel].data()[frame]
                    = active
                          ? 0.2 * std::sin(time * (channel == 0 ? 2764. : 4346.) + 0.3)
                                + 0.05 * std::cos(time * 773. + channel)
                          : 0.;
                outputs[channel].data()[frame]
                    = std::numeric_limits<double>::quiet_NaN();
              }
            }

            const auto originalInputs = inputs;
            double* inputPointers[]{inputs[0].data(), inputs[1].data()};
            double* outputPointers[]{outputs[0].data(), outputs[1].data()};
            const bool inPlace = processing != Processing::StereoOutOfPlace;
            plugin->processDoubleReplacing(
                inputPointers, inPlace ? inputPointers : outputPointers, frames);

            for(int channel = 0; channel < 2; ++channel)
            {
              CAPTURE(channel);
              auto& result = inPlace ? inputs[channel] : outputs[channel];
              const auto end = result.data() + frames;
              const auto nonfinite = std::find_if(result.data(), end, [](double value) {
                // Unlike std::isfinite, these survive the host's fast-math flags.
                return ossia::safe_isnan(value) || ossia::safe_isinf(value);
              });
              const auto firstNonfiniteFrame = nonfinite - result.data();
              CAPTURE(firstNonfiniteFrame);
              CHECK(nonfinite == end);
              CHECK(result.prefixIntact());
              CHECK(result.suffixIntact(frames));
              if(!inPlace)
              {
                // Covers both the active input slice and its surrounding guards.
                CHECK(inputs[channel].samples == originalInputs[channel].samples);
              }
            }
            sampleOffset += frames;
          }
        }
      }
    }
  }
}
