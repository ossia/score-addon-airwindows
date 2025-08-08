// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "score_addon_airwindows.hpp"

#include <Process/ProcessFactory.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/plugins/FactorySetup.hpp>
#include <score/plugins/InterfaceList.hpp>
#include <score/plugins/StringFactoryKey.hpp>
#include <score/tools/std/HashMap.hpp>

#include <Airwindows/Executor/Component.hpp>
#include <Airwindows/Library.hpp>
#include <Airwindows/ProcessFactory.hpp>

#include <AirwinRegistry.h>
#include <wobjectimpl.h>

score_addon_airwindows::score_addon_airwindows()
{
  AirwinRegistry::completeRegistry();
}

score_addon_airwindows::~score_addon_airwindows() = default;

std::vector<score::InterfaceBase*> score_addon_airwindows::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Process::ProcessModelFactory, Airwindows::ProcessFactory>,
      FW<Library::LibraryInterface, Airwindows::LibraryHandler>,
      FW<Execution::ProcessComponentFactory, Airwindows::Executor::ComponentFactory>>(
      ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_airwindows)
