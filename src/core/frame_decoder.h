// The original name of the delimiter framing, kept for code that splits 0x1A-framed streams
// without going through the Framer interface. See core/framer.h.
#pragma once

#include "core/framer.h"

namespace mqlc::core {

using FrameDecoder = DelimiterFramer;

}  // namespace mqlc::core
