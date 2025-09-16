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
#include <ossia/detail/parse_relax.hpp>
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
// In this one we create as many instances as we have channels.
class airwindows_node_polyphonic final : public ossia::graph_node
{
  struct poly_plugin
  {
    std::shared_ptr<AirwinConsolidatedBase> plugin;
    operator const AirwinConsolidatedBase*() const noexcept { return plugin.get(); }
    const AirwinConsolidatedBase* operator->() const noexcept { return plugin.get(); }
    AirwinConsolidatedBase* operator->() noexcept { return plugin.get(); }
  };
  const AirwinRegistry::awReg& plugin_class;
  double sample_rate{};

public:
  explicit airwindows_node_polyphonic(const AirwinRegistry::awReg& c, double sr)
      : plugin_class{c}
      , sample_rate{sr}
  {
    m_inlets.push_back(new ossia::audio_inlet);
    m_outlets.push_back(new ossia::audio_outlet);

    m_fxs.reserve(8);
    for(int i = 0; i < 8; i++)
    {
      m_fxs.push_back({.plugin = c.generator()});
      m_fxs.back().plugin->setSampleRate(sample_rate);
    }
  }

  auto add_control_inlet()
  {
    auto inlet = new ossia::value_inlet;
    (*inlet)->domain = ossia::domain_base<float>{0.f, 1.f};
    (*inlet)->type = ossia::val_type::FLOAT;
    m_inlets.push_back(inlet);
    return inlet;
  }

  void apply_value(int i, const ossia::value& v)
  {
    struct
    {
      airwindows_node_polyphonic& self;
      int parameter_i;
      void operator()() { }
      void operator()(ossia::impulse) { }
      void operator()(int v)
      {
        for(auto& fx : self.m_fxs)
          if(fx.plugin)
            fx->setParameter(parameter_i, v);
      }
      void operator()(float v)
      {
        for(auto& fx : self.m_fxs)
          if(fx.plugin)
            fx->setParameter(parameter_i, v);
      }
      void operator()(bool v)
      {
        for(auto& fx : self.m_fxs)
          if(fx.plugin)
            fx->setParameter(parameter_i, v ? 1 : 0);
      }
      void operator()(const std::string& str)
      {
        float val;
        if(auto v = ossia::parse_relax<float>(str))
          val = *v;
        else
          val = 0.f;

        for(auto& fx : self.m_fxs)
          if(fx.plugin)
            fx->setParameter(parameter_i, val);
      }
      void operator()(ossia::vec2f v)
      {
        for(int plug_i = 0; plug_i < self.m_fxs.size() && plug_i < v.size(); plug_i++)
        {
          auto& fx = self.m_fxs[plug_i];
          if(fx.plugin)
            fx->setParameter(parameter_i, v[plug_i]);
        }
      }
      void operator()(ossia::vec3f v)
      {
        for(int plug_i = 0; plug_i < self.m_fxs.size() && plug_i < v.size(); plug_i++)
        {
          auto& fx = self.m_fxs[plug_i];
          if(fx.plugin)
            fx->setParameter(parameter_i, v[plug_i]);
        }
      }
      void operator()(ossia::vec4f v)
      {
        for(int plug_i = 0; plug_i < self.m_fxs.size() && plug_i < v.size(); plug_i++)
        {
          auto& fx = self.m_fxs[plug_i];
          if(fx.plugin)
            fx->setParameter(parameter_i, v[plug_i]);
        }
      }
      void operator()(const std::vector<ossia::value>& v)
      {
        for(int plug_i = 0; plug_i < self.m_fxs.size() && plug_i < v.size(); plug_i++)
        {
          auto& fx = self.m_fxs[plug_i];
          if(fx.plugin)
            fx->setParameter(parameter_i, ossia::convert<float>(v[plug_i]));
        }
      }
      void operator()(const ossia::value_map_type& v)
      {
        for(int plug_i = 0; plug_i < self.m_fxs.size() && plug_i < v.size(); plug_i++)
        {
          auto& fx = self.m_fxs[plug_i];
          if(fx.plugin)
            fx->setParameter(parameter_i, ossia::convert<float>(v.at(plug_i).second));
        }
      }
    } vis{*this, i};
    v.apply(vis);
  }

