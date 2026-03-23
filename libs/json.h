// json.h — Minimal JSON parser and serializer. Header-only, no exceptions.
// Handles the full JSON spec but exposes only the subset needed by the app:
// objects, strings, integers, and booleans.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace json {

class Value {
 public:
  enum Type { Null, Object, Array, String, Number, Boolean, Discarded };

  Value() : type_(Type::Null) {}
  Value(const char *s) : type_(Type::String), str_(s) {}
  Value(const std::string &s) : type_(Type::String), str_(s) {}
  Value(std::string &&s) : type_(Type::String), str_(std::move(s)) {}
  Value(int v) : type_(Type::Number), num_(v) {}
  Value(bool v) : type_(Type::Boolean), bool_(v) {}

  // Type checks
  bool is_discarded() const { return type_ == Type::Discarded; }
  bool is_string() const { return type_ == Type::String; }
  bool is_boolean() const { return type_ == Type::Boolean; }
  bool is_number_integer() const {
    return type_ == Type::Number &&
           num_ == static_cast<double>(static_cast<int64_t>(num_));
  }

  // Object key lookup
  bool contains(const std::string &key) const {
    if (type_ != Type::Object) return false;
    for (auto &[k, v] : obj_) {
      if (k == key) return true;
    }
    return false;
  }

  // Const access — returns static null for missing keys
  const Value &operator[](const std::string &key) const {
    static const Value null_val;
    if (type_ != Type::Object) return null_val;
    for (auto &[k, v] : obj_) {
      if (k == key) return v;
    }
    return null_val;
  }

  // Mutable access — promotes Null to Object, inserts missing keys
  Value &operator[](const std::string &key) {
    if (type_ == Type::Null) type_ = Type::Object;
    for (auto &[k, v] : obj_) {
      if (k == key) return v;
    }
    obj_.emplace_back(key, Value());
    return obj_.back().second;
  }

  // Type-safe value extraction (returns default on type mismatch)
  template <typename T>
  T get() const {
    if constexpr (std::is_same_v<T, int>) {
      return (type_ == Type::Number) ? static_cast<int>(num_) : 0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return (type_ == Type::Boolean) ? bool_ : false;
    } else if constexpr (std::is_same_v<T, std::string>) {
      return (type_ == Type::String) ? str_ : std::string{};
    } else {
      return T{};
    }
  }

  // Serialize to JSON string
  std::string dump() const {
    std::string out;
    serialize(out);
    return out;
  }

  // Parse JSON string (returns Discarded value on error)
  static Value parse(const std::string &input) {
    Parser p(input.data(), input.data() + input.size());
    Value v = p.parseValue();
    if (p.failed) return discarded();
    return v;
  }

 private:
  Type type_ = Type::Null;
  std::string str_;
  double num_ = 0;
  bool bool_ = false;
  std::vector<std::pair<std::string, Value>> obj_;
  std::vector<Value> arr_;

  static Value discarded() {
    Value v;
    v.type_ = Type::Discarded;
    return v;
  }

  // --- Serializer ---

  void serialize(std::string &out) const {
    switch (type_) {
      case Type::Object: {
        out += '{';
        bool first = true;
        for (auto &[k, v] : obj_) {
          if (!first) out += ',';
          first = false;
          escapeString(out, k);
          out += ':';
          v.serialize(out);
        }
        out += '}';
        break;
      }
      case Type::Array: {
        out += '[';
        bool first = true;
        for (auto &v : arr_) {
          if (!first) out += ',';
          first = false;
          v.serialize(out);
        }
        out += ']';
        break;
      }
      case Type::String:
        escapeString(out, str_);
        break;
      case Type::Number:
        if (num_ == static_cast<double>(static_cast<int64_t>(num_))) {
          out += std::to_string(static_cast<int64_t>(num_));
        } else {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.17g", num_);
          out += buf;
        }
        break;
      case Type::Boolean:
        out += bool_ ? "true" : "false";
        break;
      default:
        out += "null";
        break;
    }
  }

  static void escapeString(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    out += '"';
  }

  // --- Parser ---

  struct Parser {
    const char *p;
    const char *end;
    bool failed = false;

