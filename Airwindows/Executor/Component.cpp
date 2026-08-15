// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Component.hpp"

#include <Process/Dataflow/Port.hpp>

#include <score/tools/SafeCast.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/detail/fmt.hpp>
#include <ossia/dataflow/graph/graph_interface.hpp>
#include <ossia/dataflow/graph_edge.hpp>
#include <ossia/dataflow/graph_edge_helpers.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/parse_relax.hpp>
#include <ossia/detail/ssize.hpp>
#include <ossia/math/safe_math.hpp>

#include <Airwindows/Metadata.hpp>
#include <Airwindows/ProcessModel.hpp>

#include <AirwinRegistry.h>
#include <airwin_consolidated_base.h>

namespace Airwindows
{
namespace Executor
{
namespace
{
//! The element type of a polyphonic value differs per ossia::value alternative.
struct to_double
{
  double operator()(float v) const noexcept { return v; }
  double operator()(const ossia::value& v) const noexcept
  {
    return ossia::convert<double>(v);
  }
};
}

/**
 * @brief What both node shapes share: the effect class and the parameter mapping.
 *
 * Control ports carry the value in display units - dB, Hz, an enumerator index - and
 * the node normalizes it back to the [0;1] the effect actually consumes.
 */
class airwindows_node_base : public ossia::graph_node
{
public:
  const AirwinRegistry::awReg& plugin_class;
  const std::vector<ParameterMetadata>& params;

  explicit airwindows_node_base(
      const AirwinRegistry::awReg& c, const std::vector<ParameterMetadata>& p)
      : plugin_class{c}
      , params{p}
  {
    m_inlets.push_back(new ossia::audio_inlet);
    m_outlets.push_back(new ossia::audio_outlet);
  }

  ossia::value_inlet* add_control_inlet()
  {
    auto inlet = new ossia::value_inlet;
    m_inlets.push_back(inlet);
    return inlet;
  }

  //! Display units -> the [0;1] the effect consumes. Total: never returns a NaN.
  float to_normalized(int parameter_i, double display) const noexcept
  {
    if(parameter_i >= 0 && parameter_i < std::ssize(params))
      return (float)params[parameter_i].toNormalized(display);
    const bool finite = !ossia::safe_isnan(display) && !ossia::safe_isinf(display);
    return finite ? (float)std::clamp(display, 0., 1.) : 0.f;
  }
};

// In this one we create as many instances as we have channels.
class airwindows_node_polyphonic final : public airwindows_node_base
{
  struct poly_plugin
  {
    std::shared_ptr<AirwinConsolidatedBase> plugin;
    operator const AirwinConsolidatedBase*() const noexcept { return plugin.get(); }
    const AirwinConsolidatedBase* operator->() const noexcept { return plugin.get(); }
    AirwinConsolidatedBase* operator->() noexcept { return plugin.get(); }
  };
  double sample_rate{};

public:
  explicit airwindows_node_polyphonic(
      const AirwinRegistry::awReg& c, const std::vector<ParameterMetadata>& p, double sr)
      : airwindows_node_base{c, p}
      , sample_rate{sr}
  {
    m_fxs.reserve(8);
    for(int i = 0; i < 8; i++)
    {
      m_fxs.push_back({.plugin = c.generator()});
      m_fxs.back().plugin->setSampleRate(sample_rate);
    }
  }

  ~airwindows_node_polyphonic() { }

  //! One value per voice.
  template <typename T, typename Conv>
  void set_per_voice(int parameter_i, const T& v, Conv convert)
  {
    for(int plug_i = 0; plug_i < m_fxs.size() && plug_i < v.size(); plug_i++)
    {
      auto& fx = m_fxs[plug_i];
      if(fx.plugin)
        fx->setParameter(parameter_i, to_normalized(parameter_i, convert(v[plug_i])));
    }
  }

