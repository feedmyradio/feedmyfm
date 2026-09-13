// YAML reader for this project's config.yml and stations.yml.
//
// This is a thin adapter: the actual parsing is done by the vendored,
// header-only fkYAML (rx/src/third_party/fkYAML.hpp, MIT), a spec parser,
// so every config edge case (duplicate keys, tabs, quoting, empty docs)
// is its problem, not ours. We keep only the tiny `Value` facade below so
// resolve.hpp -- the one consumer -- compiles unchanged.
//
// The facade keeps one project-ism the spec does not have: every scalar is
// exposed as a string and `as_i64()/as_double()/as_bool()` coerce on demand.
// resolve.hpp relies on this (e.g. `stereo: true` is read with `.as_string()`
// first, then `.as_bool()`). Frequencies and rates are plain integers in Hz;
// `_` digit-group separators (`100_306_000`) are not valid YAML and are
// rejected -- the config files and examples all use plain digits.
//
// Parse errors (bad indentation, duplicate keys, unclosed flow, ...) come
// back as std::runtime_error with fkYAML's "<msg> (at line L, column C)"
// text, which resolve.hpp prefixes with the file name.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/fkYAML.hpp"

namespace fmrx::yaml {

class Value {
public:
    enum class Type { Null, Scalar, Map, Seq };

    Value() : m_type(Type::Null) {}

    Type type() const { return m_type; }
    bool is_null() const { return m_type == Type::Null; }
    bool is_scalar() const { return m_type == Type::Scalar; }
    bool is_map() const { return m_type == Type::Map; }
    bool is_seq() const { return m_type == Type::Seq; }

    // ---- map access -------------------------------------------------------
    bool has(const std::string& key) const {
        return m_type == Type::Map && m_map.find(key) != m_map.end();
    }
    const Value& at(const std::string& key) const {
        if (m_type != Type::Map)
            throw std::runtime_error("yaml: expected a mapping to read key '" +
                                     key + "'");
        auto it = m_map.find(key);
        if (it == m_map.end())
            throw std::runtime_error("yaml: missing key '" + key + "'");
        return it->second;
    }
    // like at(), but returns a static null Value instead of throwing
    const Value& get(const std::string& key) const {
        static const Value kNull;
        if (m_type != Type::Map)
            return kNull;
        auto it = m_map.find(key);
        return it == m_map.end() ? kNull : it->second;
    }
    const std::map<std::string, Value>& items() const { return m_map; }

    // ---- sequence access ------------------------------------------------
    const std::vector<Value>& seq() const {
        if (m_type != Type::Seq)
            throw std::runtime_error("yaml: expected a sequence");
        return m_seq;
    }

    // ---- scalar coercion ----------------------------------------------
    const std::string& raw() const {
        if (m_type != Type::Scalar)
            throw std::runtime_error("yaml: expected a scalar value");
        return m_scalar;
    }
    std::string as_string() const { return raw(); }

    long long as_i64() const {
        const std::string& s = raw();
        size_t pos = 0;
        long long v = std::stoll(s, &pos, 10);
        if (pos != s.size())
            throw std::runtime_error("yaml: '" + s + "' is not an integer");
        return v;
    }
    int as_int() const {
        long long v = as_i64();
        return static_cast<int>(v);
    }
    double as_double() const {
        const std::string& s = raw();
        size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos != s.size())
            throw std::runtime_error("yaml: '" + s + "' is not a number");
        return v;
    }
    bool as_bool() const {
        const std::string& s = raw();
        if (s == "true" || s == "True" || s == "yes" || s == "on")
            return true;
        if (s == "false" || s == "False" || s == "no" || s == "off")
            return false;
        throw std::runtime_error("yaml: '" + s + "' is not a boolean");
    }

    static Value parse(const std::string& text);

private:
    // Recursively lower an fkYAML node into a Value. Scalars are stringified
    // so `raw()` always has something to hand back; the numeric/bool getters
    // re-parse from there, exactly as the old parser did.
    static Value from_node(const ::fkyaml::node& n) {
        Value v;
        if (n.is_mapping()) {
            v.m_type = Type::Map;
            for (const auto& kv : n.as_map())
                v.m_map[key_string(kv.first)] = from_node(kv.second);
        } else if (n.is_sequence()) {
            v.m_type = Type::Seq;
            for (const auto& e : n.as_seq())
                v.m_seq.push_back(from_node(e));
        } else if (n.is_null()) {
            v.m_type = Type::Null;
        } else {
            v.m_type = Type::Scalar;
            v.m_scalar = scalar_string(n);
        }
        return v;
    }

    static std::string key_string(const ::fkyaml::node& k) {
        if (k.is_string())
            return k.as_str();
        if (k.is_null())
            return "";
        return scalar_string(k);
    }

    static std::string scalar_string(const ::fkyaml::node& n) {
        if (n.is_string())
            return n.as_str();
        if (n.is_boolean())
            return n.as_bool() ? "true" : "false";
        if (n.is_integer())
            return std::to_string(n.as_int());
        if (n.is_float_number()) {
            // %.17g round-trips a double exactly through stod().
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g",
                          static_cast<double>(n.as_float()));
            return buf;
        }
        return {};
    }

    Type m_type;
    std::string m_scalar;
    std::vector<Value> m_seq;
    std::map<std::string, Value> m_map;
};

inline Value Value::parse(const std::string& text) {
    try {
        return from_node(::fkyaml::node::deserialize(text.begin(), text.end()));
    } catch (const ::fkyaml::exception& e) {
        throw std::runtime_error(std::string("yaml: ") + e.what());
    }
}

} // namespace fmrx::yaml
