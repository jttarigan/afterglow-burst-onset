#pragma once
// Minimal JSON for level files and run.json. Copied from NeonTide/core/include/nt/json.h
// (commit 3dc7266).
// Integer-only like the rest of core/: numbers are kept as i64 when they are integers, and as
// their original text ("raw") otherwise, so core never parses or formats a float.
// Objects keep insertion order, so output is stable.
#include "ag/types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ag {

class Json {
public:
    enum class Type : u8 { Null, Bool, Int, Raw, String, Array, Object };

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool v);
    static Json num(i64 v);
    static Json raw(std::string numberText);   // pre-formatted number (lab code formats floats)
    static Json str(std::string v);
    static Json array();
    static Json object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isInt() const { return type_ == Type::Int; }

    bool asBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    i64 asInt(i64 fallback = 0) const { return type_ == Type::Int ? int_ : fallback; }
    const std::string& asString() const { return str_; }   // also the text of a Raw number

    /// Object: insert or replace a key. Returns *this for chaining.
    Json& set(const std::string& key, Json value);
    /// Object lookup; nullptr if missing or not an object.
    const Json* get(std::string_view key) const;
    Json* getMutable(std::string_view key);
    /// Array append.
    Json& push(Json value);

    const std::vector<Json>& items() const { return arr_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }

    /// Serialise. indent < 0 gives a single line.
    std::string dump(int indent = 2) const;

    static bool parse(std::string_view text, Json& out, std::string& err);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    i64 int_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

/// JSON string escaping (also used by the CSV/JSON writers in lab/).
std::string json_escape(std::string_view s);

} // namespace ag
