#pragma once
#include <QString>

#include <cstdint>
#include <vector>

namespace Airwindows
{

/**
 * @brief How a normalized [0;1] airwindows parameter maps to a value a human can read.
 *
 * Airwindows plug-ins are VST2-shaped: every parameter is a bare float in [0;1], and
 * there is no static per-parameter metadata anywhere - AirwinRegistry::awReg only knows
 * the parameter *count*. What the effects do expose, per parameter, is:
 *
 * - getParameterName / getParameterLabel: the name and the unit ("dB", "hz", "%"...)
 * - getParameterDisplay: the value in those units, or an enumerator label
 * - canConvertParameterTextToValue: usually false only for a switch over choices
 *
 * That is enough to *derive* the metadata: we sweep the normalized range once, read the
 * displayed values back, and fit the resulting curve. Every kind but Curve is a closed
 * form, which is what the audio thread evaluates per incoming value.
 */
enum class ParameterKind : std::uint8_t
{
  Normalized, //!< No usable mapping found: the control stays a bare [0;1] slider
  Linear,     //!< display = offset + scale * n
  Power,      //!< display = offset + scale * n^exponent
  Decibel,    //!< display = max(decibelFloor, offset + 20*log10(n))
  Integer,    //!< Integral display values, as a staircase over the normalized range
  Enum,       //!< Discrete labelled choices; the control value is the choice index
  Curve       //!< Monotonic but none of the above; mapped through the sampled table
};

/**
 * @brief Where dB2string stops printing numbers.
 *
 * It prints "-inf" from -100 dB downwards (see airwin_consolidated_base.h), so that is
 * as low as a decibel parameter can be read back, and it stands in for silence.
 */
inline constexpr double decibelFloor = -100.;

/**
 * @brief One discrete choice of an Enum or Integer parameter.
 *
 * @c normalized is the middle of the plateau of normalized values that display as this
 * step, so that writing it back to the effect is stable. @c lowerBound is where the
 * plateau starts, which makes the normalized -> display direction exact.
 */
struct ParameterStep
{
  double display{};   //!< Choice index for Enum, the integer itself for Integer
  float normalized{}; //!< Middle of the plateau
  float lowerBound{}; //!< Start of the plateau
  QString label;      //!< Enum only; empty for Integer
};

struct ParameterMetadata
{
  QString name;
  QString unit; //!< getParameterLabel, e.g. "dB", "hz", "%". Often empty.
  ParameterKind kind{ParameterKind::Normalized};

  //! Value the effect itself starts up with, in [0;1]
  float defaultNormalized{0.5f};

  //! Bounds in *display* units. [0;1] for Normalized, [0;count-1] for Enum.
  double min{0.};
  double max{1.};

  //! Linear, Power and Decibel coefficients. @c scale may be negative.
  double offset{0.};
  double scale{1.};
  double exponent{1.};

  //! Enum and Integer only, ordered by ascending @c normalized.
  std::vector<ParameterStep> steps;

  //! Curve only: display sampled at normalized = i / (curve.size() - 1)
  std::vector<double> curve;

  //! Name with the unit appended, e.g. "Boost (dB)". This is what the port is named.
  QString displayName() const;

  //! True when the [0;1] domain means the same thing in display units.
  bool isIdentity() const noexcept;

  //! A slider over this range reads better logarithmically.
  bool prefersLogarithmicWidget() const noexcept;

  //! Both directions are total: non-finite input maps to the bottom of the range.
  double toDisplay(double normalized) const noexcept;
  double toNormalized(double display) const noexcept;

  double defaultDisplay() const noexcept { return toDisplay(defaultNormalized); }
};

/**
 * @brief Derived metadata for every parameter of a plug-in, probed once and cached.
 *
 * Returns an empty vector for an out-of-range index or an unknown name. Probing
 * instantiates the effect at 48 kHz: a handful of plug-ins display frequencies derived
 * from the sample rate, so their bounds are the ones they would show at 48 kHz.
 *
 * Not noexcept and not cheap on the first call for a plug-in: it sweeps the effect
 * thousands of times and allocates. Call it off the audio thread; hold the returned
 * reference, which stays valid for the life of the process.
 */
const std::vector<ParameterMetadata>& parameterMetadata(int pluginIndex);
const std::vector<ParameterMetadata>& parameterMetadata(const QString& pluginName);

namespace detail
{
//! Probe without consulting or filling the cache. For tests.
std::vector<ParameterMetadata> probeParameterMetadata(int pluginIndex);
}

}
