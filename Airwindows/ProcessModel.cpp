// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "ProcessModel.hpp"

#include <Process/Dataflow/ControlWidgets.hpp>
#include <Process/Dataflow/Port.hpp>

#include <score/tools/IdentifierGeneration.hpp>

#include <ossia/detail/algorithms.hpp>

#include <Airwindows/Library.hpp>
#include <Airwindows/ProcessFactory.hpp>
#include <Airwindows/ProcessMetadata.hpp>

#include <wobjectimpl.h>
W_OBJECT_IMPL(Airwindows::ProcessModel)

namespace Airwindows
{

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
    : Process::
          ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
    , audio_in{std::make_unique<Process::AudioInlet>(
          "Audio In", Id<Process::Port>(0), this)}
    , audio_out{std::make_unique<Process::AudioOutlet>(
          "Audio Out", Id<Process::Port>(0), this)}
    , m_pluginName{data}
{
  metadata().setInstanceName(*this);

  m_inlets.push_back(audio_in.get());
  m_outlets.push_back(audio_out.get());
  ((Process::AudioOutlet*)audio_out.get())->setPropagate(true);
  
  init();
  
  // Create controls for the plugin if it has parameters
  if(!m_pluginName.isEmpty())
  {
    int numParams = getParameterCount();
    for(int i = 0; i < numParams; i++)
    {
      on_addControl(i, 0.5f); // Default value
    }
  }
}

Process::ProcessFlags ProcessModel::flags() const noexcept
{
  auto f = Metadata<Process::ProcessFlags_k, ProcessModel>::get();
  if(m_pluginIndex < 0 || m_pluginIndex >= AirwinRegistry::registry.size())
    return f;

  auto& r = AirwinRegistry::registry[m_pluginIndex];
  if(r.isMono)
    f |= Process::ProcessFlags::PolyphonySupported;

  return f;
}
ProcessModel::~ProcessModel() { }

QString ProcessModel::prettyName() const noexcept
{
  return m_pluginName.isEmpty() ? "Airwindows"
                                : QString("Airwindows %1").arg(m_pluginName);
}

void ProcessModel::setPluginName(const QString& name)
{
  if(m_pluginName != name)
  {
    m_pluginName = name;
    metadata().setInstanceName(*this);
  }
}

void ProcessModel::init()
{
  if(auto it = AirwinRegistry::nameToIndex.find(m_pluginName.toStdString());
     it != AirwinRegistry::nameToIndex.end())
    m_pluginIndex = it->second;
  reg = &AirwinRegistry::registry[m_pluginIndex];
  fx.reset(reg->generator().release());
}

void ProcessModel::on_addControl(int idx, float v)
{
  if(controls.find(idx) != controls.end())
    return;

  auto ctrl = new Process::FloatSlider{
    getParameterName(idx),
    Id<Process::Port>(getStrongId(inlets()).val()), 
    this
  };

  // FIXME: use getParameterDisplay.
  // Maybe we could display something like "0.2 (-18dB)"
  ctrl->setDomain(ossia::make_domain(0.f, 1.f));
  ctrl->setValue(v);
  
  controls[idx] = ctrl;
  m_inlets.push_back(ctrl);
  controlAdded(*ctrl);
}

void ProcessModel::removeControl(const Id<Process::Port>& id)
{
  // Find and remove the control from our mapping
  auto it = ossia::find_if(controls, [&](const auto& p) { 
    return p.second->id() == id; 
  });
  if(it != controls.end())
  {
    controls.erase(it);
  }
  
  auto inlet_it = ossia::find_if(m_inlets, [&](Process::Inlet* inl) { return inl->id() == id; });
  if(inlet_it != m_inlets.end())
  {
    controlRemoved(**inlet_it);
    m_inlets.erase(inlet_it);
  }
}

void ProcessModel::removeControl(int fxnum)
{
  auto it = controls.find(fxnum);
  if(it != controls.end())
  {
    removeControl(it->second->id());
  }
}

QString ProcessModel::getParameterName(int index) const
{
  if(!fx)
    return QString("Param %1").arg(index);
    
  char name[256] = {0};
  fx->getParameterName(index, name);
  QString result = QString::fromUtf8(name);
  return result.isEmpty() ? QString("Param %1").arg(index) : result;
}

int ProcessModel::getParameterCount() const
{
  if(m_pluginIndex < 0 || m_pluginIndex >= AirwinRegistry::registry.size())
    return 0;

  return AirwinRegistry::registry[m_pluginIndex].nParams;
}
}
