#pragma once
#include <Process/ProcessMetadata.hpp>

#include <QString>

namespace Airwindows
{
class ProcessModel;
}

PROCESS_METADATA(
    , Airwindows::ProcessModel, "415785fe-a7d8-4e62-a85f-9dfccf7dcd96", "Airwindows",
    "Airwindows", Process::ProcessCategory::AudioEffect, "Plugins",
    "Airwindows audio effects collection", "Chris Johnson / BaconPaul",
    (QStringList{"Effects", "Airwindows"}), {}, {}, QUrl("https://www.airwindows.com/"),
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::ExternalEffect)
