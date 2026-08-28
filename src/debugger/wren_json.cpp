#include "wren_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wren
{
namespace debug
{
  Json Json::null() { return Json(); }

  Json Json::boolean(bool value)
  {
    Json json;
    json.type_ = Type::Bool;
    json.bool_ = value;
    return json;
  }

  Json Json::number(double value)
  {
    Json json;
    json.type_ = Type::Number;
    json.number_ = value;
    return json;
  }

  Json Json::integer(long long value)
  {
    Json json;
    json.type_ = Type::Number;
    json.number_ = static_cast<double>(value);
    return json;
  }

  Json Json::string(const char* value)
  {
    Json json;
    json.type_ = Type::String;
    json.string_ = value != nullptr ? value : "";
    return json;
  }

  Json Json::string(std::string value)
  {
    Json json;
    json.type_ = Type::String;
    json.string_ = std::move(value);
    return json;
  }

  Json Json::array()
  {
    Json json;
    json.type_ = Type::Array;
    return json;
  }

  Json Json::object()
  {
    Json json;
    json.type_ = Type::Object;
    return json;
  }

  void Json::set(const std::string& key, Json value)
  {
    for (auto& member : object_)
    {
      if (member.first == key)
      {
        member.second = std::move(value);
        return;
      }
    }
    object_.emplace_back(key, std::move(value));
  }

  void Json::setIf(bool condition, const std::string& key, Json value)
  {
    if (condition) set(key, std::move(value));
  }

  const Json* Json::find(const std::string& key) const
  {
    for (auto& member : object_)
    {
      if (member.first == key) return &member.second;
    }
    return nullptr;
  }

  std::string Json::getString(const std::string& key,
                              const std::string& fallback) const
  {
    const Json* value = find(key);
    if (value == nullptr || !value->isString()) return fallback;
    return value->string_;
  }

  int Json::getInt(const std::string& key, int fallback) const
  {
    const Json* value = find(key);
    if (value == nullptr || !value->isNumber()) return fallback;
    return static_cast<int>(value->number_);
  }

  bool Json::getBool(const std::string& key, bool fallback) const
  {
    const Json* value = find(key);
    if (value == nullptr || !value->isBool()) return fallback;
    return value->bool_;
  }

  void Json::append(Json value)
  {
    array_.push_back(std::move(value));
  }

  static void dumpString(const std::string& value, std::string* out)
  {
    out->push_back('"');
    for (char c : value)
    {
      switch (c)
      {
        case '"':  out->append("\\\""); break;
        case '\\': out->append("\\\\"); break;
        case '\b': out->append("\\b"); break;
        case '\f': out->append("\\f"); break;
        case '\n': out->append("\\n"); break;
        case '\r': out->append("\\r"); break;
        case '\t': out->append("\\t"); break;
        default:
          if (static_cast<unsigned char>(c) < 0x20)
          {
            char escaped[8];
            snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out->append(escaped);
          }
          else
          {
            out->push_back(c);
          }
      }
    }
    out->push_back('"');
  }

  static void dumpNumber(double value, std::string* out)
  {
    // Keep integral values integral so that DAP sequence numbers, ids, and
    // lines serialize as "3", not "3.0" (clients are meant to cope either
    // way, but integral numbers are friendlier to naive clients).
    if (std::isfinite(value) && value == std::floor(value) &&
        std::fabs(value) < 9.0e15)
    {
      char buffer[32];
      snprintf(buffer, sizeof(buffer), "%lld",
               static_cast<long long>(value));
      out->append(buffer);
    }
    else
    {
      char buffer[32];
      snprintf(buffer, sizeof(buffer), "%.17g", value);
      out->append(buffer);
    }
  }

  void Json::dumpTo(std::string* out) const
  {
    switch (type_)
    {
      case Type::Null:
        out->append("null");
        break;
      case Type::Bool:
        out->append(bool_ ? "true" : "false");
        break;
      case Type::Number:
        dumpNumber(number_, out);
        break;
      case Type::String:
        dumpString(string_, out);
        break;
      case Type::Array:
      {
        out->push_back('[');
        bool first = true;
        for (const Json& value : array_)
        {
          if (!first) out->push_back(',');
          first = false;
          value.dumpTo(out);
        }
        out->push_back(']');
        break;
      }
      case Type::Object:
      {
        out->push_back('{');
        bool first = true;
        for (const auto& member : object_)
        {
          if (!first) out->push_back(',');
          first = false;
          dumpString(member.first, out);
          out->push_back(':');
          member.second.dumpTo(out);
        }
        out->push_back('}');
        break;
      }
    }
  }

  std::string Json::dump() const
  {
    std::string out;
    dumpTo(&out);
    return out;
  }

  // Recursive-descent parser over the raw text.
  class Json::Parser
  {
    public:
      Parser(const std::string& text) : text_(text), position_(0) {}

      bool parse(Json* out)
      {
        skipWhitespace();
        if (!parseValue(out)) return false;
        skipWhitespace();
        return position_ == text_.size();
      }

    private:
      const std::string& text_;
      size_t position_;

      bool atEnd() const { return position_ >= text_.size(); }
      char peek() const { return text_[position_]; }

      bool consume(char c)
      {
        if (atEnd() || text_[position_] != c) return false;
        position_++;
        return true;
      }

      bool consumeLiteral(const char* literal)
      {
        size_t length = strlen(literal);
        if (text_.compare(position_, length, literal) != 0) return false;
        position_ += length;
        return true;
      }

      void skipWhitespace()
      {
        while (!atEnd())
        {
          char c = text_[position_];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r') position_++;
          else break;
        }
      }

      bool parseValue(Json* out)
      {
        skipWhitespace();
        if (atEnd()) return false;

        char c = peek();
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"')
        {
          std::string value;
          if (!parseString(&value)) return false;
          *out = string(std::move(value));
          return true;
        }
        if (c == 't')
        {
          if (!consumeLiteral("true")) return false;
          *out = boolean(true);
          return true;
        }
        if (c == 'f')
        {
          if (!consumeLiteral("false")) return false;
          *out = boolean(false);
          return true;
        }
        if (c == 'n')
        {
          if (!consumeLiteral("null")) return false;
          *out = null();
          return true;
        }
        return parseNumber(out);
      }

      bool parseObject(Json* out)
      {
        Json object = Json::object();
        position_++;
        skipWhitespace();
        if (consume('}'))
        {
          *out = std::move(object);
          return true;
        }

        for (;;)
        {
          skipWhitespace();
          std::string key;
          if (!parseString(&key)) return false;
          skipWhitespace();
          if (!consume(':')) return false;

          Json value;
          if (!parseValue(&value)) return false;
          object.set(key, std::move(value));

          if (consume(',')) continue;
          if (consume('}')) break;
          return false;
        }

        *out = std::move(object);
        return true;
      }

      bool parseArray(Json* out)
      {
        Json array = Json::array();
        position_++;
        skipWhitespace();
        if (consume(']'))
        {
          *out = std::move(array);
          return true;
        }

        for (;;)
        {
          Json value;
          if (!parseValue(&value)) return false;
          array.append(std::move(value));

          if (consume(',')) continue;
          if (consume(']')) break;
          return false;
        }

        *out = std::move(array);
        return true;
      }

      bool parseString(std::string* out)
      {
        if (!consume('"')) return false;
        std::string value;
        for (;;)
        {
          if (atEnd()) return false;
          char c = text_[position_++];
          if (c == '"') break;
          if (c != '\\')
          {
            value.push_back(c);
            continue;
          }

          if (atEnd()) return false;
          char escape = text_[position_++];
          switch (escape)
          {
            case '"':  value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/':  value.push_back('/'); break;
            case 'b':  value.push_back('\b'); break;
            case 'f':  value.push_back('\f'); break;
            case 'n':  value.push_back('\n'); break;
            case 'r':  value.push_back('\r'); break;
            case 't':  value.push_back('\t'); break;
            case 'u':
            {
              unsigned code;
              if (!parseHex4(&code)) return false;
              appendUtf8(code, &value);
              break;
            }
            default:
              return false;
          }
        }
        *out = std::move(value);
        return true;
      }

      bool parseHex4(unsigned* out)
      {
        if (position_ + 4 > text_.size()) return false;
        unsigned code = 0;
        for (int i = 0; i < 4; i++)
        {
          char c = text_[position_++];
          code <<= 4;
          if (c >= '0' && c <= '9') code += static_cast<unsigned>(c - '0');
          else if (c >= 'a' && c <= 'f') code += static_cast<unsigned>(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F') code += static_cast<unsigned>(c - 'A' + 10);
          else return false;
        }
        *out = code;
        return true;
      }

      static void appendUtf8(unsigned code, std::string* out)
      {
        if (code < 0x80)
        {
          out->push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
          out->push_back(static_cast<char>(0xC0 | (code >> 6)));
          out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
          out->push_back(static_cast<char>(0xE0 | (code >> 12)));
          out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
      }

      bool parseNumber(Json* out)
      {
        size_t start = position_;
        if (consume('-')) {}
        while (!atEnd() && peek() >= '0' && peek() <= '9') position_++;
        if (consume('.'))
        {
          while (!atEnd() && peek() >= '0' && peek() <= '9') position_++;
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E'))
        {
          position_++;
          if (!atEnd() && (peek() == '+' || peek() == '-')) position_++;
          while (!atEnd() && peek() >= '0' && peek() <= '9') position_++;
        }
        if (position_ == start) return false;

        *out = number(strtod(text_.c_str() + start, nullptr));
        return true;
      }
  };

  bool Json::parse(const std::string& text, Json* out)
  {
    Parser parser(text);
    return parser.parse(out);
  }
}
}
