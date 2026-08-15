// Derived airwindows parameter metadata.
//
// Airwindows effects publish no per-parameter metadata: every parameter is a bare
// float in [0;1]. Airwindows::probeParameterMetadata() recovers the real range, the
// unit, the discrete choices and the default by sweeping the effect and reading
// getParameterDisplay() back. These tests pin that inference down against effects
// whose display code is known, and then assert the mapping invariants over the whole
// registry.

#include <Airwindows/Metadata.hpp>
#include <Airwindows/Registry.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <AirwinRegistry.h>
#include <cmath>

#include <limits>

using Catch::Approx;
using namespace Airwindows;

namespace
{
const std::vector<ParameterMetadata>& metadataFor(const char* name)
{
  initializeRegistry();
  return parameterMetadata(QString::fromLatin1(name));
}
}

TEST_CASE("The airwindows registry is populated", "[airwindows][metadata]")
{
  initializeRegistry();

  REQUIRE(!AirwinRegistry::registry.empty());
  REQUIRE(AirwinRegistry::nameToIndex.count("ADClip7") == 1);
  CHECK(parameterMetadata(QStringLiteral("NoSuchEffect")).empty());
  CHECK(parameterMetadata(-1).empty());
}

TEST_CASE("Affine parameters expose their real range", "[airwindows][metadata]")
{
  // ADClip7: float2string(A*18.0) with the label "dB"
  const auto& adclip = metadataFor("ADClip7");
  REQUIRE(adclip.size() == 4);

  const auto& boost = adclip[0];
  CHECK(boost.name == "Boost");
  CHECK(boost.unit == "dB");
  CHECK(boost.kind == ParameterKind::Linear);
  CHECK(boost.min == Approx(0.).margin(1e-3));
  CHECK(boost.max == Approx(18.).margin(1e-3));
  CHECK(boost.displayName() == "Boost (dB)");
  CHECK(boost.toDisplay(0.5) == Approx(9.).margin(1e-3));
  CHECK(boost.toNormalized(9.) == Approx(0.5).margin(1e-4));

  // Out of range on either side saturates rather than wrapping
  CHECK(boost.toNormalized(-100.) == Approx(0.));
  CHECK(boost.toNormalized(1000.) == Approx(1.));

  // Compresaturator: float2string((A*24.0)-12.0), i.e. a *negative* lower bound
  const auto& compre = metadataFor("Compresaturator");
  REQUIRE(compre.size() == 5);

  const auto& drive = compre[0];
  CHECK(drive.name == "Drive");
  CHECK(drive.kind == ParameterKind::Linear);
  CHECK(drive.min == Approx(-12.).margin(1e-3));
  CHECK(drive.max == Approx(12.).margin(1e-3));
  CHECK(drive.toDisplay(0.5) == Approx(0.).margin(1e-3));
  CHECK(drive.toNormalized(-12.) == Approx(0.).margin(1e-4));

  // float2string(B*100) with the label "%"
  CHECK(compre[1].kind == ParameterKind::Linear);
  CHECK(compre[1].unit == "%");
  CHECK(compre[1].max == Approx(100.).margin(1e-2));
}

TEST_CASE("Parameters that already read 0-1 stay as they are", "[airwindows][metadata]")
{
  // ADClip7's Soften is a plain float2string(B): the display *is* the normalized
  // value, so the control keeps the [0;1] domain it had before this existed.
  const auto& soften = metadataFor("ADClip7")[1];

  CHECK(soften.name == "Soften");
  CHECK(soften.unit.isEmpty());
  CHECK(soften.min == Approx(0.).margin(1e-4));
  CHECK(soften.max == Approx(1.).margin(1e-4));
  CHECK(soften.displayName() == "Soften");
  CHECK(soften.toDisplay(0.375) == Approx(0.375).margin(1e-3));
}

TEST_CASE("Discrete parameters become a list of choices", "[airwindows][metadata]")
{
  // ADClip7's Mode is a switch over (VstInt32)(D * 2.999): three popup entries.
  const auto& mode = metadataFor("ADClip7")[3];

  CHECK(mode.name == "Mode");
  REQUIRE(mode.kind == ParameterKind::Enum);
  REQUIRE(mode.steps.size() == 3);
  CHECK(mode.steps[0].label == "Normal");
  CHECK(mode.steps[1].label == "Atten");
  CHECK(mode.steps[2].label == "Clips");
  CHECK(mode.min == Approx(0.));
  CHECK(mode.max == Approx(2.));

  // The unit is meaningless for a popup, so it is not appended to the name
  CHECK(mode.displayName() == "Mode");

  // The endpoints of the normalized range are the first and last choice
  CHECK(mode.toDisplay(0.) == Approx(0.));
  CHECK(mode.toDisplay(1.) == Approx(2.));

  // Every choice round-trips, and lands somewhere inside its own plateau
  for(int i = 0; i < 3; i++)
  {
    const double n = mode.toNormalized(i);
    CHECK(mode.toDisplay(n) == Approx(i));
    CHECK(n >= mode.steps[i].lowerBound);
  }
}