  void run(const ossia::token_request& t, ossia::exec_state_facade e) noexcept override
  {
    auto& audio_in = *m_inlets[0]->target<ossia::audio_port>();
    auto& audio_out = *m_outlets[0]->target<ossia::audio_port>();

    // Resize
    const auto poly_channels = audio_in.channels();
    audio_out.set_channels(poly_channels);
    for(auto& out : audio_out)
      out.resize(e.bufferSize());

    // FIXME should be in main thread
    while(m_fxs.size() < poly_channels)
    {
      m_fxs.push_back({.plugin = this->plugin_class.generator()});
      m_fxs.back().plugin->setSampleRate(sample_rate);
    }

    // Handle parameter changes
    for(std::size_t i = 1; i < m_inlets.size(); i++)
    {
      if(auto port = m_inlets[i]->target<ossia::value_port>())
      {
        if(!port->get_data().empty())
        {
          auto& val = port->get_data().back().value;
          apply_value(i - 1, val);
        }
      }
    }

    if(poly_channels == 0)
      return;

    const auto [tick_start, d] = e.timings(t);

    if(d <= 0)
      return;

    if(audio_in.channel(0).size() < (tick_start + d))
      return;

    double* unused_zero_buffer = (double*)alloca(sizeof(double) * e.bufferSize() + 64);
    for(int i = 0; i < poly_channels; i++)
    {
      double* input = audio_in.channel(i).data() + tick_start;
      double* output = audio_out.channel(i).data() + tick_start;

      // Copy input to output first
      std::copy_n(input, d, output);
      std::fill_n(unused_zero_buffer, e.bufferSize(), 0.);

      // Process in place
      double* io[2] = {output, unused_zero_buffer};
      m_fxs[i]->processDoubleReplacing(io, io, d);
    }
  }

  [[nodiscard]] std::string label() const noexcept override
  {
    return fmt::format("airwindows poly::{}", plugin_class.name);
  }

  std::vector<poly_plugin> m_fxs;
};
class airwindows_node_stereo final : public ossia::graph_node
{
public:
  const AirwinRegistry::awReg& plugin_class;
  explicit airwindows_node_stereo(
      const AirwinRegistry::awReg& c, std::shared_ptr<AirwinConsolidatedBase> fx)
      : plugin_class{c}
      , m_fx{std::move(fx)}
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

    if(inp.channel(0).size() < e.bufferSize())
      return;

    if(channels == 1)
    {
      // Mono processing
      double* input = inp.channel(0).data() + tick_start;
      double* output = outp.channel(0).data() + tick_start;

      double* unused_zero_buffer = (double*)alloca(sizeof(double) * e.bufferSize() + 64);
      // Copy input to output first
      std::copy_n(input, d, output);
      std::fill_n(unused_zero_buffer, e.bufferSize(), 0.);

      // Process in place
      double* io[2] = {output, unused_zero_buffer};
      m_fx->processDoubleReplacing(io, io, d);
    }
    else if(channels >= 2)
    {
      if(inp.channel(1).size() < e.bufferSize())
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

  [[nodiscard]] std::string label() const noexcept override
  {
    return fmt::format("airwindows::{}", plugin_class.name);
  }

  std::shared_ptr<AirwinConsolidatedBase> m_fx;
};

Component::Component(
    Airwindows::ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
    : ::Execution::ProcessComponent_T<Airwindows::ProcessModel, ossia::node_process>{
          proc, ctx, "AirwindowsComponent", parent}
{
  if(!proc.reg)
    return;

  // Create the airwindows effect
  auto fx_ptr = proc.fx;
  if(!fx_ptr)
    return;

  const auto sr = ctx.execState->sampleRate;
  fx_ptr->setSampleRate(sr);

  const bool monophonic = proc.flags() & Process::ProcessFlags::PolyphonySupported;

  if(monophonic)
  {
    // Create the node with the effect
    auto node
        = ossia::make_node<airwindows_node_polyphonic>(*ctx.execState, *proc.reg, sr);

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
      exec_inlet->data.write_value(ossia::convert<float>(model_inlet->value()), 0);

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
  else
  {
    // Create the node with the effect
    auto node = ossia::make_node<airwindows_node_stereo>(
        *ctx.execState, *proc.reg, std::move(fx_ptr));
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
}

Component::~Component() { }

}
}
