// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Component.hpp"

#include <Explorer/DocumentPlugin/DeviceDocumentPlugin.hpp>

#include <Scenario/Execution/score2OSSIA.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/serialization/AnySerialization.hpp>
#include <score/serialization/MapSerialization.hpp>
#include <score/tools/Bind.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/graph/graph_interface.hpp>
#include <ossia/dataflow/graph_edge.hpp>
#include <ossia/dataflow/graph_edge_helpers.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/logger.hpp>
#include <ossia/detail/ssize.hpp>
#include <ossia/editor/scenario/time_interval.hpp>
#include <ossia/editor/state/message.hpp>
#include <ossia/editor/state/state.hpp>

#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlContext>
#include <QTimer>

#include <Airwindows/ProcessModel.hpp>

#include <AirwinRegistry.h>
#include <airwin_consolidated_base.h>

namespace Airwindows
{
namespace Executor
{
class airwindows_node final : public ossia::graph_node
{
public:
  explicit airwindows_node(std::unique_ptr<AirwinConsolidatedBase> fx)
      : m_fx{std::move(fx)}
  {
    m_inlets.push_back(new ossia::audio_inlet);
    m_outlets.push_back(new ossia::audio_outlet);
  }

  auto add_control_inlet()
  {
    auto inlet = new ossia::value_inlet;
    (*inlet)->domain = ossia::domain_base<float>{0.f, 1.f};
    (*inlet)->type = ossia::val_type::FLOAT;
    m_inlets.push_back(inlet);
    return inlet;
  }

  void run(const ossia::token_request& t, ossia::exec_state_facade e) noexcept override
  {
    if(!m_fx)
      return;

    auto& inp = *m_inlets[0]->target<ossia::audio_port>();
    auto& outp = *m_outlets[0]->target<ossia::audio_port>();

    // Handle parameter changes
    for(std::size_t i = 1; i < m_inlets.size(); i++)
    {
      if(auto port = m_inlets[i]->target<ossia::value_port>())
      {
        if(!port->get_data().empty())
        {
          float val = ossia::convert<float>(port->get_data().back().value);
          m_fx->setParameter(i - 1, val); // -1 because first inlet is audio
        }
      }
    }

    const auto [tick_start, d] = e.timings(t);

    if(d <= 0)
      return;

    // Get input channels
    const auto channels = inp.channels();
    if(channels == 0)
      return;

    // Prepare output
    outp.set_channels(channels);
    for(auto& out : outp)
      out.resize(e.bufferSize());

    if(inp.channel(0).size() < (tick_start + d))
      return;

    // Most airwindows plugins are stereo, but some are mono
    // We need to handle both cases
    // FIXME: handle polyphonic mode
    if(channels == 1 && inp.channels() >= 1)
    {
      // Mono processing
      double* input = inp.channel(0).data() + tick_start;
      double* output = outp.channel(0).data() + tick_start;

      // Copy input to output first
      std::copy_n(input, d, output);

      // Process in place
      double* io[1] = {output};
      m_fx->processDoubleReplacing(io, io, d);
    }
    else if(channels >= 2 && inp.channels() >= 2)
    {
      if(inp.channel(1).size() < (tick_start + d))
        return;
      // Stereo processing (use first 2 channels)
      double* inputs[2]
          = {inp.channel(0).data() + tick_start, inp.channel(1).data() + tick_start};
      double* outputs[2]
          = {outp.channel(0).data() + tick_start, outp.channel(1).data() + tick_start};

      m_fx->processDoubleReplacing(inputs, outputs, d);

      // Copy extra channels as-is
      for(std::size_t i = 2; i < channels; ++i)
      {
        double* input = inp.channel(i).data() + tick_start;
        double* output = outp.channel(i).data() + tick_start;
        std::copy_n(input, d, output);
      }
    }
  }

  [[nodiscard]] std::string label() const noexcept override { return "airwindows"; }

  std::unique_ptr<AirwinConsolidatedBase> m_fx;
};

Component::Component(
    Airwindows::ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
    : ::Execution::ProcessComponent_T<Airwindows::ProcessModel, ossia::node_process>{
          proc, ctx, "AirwindowsComponent", parent}
{
  // Create the airwindows effect
  auto fx_ptr = proc.createEffect();
  if(!fx_ptr)
    return;

  fx_ptr->setSampleRate(ctx.execState->sampleRate);
  // Create the node with the effect
  auto node = ossia::make_node<airwindows_node>(*ctx.execState, std::move(fx_ptr));
  auto& fx = *node->m_fx;

  // Add control inlets for each parameter
  const auto& inls = proc.inlets();
  int idx = 0;
  auto it = inls.begin();
  ++it;
  ++idx;
  auto weak_node = std::weak_ptr{node};
  for(; it != inls.end(); ++it)
  {
    auto model_inlet = qobject_cast<const Process::ControlInlet*>(*it);
    SCORE_ASSERT(model_inlet);
    auto exec_inlet = node->add_control_inlet();
    fx.setParameter(idx - 1, ossia::convert<float>(model_inlet->value()));

    connect(
        model_inlet, &Process::ControlInlet::valueChanged, this,
        [idx, weak_node, exec_inlet](const ossia::value& v) {
      if(auto n = weak_node.lock())
      {
        SCORE_ASSERT(n->root_inputs().size() > idx);
        exec_inlet->data.write_value(v, 0);
      }
    });

    idx++;
  }

  this->node = node;

  // Connect the ports
  m_ossia_process = std::make_shared<ossia::node_process>(node);
}

Component::~Component() { }

}
}