TEST_CASE("Integer parameters keep their integral range", "[airwindows][metadata]")
{
  // BitShiftGain: int2string((VstInt32)((A * 32) - 16)), labelled "bits"
  const auto& bits = metadataFor("BitShiftGain");
  REQUIRE(bits.size() == 1);

  const auto& shift = bits[0];
  CHECK(shift.name == "BitShift");
  CHECK(shift.unit == "bits");
  REQUIRE(shift.kind == ParameterKind::Integer);
  CHECK(shift.min == Approx(-16.));
  CHECK(shift.max == Approx(16.));
  CHECK(shift.displayName() == "BitShift (bits)");
  CHECK(shift.toDisplay(0.5) == Approx(0.));

  // Every integer in the range is reachable and stable
  for(int k = -16; k <= 16; k++)
    CHECK(shift.toDisplay(shift.toNormalized(k)) == Approx(k));

  // The mapping is a staircase: whole plateaus of normalized values share a step
  CHECK(shift.toDisplay(0.) == Approx(-16.));
  CHECK(shift.toDisplay(1.) == Approx(16.));
}

TEST_CASE("Non-affine parameters get a closed-form fit", "[airwindows][metadata]")
{
  // Compresaturator's Expand is float2string(C*C*5000): quadratic, so neither an
  // affine fit nor a staircase.
  const auto& expand = metadataFor("Compresaturator")[2];

  CHECK(expand.name == "Expand");
  REQUIRE(expand.kind == ParameterKind::Power);
  CHECK(expand.exponent == Approx(2.));
  CHECK(expand.curve.empty()); // closed form: no sampled table retained
  CHECK(expand.min == Approx(0.).margin(1e-2));
  CHECK(expand.max == Approx(5000.).margin(1e-2));

  // 0.5 * 0.5 * 5000
  CHECK(expand.toDisplay(0.5) == Approx(1250.).epsilon(1e-3));

  // The inverse follows the curve, not a straight line between the bounds
  CHECK(expand.toNormalized(1250.) == Approx(0.5).epsilon(1e-3));
  for(double d : {50., 500., 1250., 3000., 4900.})
    CHECK(expand.toDisplay(expand.toNormalized(d)) == Approx(d).epsilon(1e-3));
}

TEST_CASE("Defaults come from the effect itself", "[airwindows][metadata]")
{
  // Every control used to start at 0.5; these are what the constructors actually set.
  const auto& adclip = metadataFor("ADClip7");
  REQUIRE(adclip.size() == 4);
  CHECK(adclip[0].defaultNormalized == Approx(0.0f));
  CHECK(adclip[1].defaultNormalized == Approx(0.5f));
  CHECK(adclip[2].defaultNormalized == Approx(0.5f));
  CHECK(adclip[3].defaultNormalized == Approx(0.0f));

  // ...and they are read before probing starts overwriting parameters.
  const auto& compre = metadataFor("Compresaturator");
  REQUIRE(compre.size() == 5);
  CHECK(compre[3].defaultNormalized == Approx(1.0f));
  CHECK(compre[4].defaultNormalized == Approx(1.0f));

  // Boost defaults to 0 dB, Mode to "Normal", Expand to 1250 samples
  CHECK(adclip[0].defaultDisplay() == Approx(0.).margin(1e-3));
  CHECK(adclip[3].defaultDisplay() == Approx(0.));
  CHECK(compre[2].defaultDisplay() == Approx(1250.).epsilon(1e-3));
}

TEST_CASE("Decibel parameters are recognized as such", "[airwindows][metadata]")
{
  // Fracture's Out Lvl goes through dB2string, which prints "-inf" below -100 dB.
  const auto& fracture = metadataFor("Fracture");
  REQUIRE(fracture.size() == 4);

  const auto& out = fracture[2]; // dB2string(C)
  CHECK(out.name == "Out Lvl");
  REQUIRE(out.kind == ParameterKind::Decibel);
  CHECK(out.curve.empty());
  CHECK(out.min == Approx(decibelFloor));
  CHECK(out.max == Approx(0.).margin(1e-3));

  // Unity is 0 dB, half amplitude is -6.02 dB
  CHECK(out.toDisplay(1.0) == Approx(0.).margin(1e-3));
  CHECK(out.toDisplay(0.5) == Approx(-6.0206).margin(1e-2));
  CHECK(out.toNormalized(-6.0206) == Approx(0.5).epsilon(1e-3));

  // Silence reads as the floor and comes back as silence, not as 10^-5
  CHECK(out.toDisplay(0.) == Approx(decibelFloor));
  CHECK(out.toNormalized(decibelFloor) == Approx(0.));
  CHECK(out.toNormalized(-1e9) == Approx(0.));
}