  void apply_value(int i, const ossia::value& v)
  {
    struct
    {
      airwindows_node_polyphonic& self;
      int parameter_i;

      //! All channels get the same value.
      void set_all(double display)
      {
        const float norm = self.to_normalized(parameter_i, display);
        for(auto& fx : self.m_fxs)
          if(fx.plugin)
            fx->setParameter(parameter_i, norm);
      }

      void operator()() { }
      void operator()(ossia::impulse) { }
      void operator()(int v) { set_all(v); }
      void operator()(float v) { set_all(v); }
      void operator()(bool v) { set_all(v ? 1. : 0.); }
      void operator()(const std::string& str)
      {
        set_all(ossia::parse_relax<float>(str).value_or(0.f));
      }
      void operator()(ossia::vec2f v)
      {
        self.set_per_voice(parameter_i, v, to_double{});
      }
      void operator()(ossia::vec3f v)
      {
        self.set_per_voice(parameter_i, v, to_double{});
      }
      void operator()(ossia::vec4f v)
      {
        self.set_per_voice(parameter_i, v, to_double{});
      }
      void operator()(const std::vector<ossia::value>& v)
      {
        self.set_per_voice(parameter_i, v, to_double{});
      }
      void operator()(const ossia::value_map_type& v)
      {
        for(int plug_i = 0; plug_i < self.m_fxs.size() && plug_i < v.size(); plug_i++)
        {
          auto& fx = self.m_fxs[plug_i];
          if(fx.plugin)
            fx->setParameter(
                parameter_i,
                self.to_normalized(
                    parameter_i, ossia::convert<double>(v.at(plug_i).second)));
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
      auto& chan = audio_in.channel(i);
      if(chan.size() < e.bufferSize())
        chan.resize(e.bufferSize());
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

class airwindows_node_stereo final : public airwindows_node_base
{
public:
  explicit airwindows_node_stereo(
      const AirwinRegistry::awReg& c, const std::vector<ParameterMetadata>& p,
      std::shared_ptr<AirwinConsolidatedBase> fx)
      : airwindows_node_base{c, p}
      , m_fx{std::move(fx)}
  {
  }

  ~airwindows_node_stereo() { }

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
          // -1 because the first inlet is audio
          const double val = ossia::convert<double>(port->get_data().back().value);
          m_fx->setParameter((int)i - 1, to_normalized((int)i - 1, val));
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

    for(auto& chan : inp)
      chan.resize(std::max((int)chan.size(), e.bufferSize()));
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

  const auto sr = ctx.execState->sampleRate;
  const bool monophonic = proc.flags() & Process::ProcessFlags::PolyphonySupported;
  const auto& params = proc.parameters();

  // We always build fresh effects: airwindows plug-ins cannot reset their internal
  // state, so reusing one would let e.g. delay and reverb trails survive a
  // stop / play sequence.
  std::shared_ptr<airwindows_node_base> node;
  if(monophonic)
  {
    node = ossia::make_node<airwindows_node_polyphonic>(
        *ctx.execState, *proc.reg, params, sr);
  }
  else
  {
    auto fx_ptr = proc.reg->generator();
    if(!fx_ptr)
      return;
    fx_ptr->setSampleRate(sr);

    node = ossia::make_node<airwindows_node_stereo>(
        *ctx.execState, *proc.reg, params, std::move(fx_ptr));
  }

  // Add a control inlet per parameter. The port carries display units; the node
  // normalizes them on the audio thread.
  const auto& inls = proc.inlets();
  auto weak_node = std::weak_ptr{node};
  for(auto it = std::next(inls.begin()); it != inls.end(); ++it)
  {
    auto model_inlet = qobject_cast<Process::ControlInlet*>(*it);
    SCORE_ASSERT(model_inlet);

    auto exec_inlet = node->add_control_inlet();
    model_inlet->setupExecution(*exec_inlet, this);
    exec_inlet->data.write_value(model_inlet->value(), 0);

    connect(
        model_inlet, &Process::ControlInlet::valueChanged, this,
        [weak_node, exec_inlet](const ossia::value& v) {
      if(auto n = weak_node.lock())
        exec_inlet->data.write_value(v, 0);
    });
  }

  this->node = node;
  m_ossia_process = std::make_shared<ossia::node_process>(node);
}

Component::~Component() { }

}
}
