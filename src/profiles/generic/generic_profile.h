#pragma once

#include "profiles/profile.h"

namespace mqlc::profiles::generic {

[[nodiscard]] std::unique_ptr<Profile> make_profile();

}  // namespace mqlc::profiles::generic