TEST_CASE("Both mappings are total", "[airwindows][metadata]")
{
  // A cable, an OSC device or a mapping process can hand the executor anything at
  // all. An airwindows effect fed a NaN stays poisoned until the executor is torn
  // down - it cannot reset its own filter state - so neither direction may pass one
  // through. (std::clamp(NaN, lo, hi) returns NaN, which is how this used to leak.)
  const auto quiet_nan = std::numeric_limits<double>::quiet_NaN();
  const auto inf = std::numeric_limits<double>::infinity();

  for(const char* effect : {"ADClip7", "BitShiftGain", "Compresaturator", "Fracture"})
  {
    const auto& params = metadataFor(effect);
    REQUIRE(!params.empty());
    for(const auto& m : params)
    {
      INFO("effect: " << effect << " parameter: " << m.name.toStdString());
      for(double bad : {quiet_nan, inf, -inf})
      {
        CHECK(std::isfinite(m.toNormalized(bad)));
        CHECK(m.toNormalized(bad) >= 0.);
        CHECK(m.toNormalized(bad) <= 1.);
        CHECK(std::isfinite(m.toDisplay(bad)));
      }
    }
  }
}

TEST_CASE("An unmapped parameter is the identity", "[airwindows][metadata]")
{
  // What a parameter falls back to when nothing could be inferred from it.
  ParameterMetadata m;
  REQUIRE(m.kind == ParameterKind::Normalized);

  CHECK(m.min == Approx(0.));
  CHECK(m.max == Approx(1.));
  CHECK(m.toDisplay(0.3) == Approx(0.3));
  CHECK(m.toNormalized(0.3) == Approx(0.3));
  CHECK(m.toNormalized(-1.) == Approx(0.));
  CHECK(m.toNormalized(2.) == Approx(1.));
}

TEST_CASE("Every parameter of every effect maps consistently", "[airwindows][metadata]")
{
  initializeRegistry();

  const int count = (int)AirwinRegistry::registry.size();
  REQUIRE(count > 100);

  int mapped = 0, total = 0, tables = 0;
  for(int p = 0; p < count; p++)
  {
    const auto& reg = AirwinRegistry::registry[p];
    const auto params = detail::probeParameterMetadata(p);
    INFO("effect: " << reg.name);
    REQUIRE((int)params.size() == reg.nParams);

    for(int i = 0; i < (int)params.size(); i++)
    {
      const auto& m = params[i];
      INFO(
          "effect: " << reg.name << " parameter: " << i << " (" << m.name.toStdString()
                     << ")");
      total++;
      if(m.kind != ParameterKind::Normalized)
        mapped++;
      if(!m.curve.empty())
        tables++;

      CHECK(!m.name.isEmpty());
      CHECK(m.min <= m.max);
      CHECK(m.defaultNormalized >= 0.f);
      CHECK(m.defaultNormalized <= 1.f);

      // A step-shaped parameter is a real list of choices
      if(m.kind == ParameterKind::Enum || m.kind == ParameterKind::Integer)
        CHECK(m.steps.size() >= 2);
      else
        CHECK(m.steps.empty());

      // The bounds are what the ends of the normalized range display as, and
      // nothing in between escapes them.
      const double tolerance = 1e-3 * (m.max - m.min) + 1e-6;
      for(int k = 0; k <= 20; k++)
      {
        const double n = (double)k / 20.;
        const double d = m.toDisplay(n);
        CHECK(d >= m.min - tolerance);
        CHECK(d <= m.max + tolerance);

        // Going back to normalized and forward again lands on the same reading:
        // this is exactly what the executor does with a value coming off a port.
        CHECK(m.toDisplay(m.toNormalized(d)) == Approx(d).margin(tolerance));
      }
    }
  }

  CHECK(total > 1000);
  // Every parameter is a closed form today; the sampled table is a safety net only.
  CHECK(tables == 0);
  // As of the current airwin2rack revision every single parameter can be mapped.
  // A new effect upstream whose display code we cannot read would trip this - so
  // would a regression in the inference itself, which is the point.
  INFO(mapped << " of " << total << " parameters could be mapped");
  CHECK(mapped == total);
}
