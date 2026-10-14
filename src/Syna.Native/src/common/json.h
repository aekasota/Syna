#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace syna {

// Minimal streaming JSON writer for the low-frequency reports crossing the C ABI.
class JsonWriter {
public:
    JsonWriter& begin_object() { prefix(); out_ += '{'; stack_.push_back(true); return *this; }
    JsonWriter& end_object() { out_ += '}'; stack_.pop_back(); return *this; }
    JsonWriter& begin_array() { prefix(); out_ += '['; stack_.push_back(true); return *this; }
    JsonWriter& end_array() { out_ += ']'; stack_.pop_back(); return *this; }

    JsonWriter& key(std::string_view k) {
        prefix();
        write_string(k);
        out_ += ':';
        pending_value_ = true;
        return *this;
    }

    JsonWriter& value(std::string_view v) { prefix(); write_string(v); return *this; }
    JsonWriter& value(const char* v) { return value(std::string_view(v ? v : "")); }
    JsonWriter& value(const std::string& v) { return value(std::string_view(v)); }
    JsonWriter& value(bool v) { prefix(); out_ += v ? "true" : "false"; return *this; }
    JsonWriter& value(int v) { return value(static_cast<int64_t>(v)); }
    JsonWriter& value(unsigned v) { return value(static_cast<int64_t>(v)); }
    JsonWriter& value(int64_t v) { prefix(); out_ += std::to_string(v); return *this; }
    JsonWriter& value(double v) {
        prefix();
        if (!std::isfinite(v)) { out_ += "null"; return *this; }
        char buf[32];
        snprintf(buf, sizeof(buf), "%.10g", v);
        out_ += buf;
        return *this;
    }
    JsonWriter& null() { prefix(); out_ += "null"; return *this; }

    template <class T>
    JsonWriter& field(std::string_view k, const T& v) { key(k); return value(v); }

    const std::string& str() const { return out_; }

private:
    void prefix() {
        if (pending_value_) { pending_value_ = false; return; }
        if (stack_.empty()) return;
        if (stack_.back()) stack_.back() = false;
        else out_ += ',';
    }

    void write_string(std::string_view s) {
        out_ += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += static_cast<char>(c);
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> stack_;
    bool pending_value_ = false;
};

}  // namespace syna
