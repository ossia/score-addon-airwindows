// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "Metadata.hpp"

#include "Registry.hpp"

#include <ossia/math/safe_math.hpp>

#include <AirwinRegistry.h>
#include <airwin_consolidated_base.h>
#include <cmath>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Airwindows
{
namespace
{
//! std::isfinite compiles to a constant under -ffast-math, which would silently
//! delete every guard below. ossia's version inspects the bits instead.
bool isFinite(double v) noexcept
{
  return !ossia::safe_isnan(v) && !ossia::safe_isinf(v);
}

//! Odd, so that the middle of a plateau always lands on a sample.
constexpr int probe_samples = 2049;

//! float2string uses "%8.4f": four *absolute* decimals of resolution.
constexpr double display_epsilon = 2e-4;

//! Past this, a "staircase" is far more likely to be a finely-sampled ramp.
constexpr int max_steps = 512;
constexpr int max_enumerators = 64;

//! Exponents tried for the Power fit. Everything airwindows prints is in here.
constexpr int min_exponent = 2;
constexpr int max_exponent = 6;

//! getParameterDisplay writes at most kVstMaxParamStrLen bytes, and vst_strncpy
//! does not guarantee a terminator: give it room and zero the buffer every time.
struct DisplayBuffer
{
  char data[2 * kVstMaxParamStrLen]{};

  char* clear() noexcept
  {
    std::fill(std::begin(data), std::end(data), '\0');
    return data;
  }
  std::string_view get() noexcept
  {
    data[std::size(data) - 1] = '\0';
    return std::string_view{data};
  }
};

std::optional<double> parseDisplay(std::string_view txt) noexcept
{
  // Careful: string2float() goes through std::stof, which happily parses "inf".
  float v{};
  if(string2float(txt.data(), v) && isFinite(v))
    return (double)v;

  // dB2string prints "-inf" from decibelFloor downwards, and nothing else in the
  // suite prints an infinity. A *positive* one would be an overflow, not a floor.
  if(txt.find("-inf") != std::string_view::npos)
    return decibelFloor;

  return std::nullopt;
}

//! Contiguous run of normalized values that all display the same thing.
struct Plateau
{
  int begin{};
  int end{}; //!< one past the last sample
};

float sampleToNormalized(int k) noexcept
{
  return (float)((double)k / (double)(probe_samples - 1));
}

ParameterStep makeStep(const Plateau& p, double display, QString label)
{
  return ParameterStep{
      .display = display,
      .normalized = sampleToNormalized((p.begin + p.end - 1) / 2),
      .lowerBound = sampleToNormalized(p.begin),
      .label = std::move(label)};
}

//! Group a sampled sweep into runs of equal value, by whatever "equal" means for it.
template <typename T>
std::vector<Plateau> findPlateaus(const std::vector<T>& samples)
{
  std::vector<Plateau> plateaus;
  for(int k = 0; k < (int)samples.size(); k++)
  {
    if(!plateaus.empty() && samples[k] == samples[plateaus.back().begin])
      plateaus.back().end = k + 1;
    else
      plateaus.push_back({.begin = k, .end = k + 1});
  }
  return plateaus;
}

/**
 * @brief Discrete parameter: sweep the range and group identical display strings.
 *
 * Tried when canConvertParameterTextToValue() is false. That is *usually* a switch
 * over choices, but not always: the generator also skips a parameter whose display
 * it simply failed to parse - `60.0+(A*80.0)` written offset-first, or a case label
 * that is not `kParamA`. Those come out as thousands of distinct strings here, and
 * the caller falls back to fitting them numerically.
 *
 * @return whether the parameter really was a list of choices
 */
bool classifyEnum(
    ParameterMetadata& m, int index, AirwinConsolidatedBase& fx, DisplayBuffer& buf)
{
  // Compared as raw bytes: all but a handful of these are thrown away, and building
  // a couple of thousand QStrings per parameter to discard them is pure waste.
  std::vector<std::string> texts;
  texts.reserve(probe_samples);
  for(int k = 0; k < probe_samples; k++)
  {
    fx.setParameter(index, sampleToNormalized(k));
    fx.getParameterDisplay(index, buf.clear());
    texts.emplace_back(buf.get());
  }

  const auto plateaus = findPlateaus(texts);
  if(plateaus.size() < 2 || plateaus.size() > max_enumerators)
    return false;

  m.kind = ParameterKind::Enum;
  m.min = 0.;
  m.max = (double)plateaus.size() - 1.;
  m.steps.reserve(plateaus.size());
  for(std::size_t i = 0; i < plateaus.size(); i++)
  {
    auto label = QString::fromUtf8(texts[plateaus[i].begin]).trimmed();
    if(label.isEmpty())
      label = QString::number(i);
    m.steps.push_back(makeStep(plateaus[i], (double)i, std::move(label)));
  }
  return true;
}

//! Integral staircase: every sample rounds to itself and there are real plateaus.
bool classifyInteger(ParameterMetadata& m, const std::vector<double>& s)
{
  std::vector<double> rounded;
  rounded.reserve(s.size());
  for(double v : s)
  {
    if(std::abs(v - std::round(v)) > display_epsilon)
      return false;
    rounded.push_back(std::round(v));
  }

  const auto plateaus = findPlateaus(rounded);

  // A ramp finely enough sampled to hit only integers is not a staircase: require
  // that the plateaus are actually wide.
  const int limit = std::min<int>(max_steps, (int)s.size() / 4);
  if((int)plateaus.size() < 2 || (int)plateaus.size() > limit)
    return false;

  m.kind = ParameterKind::Integer;
  m.steps.reserve(plateaus.size());
  for(const auto& p : plateaus)
    m.steps.push_back(makeStep(p, rounded[p.begin], {}));

  std::tie(m.min, m.max) = std::minmax(rounded.front(), rounded.back());
  return true;
}

//! How far a candidate closed form strays from what the effect actually printed.
double worstDeviation(const std::vector<double>& s, auto&& predict)
{
  double worst = 0.;
  const int n = (int)s.size();
  for(int k = 0; k < n; k++)
    worst = std::max(worst, std::abs(s[k] - predict((double)k / (double)(n - 1))));
  return worst;
}

double fitTolerance(const std::vector<double>& s)
{
  return std::max(2. * display_epsilon, 1e-3 * std::abs(s.back() - s.front()));
}

//! display = offset + scale * n, within the resolution of the printout.
bool classifyLinear(ParameterMetadata& m, const std::vector<double>& s)
{
  const double offset = s.front();
  const double scale = s.back() - s.front();
  if(worstDeviation(s, [&](double t) { return offset + scale * t; }) > fitTolerance(s))
    return false;

  m.kind = ParameterKind::Linear;
  m.offset = offset;
  m.scale = scale;
  std::tie(m.min, m.max) = std::minmax(s.front(), s.back());
  return true;
}

/**
 * @brief display = offset + scale * n^k.
 *
 * Airwindows writes these out longhand - `C*C*5000`, `(pow(A,3)*2070)+30`,
 * `((B*B)*(B*B)*148.5)+1.5` - so the exponent is always a small integer and a search
 * over the handful of them fits every one of them exactly.
 */
bool classifyPower(ParameterMetadata& m, const std::vector<double>& s)
{
  const double offset = s.front();
  const double scale = s.back() - s.front();
  const double tolerance = fitTolerance(s);

  for(int k = min_exponent; k <= max_exponent; k++)
  {
    const auto predict = [&](double t) { return offset + scale * std::pow(t, k); };
    if(worstDeviation(s, predict) <= tolerance)
    {
      m.kind = ParameterKind::Power;
      m.offset = offset;
      m.scale = scale;
      m.exponent = k;
      std::tie(m.min, m.max) = std::minmax(s.front(), s.back());
      return true;
    }
  }
  return false;
}

//! display = offset + 20*log10(n), floored: what dB2string prints.
bool classifyDecibel(ParameterMetadata& m, const std::vector<double>& s)
{
  // At n = 1 the log term vanishes, so the last sample *is* the offset.
  const double offset = s.back();
  const auto predict = [&](double t) {
    return t <= 0. ? decibelFloor : std::max(decibelFloor, offset + 20. * std::log10(t));
  };

  // Decibels span a hundred units of mostly-floor: judge against the printout's own
  // resolution rather than the range, which fitTolerance would make far too lax.
  if(worstDeviation(s, predict) > 2. * display_epsilon)
    return false;

  m.kind = ParameterKind::Decibel;
  m.offset = offset;
  m.min = decibelFloor;
  m.max = offset;
  return true;
}

/**
 * @brief Continuous parameter: sample the printed value and fit it.
 *
 * Anything we cannot invert reliably - an unparseable printout, a constant, or a
 * non-monotonic curve - is left Normalized so the control keeps its [0;1] domain
 * rather than lying about its range.
 */
void classifyContinuous(
    ParameterMetadata& m, int index, AirwinConsolidatedBase& fx, DisplayBuffer& buf)
{
  std::vector<double> s;
  s.reserve(probe_samples);
  for(int k = 0; k < probe_samples; k++)
  {
    fx.setParameter(index, sampleToNormalized(k));
    fx.getParameterDisplay(index, buf.clear());
    auto v = parseDisplay(buf.get());
    if(!v)
      return; // stays Normalized
    s.push_back(*v);
  }

  const auto [lo, hi] = std::minmax_element(s.begin(), s.end());
  if(*hi - *lo < display_epsilon)
    return; // constant printout: nothing to map

  // Against the running extreme rather than the previous sample: per-step slack
  // would let a curve drift monotonically-ish downhill across 2000 samples and
  // still pass, and the inverse would then binary-search a table that is not sorted.
  bool ascending = true, descending = true;
  double highest = s.front(), lowest = s.front();
  for(double v : s)
  {
    ascending &= (v >= highest - display_epsilon);
    descending &= (v <= lowest + display_epsilon);
    highest = std::max(highest, v);
    lowest = std::min(lowest, v);
  }
  if(!ascending && !descending)
    return; // not invertible

  if(classifyInteger(m, s))
    return;
  if(classifyLinear(m, s))
    return;
  if(classifyDecibel(m, s))
    return;
  if(classifyPower(m, s))
    return;

  m.kind = ParameterKind::Curve;
  std::tie(m.min, m.max) = std::minmax(s.front(), s.back());
  m.curve = std::move(s);
}

//! Nearest step to a display value. The steps are monotone but may descend.
const ParameterStep*
nearestStep(const std::vector<ParameterStep>& steps, double display) noexcept
{
  const bool ascending = steps.back().display >= steps.front().display;
  const auto it
      = ascending ? std::lower_bound(
                        steps.begin(), steps.end(), display,
                        [](const ParameterStep& s, double d) { return s.display < d; })
                  : std::lower_bound(
                        steps.begin(), steps.end(), display,
                        [](const ParameterStep& s, double d) { return s.display > d; });

  if(it == steps.begin())
    return &*it;
  if(it == steps.end())
    return &steps.back();

  const auto prev = std::prev(it);
  return std::abs(prev->display - display) <= std::abs(it->display - display) ? &*prev
                                                                              : &*it;
}
}

QString ParameterMetadata::displayName() const
{
  // The unit only describes the *mapped* value: appending it to a bare [0;1] slider
  // or to a list of choices would be a lie.
  if(unit.isEmpty() || kind == ParameterKind::Enum || kind == ParameterKind::Normalized)
    return name;
  return QStringLiteral("%1 (%2)").arg(name, unit);
}

bool ParameterMetadata::isIdentity() const noexcept
{
  switch(kind)
  {
    case ParameterKind::Normalized:
      return true;
    case ParameterKind::Linear:
      return std::abs(offset) < 1e-9 && std::abs(scale - 1.) < 1e-9;
    default:
      return false;
  }
}

bool ParameterMetadata::prefersLogarithmicWidget() const noexcept
{
  // A linear slider over a power-law range spends most of its travel in the top
  // octave. score's LogFloatSlider needs a strictly positive minimum.
  return kind == ParameterKind::Power && min > 0. && max > min * 100.;
}

double ParameterMetadata::toDisplay(double normalized) const noexcept
{
  if(!isFinite(normalized))
    return min;

  const double n = std::clamp(normalized, 0., 1.);
  switch(kind)
  {
    case ParameterKind::Linear:
      return offset + scale * n;

    case ParameterKind::Power:
      return offset + scale * std::pow(n, exponent);

    case ParameterKind::Decibel:
      return n <= 0. ? decibelFloor
                     : std::max(decibelFloor, offset + 20. * std::log10(n));

    case ParameterKind::Integer:
    case ParameterKind::Enum: {
      if(steps.empty())
        break;
      auto it = std::upper_bound(
          steps.begin(), steps.end(), n,
          [](double v, const ParameterStep& s) { return v < (double)s.lowerBound; });
      return it == steps.begin() ? steps.front().display : std::prev(it)->display;
    }

    case ParameterKind::Curve: {
      if(curve.size() < 2)
        break;
      const double x = n * (double)(curve.size() - 1);
      const auto i = std::min<std::size_t>((std::size_t)x, curve.size() - 2);
      return curve[i] + (curve[i + 1] - curve[i]) * (x - (double)i);
    }

    case ParameterKind::Normalized:
      break;
  }
  return n;
}

double ParameterMetadata::toNormalized(double display) const noexcept
{
  // A cable, an OSC device or a mapping process can hand us anything at all, and an
  // airwindows effect fed a NaN stays poisoned until the executor is torn down: it
  // has no way to reset its filter state.
  if(!isFinite(display))
    return 0.;

  switch(kind)
  {
    case ParameterKind::Linear:
      if(scale == 0.)
        break;
      return std::clamp((display - offset) / scale, 0., 1.);

    case ParameterKind::Power:
      if(scale == 0.)
        break;
      return std::clamp(
          std::pow(std::max((display - offset) / scale, 0.), 1. / exponent), 0., 1.);

    case ParameterKind::Decibel:
      if(display <= decibelFloor)
        return 0.;
      return std::clamp(std::pow(10., (display - offset) / 20.), 0., 1.);

    case ParameterKind::Integer:
    case ParameterKind::Enum:
      if(steps.empty())
        break;
      return nearestStep(steps, display)->normalized;

    case ParameterKind::Curve: {
      const int n = (int)curve.size();
      if(n < 2)
        break;
      const bool ascending = curve.back() >= curve.front();
      if(ascending ? (display <= curve.front()) : (display >= curve.front()))
        return 0.;
      if(ascending ? (display >= curve.back()) : (display <= curve.back()))
        return 1.;

      int lo = 0, hi = n - 1;
      while(hi - lo > 1)
      {
        const int mid = (lo + hi) / 2;
        const bool before
            = ascending ? (curve[mid] <= display) : (curve[mid] >= display);
        if(before)
          lo = mid;
        else
          hi = mid;
      }
      const double a = curve[lo], b = curve[hi];
      const double f = (a == b) ? 0. : (display - a) / (b - a);
      return std::clamp(((double)lo + f) / (double)(n - 1), 0., 1.);
    }

    case ParameterKind::Normalized:
      break;
  }
  return std::clamp(display, 0., 1.);
}

namespace detail
{
std::vector<ParameterMetadata> probeParameterMetadata(int pluginIndex)
{
  initializeRegistry();

  if(pluginIndex < 0 || pluginIndex >= (int)AirwinRegistry::registry.size())
    return {};

  const auto& reg = AirwinRegistry::registry[pluginIndex];
  auto fx = reg.generator();
  if(!fx)
    return {};

  // A few effects derive the frequency they print from the sample rate.
  fx->setSampleRate(48000.f);

  const int n = std::max(0, reg.nParams);
  std::vector<ParameterMetadata> out(n);

  // Whatever the effect starts up with *is* the default: read every one of them
  // before the probing below starts overwriting parameters.
  for(int i = 0; i < n; i++)
    out[i].defaultNormalized = std::clamp(fx->getParameter(i), 0.f, 1.f);

  DisplayBuffer buf;
  for(int i = 0; i < n; i++)
  {
    auto& m = out[i];

    fx->getParameterName(i, buf.clear());
    m.name = QString::fromUtf8(buf.get()).trimmed();
    if(m.name.isEmpty())
      m.name = QStringLiteral("Param %1").arg(i);

    fx->getParameterLabel(i, buf.clear());
    m.unit = QString::fromUtf8(buf.get()).trimmed();

    if(fx->canConvertParameterTextToValue(i) || !classifyEnum(m, i, *fx, buf))
      classifyContinuous(m, i, *fx, buf);

    // Put it back: an effect whose printout for one parameter depends on another
    // would otherwise be probed against a parameter left pinned at 1.0.
    fx->setParameter(i, m.defaultNormalized);
  }

  return out;
}
}

const std::vector<ParameterMetadata>& parameterMetadata(int pluginIndex)
{
  static const std::vector<ParameterMetadata> none;
  static std::mutex mut;
  // Node-based: rehashing relinks nodes but never moves a stored value_type, so a
  // reference handed out here stays valid - and readable from the audio thread -
  // however many other plug-ins get probed afterwards. Nothing is ever erased.
  static std::unordered_map<int, std::vector<ParameterMetadata>> cache;

  initializeRegistry();

  if(pluginIndex < 0 || pluginIndex >= (int)AirwinRegistry::registry.size())
    return none;

  std::lock_guard lock{mut};
  auto it = cache.find(pluginIndex);
  if(it == cache.end())
    it = cache.emplace(pluginIndex, detail::probeParameterMetadata(pluginIndex)).first;
  return it->second;
}

const std::vector<ParameterMetadata>& parameterMetadata(const QString& pluginName)
{
  static const std::vector<ParameterMetadata> none;

  initializeRegistry();
  auto it = AirwinRegistry::nameToIndex.find(pluginName.toStdString());
  if(it == AirwinRegistry::nameToIndex.end())
    return none;
  return parameterMetadata(it->second);
}

}
