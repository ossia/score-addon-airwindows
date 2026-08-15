#pragma once
#include <Process/Process.hpp>

#include <Airwindows/Metadata.hpp>
#include <Airwindows/ProcessMetadata.hpp>

#include <AirwinRegistry.h>

#include <verdigris>

namespace Process
{
struct ControlInlet;
}

namespace Airwindows
{
class ProcessModel;
class ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  MODEL_METADATA_IMPL(ProcessModel)

  W_OBJECT(ProcessModel)

  friend class DataStreamReader;
  friend class DataStreamWriter;
  friend class JSONReader;
  friend class JSONWriter;

public:
  /**
   * @brief Serialization format of this process.
   *
   * 1: one FloatSlider per parameter, domain [0;1], value = the raw normalized
   *    parameter the effect consumes. Written by builds before this existed, and
   *    therefore *not* recorded in the file - an absent "Version" means 1.
   * 2: controls carry the parameter's real range - see Airwindows::ParameterMetadata.
   *    Values are in display units and the executor normalizes them.
   */
  static constexpr int formatVersion = 2;

  explicit ProcessModel(
      const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
      QObject* parent);

  template <typename Impl>
  explicit ProcessModel(Impl& vis, QObject* parent)
      : Process::ProcessModel{vis, parent}
  {
    vis.writeTo(*this);
    init();
    migrateControls();
  }

  ~ProcessModel() override;

  QString prettyName() const noexcept override;
  QString prettyShortName() const noexcept override
  {
    return Metadata<PrettyName_k, ProcessModel>::get();
  }
  QString category() const noexcept override
  {
    return Metadata<Category_k, ProcessModel>::get();
  }
  QStringList tags() const noexcept override
  {
    return Metadata<Tags_k, ProcessModel>::get();
  }
  Process::ProcessFlags flags() const noexcept override;

  const QString& pluginName() const noexcept { return m_pluginName; }

  /**
   * @brief Derived per-parameter metadata; empty if the plug-in could not be found.
   *
   * Probes the effect on the first call for a given plug-in - not cheap, and not
   * callable from the audio thread. The executor takes this reference once at setup.
   */
  const std::vector<ParameterMetadata>& parameters() const;

  AirwinRegistry::awReg* reg{};

private:
  QString m_pluginName;
  int m_pluginIndex{-1};

  //! Format the loaded document was written by; see @c formatVersion.
  int m_loadedVersion{formatVersion};

  void init();

  //! Build one control per parameter, at the effect's own default value.
  void createControls();

  //! Append a control for @p idx, whose value is given normalized.
  void addControl(int idx, float normalized);

  /**
   * @brief Bring deserialized controls in line with the current metadata.
   *
   * Idempotent: a control that already exposes what the metadata describes is left
   * untouched. Anything else is rebuilt - and only a control that still looks like
   * format 1 has its value reinterpreted as normalized. A format-2 value is in
   * display units and stays that way, however far the metadata has since moved.
   */
  void migrateControls();

  Process::ControlInlet*
  makeControl(const ParameterMetadata& meta, Id<Process::Port> id, double displayValue);
};

}
