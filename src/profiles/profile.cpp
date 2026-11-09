#include "profiles/profile.h"

#include "profiles/fincore/fincore_profile.h"
#include "profiles/generic/generic_profile.h"

namespace mqlc::profiles {

std::unique_ptr<Profile> make_profile(core::ProfileKind kind) {
    return kind == core::ProfileKind::generic ? generic::make_profile() : fincore::make_profile();
}

}  // namespace mqlc::profiles
