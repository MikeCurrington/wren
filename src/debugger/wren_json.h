#pragma once
#ifndef wren_json_h
#define wren_json_h

#include <string>
#include <utility>
#include <vector>

namespace wren
{
namespace debug
{
  // A minimal JSON value, just rich enough for the Debug Adapter Protocol:
  // null, bool, number, string, array, and object. Objects preserve insertion
  // order so that serialized messages are stable and readable.
  class Json
  {
    public:
      enum class Type
      {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
      };

      Json() = default;

      static Json null();
      static Json boolean(bool value);
      static Json number(double value);
      static Json integer(long long value);
      static Json string(const char* value);
      static Json string(std::string value);
      static Json array();
      static Json object();

      Type type() const { return type_; }

      bool isNull() const { return type_ == Type::Null; }
      bool isBool() const { return type_ == Type::Bool; }
      bool isNumber() const { return type_ == Type::Number; }
      bool isString() const { return type_ == Type::String; }
      bool isArray() const { return type_ == Type::Array; }
      bool isObject() const { return type_ == Type::Object; }

      // Accessors. These do not check the value's type; callers are expected
      // to know the shape of the messages they process.
      bool boolValue() const { return bool_; }
      double numberValue() const { return number_; }
      const std::string& stringValue() const { return string_; }
      const std::vector<Json>& arrayValue() const { return array_; }

      // Object members.
      void set(const std::string& key, Json value);
      void setIf(bool condition, const std::string& key, Json value);

      // Returns nullptr if [key] is not present.
      const Json* find(const std::string& key) const;

      // Convenience typed lookups with defaults.
      std::string getString(const std::string& key,
                            const std::string& fallback = "") const;
      int getInt(const std::string& key, int fallback = 0) const;
      bool getBool(const std::string& key, bool fallback = false) const;

      // Array members.
      void append(Json value);
      size_t size() const { return array_.size(); }

      // Serializes to compact JSON.
      std::string dump() const;

      // Parses [text] into [out]. Returns false and leaves [out] untouched if
      // the text is not a single well-formed JSON value.
      static bool parse(const std::string& text, Json* out);

    private:
      Type type_ = Type::Null;
      bool bool_ = false;
      double number_ = 0;
      std::string string_;
      std::vector<Json> array_;
      std::vector<std::pair<std::string, Json>> object_;

      void dumpTo(std::string* out) const;

      class Parser;
  };
}
}

#endif
