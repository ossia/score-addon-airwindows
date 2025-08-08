// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>

#include <Process/Dataflow/ControlWidgets.hpp>
#include <Airwindows/Library.hpp>
#include <Airwindows/ProcessMetadata.hpp>

#include <score/tools/IdentifierGeneration.hpp>

#include <ossia/detail/algorithms.hpp>

#include <wobjectimpl.h>
W_OBJECT_IMPL(Airwindows::ProcessModel)

namespace Airwindows
{

ProcessModel::ProcessModel(
    const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
    QObject* parent)
    : Process::
          ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
    , audio_in{Process::make_audio_inlet(Id<Process::Port>(0), this)}
    , audio_out{Process::make_audio_outlet(Id<Process::Port>(0), this)}
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

std::unique_ptr<AirwinConsolidatedBase> ProcessModel::createEffect() const
{
  if(m_pluginName.isEmpty())
    return {};

  // Initialize registry if needed
  initializeRegistry();

  // Find the plugin in the registry
  auto it = AirwinRegistry::nameToIndex.find(m_pluginName.toStdString());
  if(it == AirwinRegistry::nameToIndex.end())
    return {};

  int index = it->second;
  if(index < 0 || index >= AirwinRegistry::registry.size())
    return {};

  // Create the effect using the generator function
  return AirwinRegistry::registry[index].generator();
}

void ProcessModel::init()
{
  // Basic initialization
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
  auto fx = createEffect();
  if(!fx)
    return QString("Param %1").arg(index);
    
  char name[256] = {0};
  fx->getParameterName(index, name);
  QString result = QString::fromUtf8(name);
  return result.isEmpty() ? QString("Param %1").arg(index) : result;
}

QString ProcessModel::getParameterLabel(int index) const
{
  auto fx = createEffect();
  if(!fx)
    return "";
    
  char label[256] = {0};
  fx->getParameterLabel(index, label);
  return QString::fromUtf8(label);
}

QString ProcessModel::getParameterDisplay(int index) const
{
  auto fx = createEffect();
  if(!fx)
    return "";
    
  char display[256] = {0};
  fx->getParameterDisplay(index, display);
  return QString::fromUtf8(display);
}

int ProcessModel::getParameterCount() const
{
  if(m_pluginName.isEmpty())
    return 0;
    
  // Initialize registry if needed
  initializeRegistry();
  
  // Find the plugin in the registry
  auto it = AirwinRegistry::nameToIndex.find(m_pluginName.toStdString());
  if(it == AirwinRegistry::nameToIndex.end())
    return 0;
    
  int index = it->second;
  if(index < 0 || index >= AirwinRegistry::registry.size())
    return 0;
    
  // Get parameter count from the registry
  return AirwinRegistry::registry[index].nParams;
}
}
