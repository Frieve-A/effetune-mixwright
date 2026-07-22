#pragma once

#include <string>
#include <string_view>

namespace effetune::vst::plugin {

[[nodiscard]] bool openExternalUrl(std::string_view url, std::string *error = nullptr);

} // namespace effetune::vst::plugin
