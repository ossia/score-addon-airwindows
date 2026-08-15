// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "Registry.hpp"

#include <AirwinRegistry.h>
#include <airwin_consolidated_base.h>

namespace Airwindows
{

void initializeRegistry()
{
  static const int once = [] {
    if(!(AirwinConsolidatedBase::defaultSampleRate > 2000.f))
      AirwinConsolidatedBase::defaultSampleRate = 48000.f;

    return AirwinRegistry::completeRegistry();
  }();
  (void)once;
}

}
