#pragma once
#include "HalDisplay.h"
#include "HalGPIO.h"

// A synchronous transfer does not return to simulator_main's frame loop.
// Run its host-only frame boundary before the firmware polls transfer input.
inline void simBeginTransferFrame() {
  display.presentIfNeeded();
  gpio.beginFrame();
}
