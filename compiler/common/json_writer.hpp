#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace parc::json {

// Builds indented JSON for nested output like ast.json:
//   w.begin_object(); w.key("node"); w.string("Let"); w.end_object();
class Writer {
public:
    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    void end_array() { close(']'); }

    void key(std::string_view name);

    void string(std::string_view value);
    void integer(long long value);
    void number(double value);
    void boolean(bool value);
    void null();

    const std::string& str() const { return out_; }

private:
    std::string out_;
    std::vector<bool> empty_;  // one entry per open container: has it no items yet?
    bool after_key_ = false;

    void prefix();
    void raw(const std::string& text);
    void open(char bracket);
    void close(char bracket);
};

}  // namespace parc::json
