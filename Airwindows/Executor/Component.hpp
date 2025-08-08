#pragma once
#include <Process/Execution/ProcessComponent.hpp>
#include <Process/ExecutionContext.hpp>

#include <score/document/DocumentContext.hpp>
#include <score/document/DocumentInterface.hpp>

#include <ossia/dataflow/node_process.hpp>
#include <ossia/editor/scenario/time_process.hpp>
#include <ossia/editor/scenario/time_value.hpp>

#include <memory>

namespace Airwindows
{
class ProcessModel;
namespace Executor
{
class Component final
    : public ::Execution::ProcessComponent_T<
          Airwindows::ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("19073849-56b9-4541-bb2b-193b63833852")
public:
  Component(
      Airwindows::ProcessModel& element, const Execution::Context& ctx, QObject* parent);
  ~Component() override;
};

using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
