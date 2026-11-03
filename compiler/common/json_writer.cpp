#include "common/json_writer.hpp"

#include "common/json.hpp"

namespace parc::json {

void Writer::prefix() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (empty_.empty()) return;
    if (!empty_.back()) out_ += ',';
    empty_.back() = false;
    out_ += '\n';
    out_.append(2 * empty_.size(), ' ');
}

void Writer::raw(const std::string& text) {
    prefix();
    out_ += text;
}

void Writer::open(char bracket) {
    prefix();
    out_ += bracket;
    empty_.push_back(true);
}

void Writer::close(char bracket) {
    bool was_empty = empty_.back();
    empty_.pop_back();
    if (!was_empty) {
        out_ += '\n';
        out_.append(2 * empty_.size(), ' ');
    }
    out_ += bracket;
}

void Writer::key(std::string_view name) {
    prefix();
    out_ += quote(name) + ": ";
    after_key_ = true;
}

void Writer::string(std::string_view value) { raw(quote(value)); }
void Writer::integer(long long value) { raw(std::to_string(value)); }
void Writer::number(double value) { raw(json::number(value)); }
void Writer::boolean(bool value) { raw(value ? "true" : "false"); }
void Writer::null() { raw("null"); }

}  // namespace parc::json
