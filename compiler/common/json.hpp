#pragma once

#include <string>
#include <string_view>

// Just enough JSON writing for the phase output files; ParC never reads JSON back in C++.
namespace parc::json {

std::string quote(std::string_view text);
std::string number(double value);

}  // namespace parc::json
