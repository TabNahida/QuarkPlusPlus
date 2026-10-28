#pragma once

#include <pace/tls.hpp>

namespace quarkpp
{
// Shared immutable trust configuration. PACE verifies both chains and hostnames;
// Windows and macOS roots come from the native certificate store.
[[nodiscard]] const pace::tls::context &default_api_trust();
} // namespace quarkpp
