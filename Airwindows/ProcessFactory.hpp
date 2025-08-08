#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include <Process/Script/ScriptEditor.hpp>
#include <Process/WidgetLayer/WidgetProcessFactory.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>

#include <Airwindows/ProcessMetadata.hpp>
#include <Airwindows/ProcessModel.hpp>

namespace Airwindows
{
using ProcessFactory = Process::ProcessFactory_T<Airwindows::ProcessModel>;
}