    Parser(const char *b, const char *e) : p(b), end(e) {}

    void skipWS() {
      while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
    }

    char peek() { return p < end ? *p : '\0'; }
    char next() { return p < end ? *p++ : '\0'; }

    bool expect(char c) {
      skipWS();
      if (peek() == c) {
        ++p;
        return true;
      }
      failed = true;
      return false;
    }

    Value parseValue() {
      skipWS();
      if (failed || p >= end) {
        failed = true;
        return Value();
      }
      switch (peek()) {
        case '{': return parseObject();
        case '[': return parseArray();
        case '"': return parseString();
        case 't':
        case 'f': return parseBool();
        case 'n': return parseNull();
        default: return parseNumber();
      }
    }

    Value parseObject() {
      Value v;
      v.type_ = Type::Object;
      ++p;  // skip '{'
      skipWS();
      if (peek() == '}') {
        ++p;
        return v;
      }
      while (!failed) {
        skipWS();
        Value key = parseString();
        if (failed) break;
        if (!expect(':')) break;
        Value val = parseValue();
        if (failed) break;
        v.obj_.emplace_back(std::move(key.str_), std::move(val));
        skipWS();
        if (peek() == ',') {
          ++p;
          continue;
        }
        break;
      }
      expect('}');
      return v;
    }

    Value parseArray() {
      Value v;
      v.type_ = Type::Array;
      ++p;  // skip '['
      skipWS();
      if (peek() == ']') {
        ++p;
        return v;
      }
      while (!failed) {
        v.arr_.push_back(parseValue());
        if (failed) break;
        skipWS();
        if (peek() == ',') {
          ++p;
          continue;
        }
        break;
      }
      expect(']');
      return v;
    }

    Value parseString() {
      skipWS();
      if (next() != '"') {
        failed = true;
        return Value();
      }
      std::string s;
      while (p < end) {
        char c = next();
        if (c == '"') {
          Value v;
          v.type_ = Type::String;
          v.str_ = std::move(s);
          return v;
        }
        if (c == '\\') {
          char esc = next();
          switch (esc) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
              if (p + 4 > end) {
                failed = true;
                return Value();
              }
              char hex[5] = {p[0], p[1], p[2], p[3], '\0'};
              auto cp =
                  static_cast<uint16_t>(std::strtoul(hex, nullptr, 16));
              p += 4;
              // Encode as UTF-8
              if (cp < 0x80) {
                s += static_cast<char>(cp);
              } else if (cp < 0x800) {
                s += static_cast<char>(0xC0 | (cp >> 6));
                s += static_cast<char>(0x80 | (cp & 0x3F));
              } else {
                s += static_cast<char>(0xE0 | (cp >> 12));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
              }
              break;
            }
            default:
              failed = true;
              return Value();
          }
        } else {
          s += c;
        }
      }
      failed = true;
      return Value();
    }

    Value parseNumber() {
      const char *start = p;
      if (peek() == '-') ++p;
      if (p >= end || *p < '0' || *p > '9') {
        failed = true;
        return Value();
      }
      while (p < end && *p >= '0' && *p <= '9') ++p;
      bool is_float = false;
      if (p < end && *p == '.') {
        is_float = true;
        ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
      }
      if (p < end && (*p == 'e' || *p == 'E')) {
        is_float = true;
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
      }
      Value v;
      v.type_ = Type::Number;
      if (is_float) {
        v.num_ = std::strtod(start, nullptr);
      } else {
        v.num_ = static_cast<double>(std::strtoll(start, nullptr, 10));
      }
      return v;
    }

    Value parseBool() {
      if (p + 4 <= end && std::memcmp(p, "true", 4) == 0) {
        p += 4;
        return Value(true);
      }
      if (p + 5 <= end && std::memcmp(p, "false", 5) == 0) {
        p += 5;
        return Value(false);
      }
      failed = true;
      return Value();
    }

    Value parseNull() {
      if (p + 4 <= end && std::memcmp(p, "null", 4) == 0) {
        p += 4;
        return Value();
      }
      failed = true;
      return Value();
    }
  };
};

}  // namespace json
