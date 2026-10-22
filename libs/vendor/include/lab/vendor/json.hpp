#pragma once

// Just enough JSON for the OMM JSON vendor: a small DOM parser for requests
// and responses, and helpers for writing values. Numbers keep their source
// text so prices convert to ITCH Price(4) exactly, never through a double.
// Control path code: it allocates.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lab::vendor::json {

struct Member;

struct Value {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject } type = Type::kNull;
  bool b = false;
  std::string text;  // number text or string contents
  std::vector<Value> items;
  std::vector<Member> members;  // in source order

  bool is_null() const noexcept { return type == Type::kNull; }
  bool is_string() const noexcept { return type == Type::kString; }
  bool is_number() const noexcept { return type == Type::kNumber; }
  bool is_array() const noexcept { return type == Type::kArray; }
  bool is_object() const noexcept { return type == Type::kObject; }

  const Value* get(std::string_view key) const noexcept;
  std::string_view str(std::string_view key) const noexcept;
};

// An object member. A plain struct rather than std::pair<std::string, Value>:
// std::pair's constructor traits need Value complete, which it is not inside
// its own definition, and clang rejects that.
struct Member {
  std::string first;
  Value second;
};

inline const Value* Value::get(std::string_view key) const noexcept {
  for (const auto& [k, v] : members) {
    if (k == key) return &v;
  }
  return nullptr;
}
inline std::string_view Value::str(std::string_view key) const noexcept {
  const Value* v = get(key);
  return v != nullptr && v->is_string() ? std::string_view{v->text} : std::string_view{};
}

class Parser {
 public:
  explicit Parser(std::string_view s) : s_{s} {}

  std::optional<Value> parse() {
    Value v;
    if (!value(v)) return std::nullopt;
    ws();
    if (i_ != s_.size()) return std::nullopt;
    return v;
  }

 private:
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t'))
      ++i_;
  }
  bool lit(std::string_view w) {
    if (s_.substr(i_, w.size()) != w) return false;
    i_ += w.size();
    return true;
  }
  bool string(std::string& out) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c == '\\') {
        if (i_ >= s_.size()) return false;
        const char e = s_[i_++];
        switch (e) {
          case 'n':
            c = '\n';
            break;
          case 't':
            c = '\t';
            break;
          case 'r':
            c = '\r';
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'u': {
            if (i_ + 4 > s_.size()) return false;
            const unsigned cp =
                static_cast<unsigned>(std::stoul(std::string{s_.substr(i_, 4)}, nullptr, 16));
            i_ += 4;
            if (cp < 0x80) {
              c = static_cast<char>(cp);
            } else {  // the lab only needs ASCII; keep the rest as UTF-8
              if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
              } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              }
              c = static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
          }
          default:
            c = e;
        }
      }
      out += c;
    }
    if (i_ >= s_.size()) return false;
    ++i_;
    return true;
  }
  bool value(Value& v) {
    ws();
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '{') {
      ++i_;
      v.type = Value::Type::kObject;
      ws();
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return true;
      }
      for (;;) {
        ws();
        std::string k;
        if (!string(k)) return false;
        ws();
        if (i_ >= s_.size() || s_[i_] != ':') return false;
        ++i_;
        Value m;
        if (!value(m)) return false;
        v.members.push_back({std::move(k), std::move(m)});
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == '}') {
          ++i_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++i_;
      v.type = Value::Type::kArray;
      ws();
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return true;
      }
      for (;;) {
        Value m;
        if (!value(m)) return false;
        v.items.push_back(std::move(m));
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == ']') {
          ++i_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      v.type = Value::Type::kString;
      return string(v.text);
    }
    if (lit("null")) return true;
    if (lit("true")) {
      v.type = Value::Type::kBool;
      v.b = true;
      return true;
    }
    if (lit("false")) {
      v.type = Value::Type::kBool;
      return true;
    }
    const std::size_t start = i_;
    while (i_ < s_.size() &&
           (std::string_view{"+-.eE0123456789"}.find(s_[i_]) != std::string_view::npos))
      ++i_;
    if (i_ == start) return false;
    v.type = Value::Type::kNumber;
    v.text = s_.substr(start, i_ - start);
    return true;
  }

  std::string_view s_;
  std::size_t i_ = 0;
};

inline std::optional<Value> parse(std::string_view s) { return Parser{s}.parse(); }

// Appends `s` as a JSON string literal.
inline void quote(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// Price(4) as the shortest exact decimal: 1895100 -> 189.51.
inline void price(std::string& out, std::int64_t p4) {
  if (p4 < 0) {
    out += '-';
    p4 = -p4;
  }
  out += std::to_string(p4 / 10000);
  std::int64_t frac = p4 % 10000;
  if (frac == 0) return;
  char d[5] = {static_cast<char>('0' + frac / 1000), static_cast<char>('0' + frac / 100 % 10),
               static_cast<char>('0' + frac / 10 % 10), static_cast<char>('0' + frac % 10), 0};
  int n = 4;
  while (n > 0 && d[n - 1] == '0') --n;
  out += '.';
  out.append(d, static_cast<std::size_t>(n));
}

// A decimal number's text as Price(4), exactly; nullopt if it has more than
// four decimals or is not a plain decimal.
inline std::optional<std::int64_t> parse_price(std::string_view t) {
  bool neg = false;
  if (!t.empty() && t.front() == '-') {
    neg = true;
    t.remove_prefix(1);
  }
  std::int64_t whole = 0, frac = 0;
  int digits = 0;
  bool dot = false, any = false;
  for (const char c : t) {
    if (c == '.') {
      if (dot) return std::nullopt;
      dot = true;
      continue;
    }
    if (c < '0' || c > '9') return std::nullopt;
    any = true;
    if (!dot) {
      whole = whole * 10 + (c - '0');
    } else {
      if (++digits > 4) {
        if (c != '0') return std::nullopt;
        continue;
      }
      frac = frac * 10 + (c - '0');
    }
  }
  if (!any) return std::nullopt;
  for (int k = digits > 4 ? 4 : digits; k < 4; ++k) frac *= 10;
  const std::int64_t v = whole * 10000 + frac;
  return neg ? -v : v;
}

// ns since midnight as an ISO 8601 time with nanoseconds: 13:30:00.044123456.
inline void time_ns(std::string& out, std::uint64_t ns) {
  char b[32];
  const std::uint64_t s = ns / 1'000'000'000ULL;
  std::snprintf(
      b, sizeof b, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
      static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
      static_cast<unsigned long long>(ns % 1'000'000'000ULL));
  out += '"';
  out += b;
  out += '"';
}

inline std::optional<std::uint64_t> parse_time_ns(std::string_view t) {
  unsigned long long h = 0, m = 0, s = 0, frac = 0;
  int fd = 0;
  if (std::sscanf(std::string{t}.c_str(), "%llu:%llu:%llu.%llu%n", &h, &m, &s, &frac, &fd) < 3) {
    return std::nullopt;
  }
  const auto dot = t.find('.');
  std::uint64_t ns = 0;
  if (dot != std::string_view::npos) {
    std::string digits{t.substr(dot + 1)};
    digits.resize(9, '0');
    ns = std::stoull(digits.substr(0, 9));
  }
  return ((h * 60 + m) * 60 + s) * 1'000'000'000ULL + ns;
}

}  // namespace lab::vendor::json
