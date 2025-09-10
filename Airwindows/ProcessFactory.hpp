#pragma once
#include <Process/GenericProcessFactory.hpp>

#include <Airwindows/ProcessMetadata.hpp>
#include <Airwindows/ProcessModel.hpp>

namespace Airwindows
{
struct ProcessFactory : Process::ProcessFactory_T<Airwindows::ProcessModel>
{
  Process::Descriptor descriptor(QString txt) const noexcept override;
};
}
