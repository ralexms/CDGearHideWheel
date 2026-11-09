// The gear wheel: the highlighted piece, the toggle presses, the prompt in the centre tooltip.
#pragma once
#include "common.h"

namespace ghw {

bool install_wheel(); // vtable hooks on the gear wheel (open, close, focus, frame) and the HUD's frame

} // namespace ghw
