#include "common/json.hpp"

#include <charconv>
#include <cstdio>

namespace parc::json {

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string number(double value) {
    char buf[64];
    auto result = std::to_chars(buf, buf + sizeof buf, value);
    std::string out(buf, result.ptr);
    // Keep floats recognizable as floats: 700 -> 700.0
    if (out.find_first_of(".eEni") == std::string::npos) out += ".0";
    return out;
}

}  // namespace parc::json
