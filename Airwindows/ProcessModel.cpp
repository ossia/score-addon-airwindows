// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>

#include <score/tools/IdentifierGeneration.hpp>
#include <score/tools/SafeCast.hpp>

#include <Airwindows/ProcessFactory.hpp>
#include <Airwindows/ProcessMetadata.hpp>
#include <Airwindows/Registry.hpp>

#include <cmath>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Airwindows::ProcessModel)

namespace Airwindows
{
namespace
{
UuidKey<Process::Port> expectedControlKey(const ParameterMetadata& meta) noexcept
{
  switch(meta.kind)
  {
    case ParameterKind::Integer:
      return Metadata<ConcreteKey_k, Process::IntSlider>::get();
    case ParameterKind::Enum:
      return Metadata<ConcreteKey_k, Process::ComboBox>::get();
    default:
      break;
  }
  return meta.prefersLogarithmicWidget()
             ? Metadata<ConcreteKey_k, Process::LogFloatSlider>::get()
             : Metadata<ConcreteKey_k, Process::FloatSlider>::get();
}

bool sameBound(double lhs, double rhs) noexcept
{
  // The port stores its domain as float, so compare with the slack a float
  // round-trip of that magnitude actually costs. An absolute epsilon would start
  // reporting spurious mismatches - and therefore spurious rebuilds - as soon as a
  // bound got large.
  return std::abs(lhs - rhs) <= 1e-6 * std::max({1., std::abs(lhs), std::abs(rhs)});
}

//! Does this control already expose exactly what the metadata describes?
bool matchesMetadata(
    const Process::ControlInlet& inlet, const ParameterMetadata& meta) noexcept
{
  if(inlet.concreteKey() != expectedControlKey(meta))
    return false;

  if(meta.kind == ParameterKind::Enum)
  {
    // By label, not by count: upstream renaming or reordering two choices while
    // keeping their number would otherwise leave the combo showing stale text for
    // values the effect now reads differently.
    auto& box = safe_cast<const Process::ComboBox&>(inlet);
    if(box.alternatives.size() != meta.steps.size())
      return false;
    for(std::size_t i = 0; i < meta.steps.size(); i++)
      if(box.alternatives[i].first != meta.steps[i].label)
        return false;
    return true;
  }

  const auto& dom = inlet.domain().get();
  return sameBound(dom.convert_min<double>(), meta.min)
         && sameBound(dom.convert_max<double>(), meta.max);
}

/**
 * @brief Is this the [0;1] slider that format 1 wrote for every parameter?
 *
 * The one thing that distinguishes a stored *normalized* value from a stored
 * *display* value, for documents old enough not to carry a version.
 */
bool looksLikeLegacyControl(const Process::ControlInlet& inlet) noexcept
{
  if(inlet.concreteKey() != Metadata<ConcreteKey_k, Process::FloatSlider>::get())
    return false;

  const auto& dom = inlet.domain().get();
  return sameBound(dom.convert_min<double>(), 0.)
         && sameBound(dom.convert_max<double>(), 1.);
}

/**
 * @brief Carry a value that is already in display units into changed metadata.
 *
 * Reached when an airwin2rack update moved a parameter under a document that was
 * already format 2. The value keeps its meaning; only its representation has to be
 * fitted to the new control.
 */
double remapDisplayValue(
    const Process::ControlInlet& inlet, const ParameterMetadata& meta, double stored)
{
  if(meta.kind == ParameterKind::Enum)
  {
    // The stored value is an index into the *old* list, which the port still has.
    // Follow the label, so a choice that merely moved keeps selecting itself.
    if(inlet.concreteKey() == Metadata<ConcreteKey_k, Process::ComboBox>::get())
    {
      const auto& old = safe_cast<const Process::ComboBox&>(inlet).alternatives;
      const auto index = (std::size_t)std::clamp<long long>(
          std::llround(stored), 0, std::max<long long>(0, (long long)old.size() - 1));
      if(index < old.size())
      {
        for(std::size_t i = 0; i < meta.steps.size(); i++)
          if(meta.steps[i].label == old[index].first)
            return (double)i;
      }
    }
  }
  return std::clamp(stored, meta.min, meta.max);
}

//! Everything a port carries that is the user's, not ours. Mirrors copy_port().
void carryOverPortState(const Process::ControlInlet& from, Process::ControlInlet& to)
{
  to.displayHandledExplicitly = from.displayHandledExplicitly;
  to.setAddress(from.address());
  to.setExposed(from.exposed());
  to.setDescription(from.description());

  // Cables are not stored on the port; the document re-attaches them by path after
  // loading, which finds the new port because it keeps the old one's id.
}
}

Process::Descriptor ProcessFactory::descriptor(QString txt) const noexcept
{
  Process::Descriptor d
      = Metadata<Process::Descriptor_k, Airwindows::ProcessModel>::get();
  auto plug_index = AirwinRegistry::nameToIndex.find(txt.toStdString());
  if(plug_index == AirwinRegistry::nameToIndex.end())
    return d;
  const auto& plug = AirwinRegistry::registry[plug_index->second];

  d.description = QString::fromStdString(plug.whatText);
  for(auto& col : plug.collections)
    d.tags.push_back(QString::fromStdString(col));
  d.documentationLink = "https://www.airwindows.com/" + txt;

  return d;
}

ProcessModel::ProcessModel(
    const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
    QObject* parent)
    : Process::ProcessModel{
          duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
    , m_pluginName{data}
{
  metadata().setInstanceName(*this);

  m_inlets.push_back(new Process::AudioInlet{"Audio In", Id<Process::Port>(0), this});

  auto out = new Process::AudioOutlet{"Audio Out", Id<Process::Port>(0), this};
  out->setPropagate(true);
  m_outlets.push_back(out);

  init();
  createControls();
}

Process::ProcessFlags ProcessModel::flags() const noexcept
{
  auto f = Metadata<Process::ProcessFlags_k, ProcessModel>::get();
  if(!reg)
    return f;

  if(reg->isMono)
    f |= Process::ProcessFlags::PolyphonySupported;

  return f;
}

ProcessModel::~ProcessModel() { }

QString ProcessModel::prettyName() const noexcept
{
  return m_pluginName.isEmpty() ? "Airwindows"
                                : QString("Airwindows %1").arg(m_pluginName);
}

void ProcessModel::init()
{
  initializeRegistry();

  if(auto it = AirwinRegistry::nameToIndex.find(m_pluginName.toStdString());
     it != AirwinRegistry::nameToIndex.end())
    m_pluginIndex = it->second;

  if(m_pluginIndex < 0 || m_pluginIndex >= (int)AirwinRegistry::registry.size())
  {
    m_pluginIndex = -1;
    reg = nullptr;
    return;
  }

  reg = &AirwinRegistry::registry[m_pluginIndex];
}

const std::vector<ParameterMetadata>& ProcessModel::parameters() const
{
  return parameterMetadata(m_pluginIndex);
}

Process::ControlInlet* ProcessModel::makeControl(
    const ParameterMetadata& meta, Id<Process::Port> id, double displayValue)
{
  const auto name = meta.displayName();

  switch(meta.kind)
  {
    case ParameterKind::Enum: {
      const int count = (int)meta.steps.size();
      std::vector<std::pair<QString, ossia::value>> alternatives;
      alternatives.reserve(count);
      for(int i = 0; i < count; i++)
        alternatives.emplace_back(meta.steps[i].label, i);

      const int index = std::clamp((int)std::llround(displayValue), 0, count - 1);
      return new Process::ComboBox{
          std::move(alternatives), index, name, std::move(id), this};
    }

    case ParameterKind::Integer: {
      const int lo = (int)meta.min, hi = (int)meta.max;
      return new Process::IntSlider{lo,
                                    hi,
                                    std::clamp((int)std::llround(displayValue), lo, hi),
                                    name,
                                    std::move(id),
                                    this};
    }

    default:
      break;
  }

  const auto lo = (float)meta.min, hi = (float)meta.max;
  const auto v = (float)std::clamp(displayValue, meta.min, meta.max);
  if(meta.prefersLogarithmicWidget())
    return new Process::LogFloatSlider{lo, hi, v, name, std::move(id), this};
  return new Process::FloatSlider{lo, hi, v, name, std::move(id), this};
}

void ProcessModel::createControls()
{
  const auto& meta = parameters();
  for(int i = 0; i < (int)meta.size(); i++)
    addControl(i, meta[i].defaultNormalized);
}

void ProcessModel::migrateControls()
{
  const auto& meta = parameters();

  // Nothing to reconcile against: an unknown plug-in, or a probe that came back
  // empty for one that does have parameters. Leave whatever the file held alone
  // rather than throwing the user's settings away on the next save.
  if(!reg || (meta.empty() && reg->nParams > 0))
    return;

  // A parameter that no longer exists loses its control...
  while(m_inlets.size() > meta.size() + 1)
  {
    delete m_inlets.back();
    m_inlets.pop_back();
  }

  // Inlet 0 is the audio input; control N sits at inlet N + 1.
  for(std::size_t i = 1; i < m_inlets.size(); i++)
  {
    const int param = (int)i - 1;
    auto inlet = qobject_cast<Process::ControlInlet*>(m_inlets[i]);
    if(!inlet)
      continue;

    const auto& m = meta[param];
    if(matchesMetadata(*inlet, m))
      continue;

    // Only a control that still *is* a bare [0;1] slider holds a normalized value.
    // Anything else was written in display units by a build whose metadata simply
    // differed from ours; reinterpreting that as normalized would clamp e.g. 8 bits
    // or 9 dB to 1.0 and slam the parameter to its maximum.
    const bool legacy
        = m_loadedVersion < formatVersion && looksLikeLegacyControl(*inlet);
    const double stored = ossia::convert<double>(inlet->value());
    const double display = legacy ? m.toDisplay(std::clamp(stored, 0., 1.))
                                  : remapDisplayValue(*inlet, m, stored);

    auto ctrl = makeControl(m, inlet->id(), display);
    carryOverPortState(*inlet, *ctrl);

    m_inlets[i] = ctrl;
    delete inlet;
  }

  // ...and one that appeared with an airwin2rack update gains one.
  for(int param = std::max<int>(0, (int)m_inlets.size() - 1); param < (int)meta.size();
      param++)
    addControl(param, meta[param].defaultNormalized);
}

void ProcessModel::addControl(int idx, float normalized)
{
  const auto& meta = parameters();
  if(idx < 0 || idx >= (int)meta.size())
    return;

  const auto& m = meta[idx];
  m_inlets.push_back(makeControl(
      m, Id<Process::Port>(getStrongId(inlets()).val()), m.toDisplay(normalized)));
}
}
