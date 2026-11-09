#pragma once

#include "profiles/profile.h"

namespace mqlc::profiles::fincore {

[[nodiscard]] std::unique_ptr<Profile> make_profile();

}  // namespace mqlc::profiles::fincore
