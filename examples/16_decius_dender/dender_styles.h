#pragma once

#include <string>
#include <string_view>

namespace dender {

/// The Decius CSS version this app ships and was tested against (pins the
/// bundle the app loads and the version stamped on the view).
inline constexpr std::string_view kDeciusVersion{"0.6.2"};

/// App-specific CSS layered over the Decius bundle (the web sample's app.css).
[[nodiscard]] std::string native_css();

}  // namespace dender
