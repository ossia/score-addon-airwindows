#pragma once
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Process/Process.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>

#include <score/widgets/PluginWindow.hpp>

#include <Airwindows/ProcessMetadata.hpp>

#include <AirwinRegistry.h>
#include <airwin_consolidated_base.h>

#include <memory>
#include <verdigris>

namespace Airwindows
{
class ProcessModel;
class ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Airwindows::ProcessModel)
  W_OBJECT(ProcessModel)

  friend class DataStreamReader;
  friend class DataStreamWriter;
  friend class JSONReader;
  friend class JSONWriter;

public:
  explicit ProcessModel(
      const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
      QObject* parent);

  template <typename Impl>
  explicit ProcessModel(Impl& vis, QObject* parent)
      : Process::ProcessModel{vis, parent}
  {
    vis.writeTo(*this);
  }

  ~ProcessModel() override;

  QString prettyName() const noexcept override;

  void setPluginName(const QString& name);
  const QString& pluginName() const noexcept { return m_pluginName; }

  // Create the airwindows effect instance
  std::unique_ptr<AirwinConsolidatedBase> createEffect() const;

  // Control management
  void on_addControl(int idx, float v);
  void removeControl(const Id<Process::Port>&);
  void removeControl(int fxnum);
  
  // Get parameter info from the effect
  QString getParameterName(int index) const;
  QString getParameterLabel(int index) const;
  QString getParameterDisplay(int index) const;
  int getParameterCount() const;

  std::unique_ptr<Process::Inlet> audio_in;
  std::unique_ptr<Process::Outlet> audio_out;

  ossia::hash_map<int, Process::FloatSlider*> controls;

  // Signals
  void controlAdded(const Process::Port& p) W_SIGNAL(controlAdded, p);
  void controlRemoved(const Process::Port& p) W_SIGNAL(controlRemoved, p);

private:
  QString m_pluginName;
  void init();
};

}
