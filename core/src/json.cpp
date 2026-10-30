#include "ag/json.h"

#include <charconv>

namespace ag {

Json Json::boolean(bool v) {
    Json j;
    j.type_ = Type::Bool;
    j.bool_ = v;
    return j;
}

Json Json::num(i64 v) {
    Json j;
    j.type_ = Type::Int;
    j.int_ = v;
    return j;
}

Json Json::raw(std::string numberText) {
    Json j;
    j.type_ = Type::Raw;
    j.str_ = std::move(numberText);
    return j;
}

Json Json::str(std::string v) {
    Json j;
    j.type_ = Type::String;
    j.str_ = std::move(v);
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) {
        *this = object();
    }
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return *this;
        }
    }
    obj_.emplace_back(key, std::move(value));
    return *this;
}

const Json* Json::get(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Json* Json::getMutable(std::string_view key) {
    if (type_ != Type::Object) return nullptr;
    for (auto& kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) {
        *this = array();
    }
    arr_.push_back(std::move(value));
    return *this;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uc < 0x20u) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[uc >> 4u]);
                    out.push_back(kHex[uc & 15u]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<usize>(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Int: {
            char buf[32];
            auto r = std::to_chars(buf, buf + sizeof(buf), int_);
            out.append(buf, r.ptr);
            break;
        }
        case Type::Raw: out += str_; break;
        case Type::String:
            out.push_back('"');
            out += json_escape(str_);
            out.push_back('"');
            break;
        case Type::Array:
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (usize i = 0; i < arr_.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        case Type::Object:
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (usize i = 0; i < obj_.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                out.push_back('"');
                out += json_escape(obj_[i].first);
                out += indent < 0 ? "\":" : "\": ";
                obj_[i].second.dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

namespace {

struct Parser {
    std::string_view s;
    usize i = 0;
    std::string err;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool fail(const char* msg) {
        if (err.empty()) err = std::string(msg) + " at offset " + std::to_string(i);
        return false;
    }
    bool lit(std::string_view w) {
        if (s.substr(i, w.size()) != w) return false;
        i += w.size();
        return true;
    }
    bool hex4(u32& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4u;
            if (c >= '0' && c <= '9') v |= static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<u32>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    u32 cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp < 0x80u) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800u) {
                        out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
                        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                    } else {
                        out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
                        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
                        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                    }
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            ++i;
            out = Json::object();
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.set(key, std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            out = Json::array();
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string v;
            if (!string(v)) return false;
            out = Json::str(std::move(v));
            return true;
        }
        if (lit("true")) {
            out = Json::boolean(true);
            return true;
        }
        if (lit("false")) {
            out = Json::boolean(false);
            return true;
        }
        if (lit("null")) {
            out = Json::null();
            return true;
        }
        // Number: integers become Int, anything with a fraction or exponent stays Raw text.
        const usize start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool integral = true;
        while (i < s.size()) {
            const char d = s[i];
            if (d >= '0' && d <= '9') {
                ++i;
            } else if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
                integral = false;
                ++i;
            } else {
                break;
            }
        }
        if (i == start) return fail("unexpected character");
        const std::string_view text = s.substr(start, i - start);
        if (integral) {
            i64 v = 0;
            auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec == std::errc() && r.ptr == text.data() + text.size()) {
                out = Json::num(v);
                return true;
            }
        }
        out = Json::raw(std::string(text));
        return true;
    }
};

} // namespace

bool Json::parse(std::string_view text, Json& out, std::string& err) {
    Parser p{text, 0, {}};
    if (!p.value(out, 0)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        err = "trailing characters at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

} // namespace ag
