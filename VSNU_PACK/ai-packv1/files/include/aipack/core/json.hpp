// =============================================================================
// AI Packs System - Minimal JSON Parser/Writer
// Lightweight JSON for embedded devices (no external dependency)
// =============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace aipack {
namespace json {

// Forward declaration
class Value;
using Object = std::unordered_map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    enum Type { Null, Bool, Integer, Number, String, ArrayType, ObjectType };

    Value() : type_(Null) {}
    Value(bool b) : type_(Bool), bool_(b) {}
    Value(int v) : type_(Integer), int_(v) {}
    Value(int64_t v) : type_(Integer), int_(v) {}
    Value(uint32_t v) : type_(Integer), int_(static_cast<int64_t>(v)) {}
    Value(uint16_t v) : type_(Integer), int_(static_cast<int64_t>(v)) {}
    Value(double v) : type_(Number), double_(v) {}
    Value(const char* s) : type_(String), string_(s) {}
    Value(const std::string& s) : type_(String), string_(s) {}
    Value(std::string&& s) : type_(String), string_(std::move(s)) {}
    Value(const Array& a) : type_(ArrayType), array_(a) {}
    Value(Array&& a) : type_(ArrayType), array_(std::move(a)) {}
    Value(const Object& o) : type_(ObjectType), object_(o) {}
    Value(Object&& o) : type_(ObjectType), object_(std::move(o)) {}

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isBool() const { return type_ == Bool; }
    bool isInt() const { return type_ == Integer; }
    bool isNumber() const { return type_ == Number || type_ == Integer; }
    bool isString() const { return type_ == String; }
    bool isArray() const { return type_ == ArrayType; }
    bool isObject() const { return type_ == ObjectType; }

    bool asBool(bool def = false) const {
        return type_ == Bool ? bool_ : def;
    }
    int64_t asInt(int64_t def = 0) const {
        if (type_ == Integer) return int_;
        if (type_ == Number) return static_cast<int64_t>(double_);
        return def;
    }
    double asDouble(double def = 0.0) const {
        if (type_ == Number) return double_;
        if (type_ == Integer) return static_cast<double>(int_);
        return def;
    }
    const std::string& asString(const std::string& def = "") const {
        return type_ == String ? string_ : def;
    }
    const Array& asArray() const {
        static Array empty;
        return type_ == ArrayType ? array_ : empty;
    }
    Array& asArray() {
        static Array empty;
        return type_ == ArrayType ? array_ : empty;
    }
    const Object& asObject() const {
        static Object empty;
        return type_ == ObjectType ? object_ : empty;
    }
    Object& asObject() {
        static Object empty;
        return type_ == ObjectType ? object_ : empty;
    }

    // Object access
    Value& operator[](const std::string& key) {
        if (type_ != ObjectType) {
            type_ = ObjectType;
            object_.clear();
        }
        return object_[key];
    }
    const Value& operator[](const std::string& key) const {
        static Value null;
        if (type_ != ObjectType) return null;
        auto it = object_.find(key);
        return it != object_.end() ? it->second : null;
    }

    bool has(const std::string& key) const {
        return type_ == ObjectType && object_.find(key) != object_.end();
    }

    // Convenience: get string from object
    std::string getString(const std::string& key,
                          const std::string& def = "") const {
        if (!has(key)) return def;
        return (*this)[key].asString(def);
    }

    int64_t getInt(const std::string& key, int64_t def = 0) const {
        if (!has(key)) return def;
        return (*this)[key].asInt(def);
    }

    bool getBool(const std::string& key, bool def = false) const {
        if (!has(key)) return def;
        return (*this)[key].asBool(def);
    }

    std::vector<std::string> getStringArray(const std::string& key) const {
        std::vector<std::string> result;
        if (has(key) && (*this)[key].isArray()) {
            for (const auto& v : (*this)[key].asArray()) {
                if (v.isString()) result.push_back(v.asString());
            }
        }
        return result;
    }

    // Array access
    Value& operator[](size_t idx) {
        if (type_ != ArrayType) {
            type_ = ArrayType;
            array_.clear();
        }
        if (idx >= array_.size()) array_.resize(idx + 1);
        return array_[idx];
    }

    void push_back(const Value& v) {
        if (type_ != ArrayType) {
            type_ = ArrayType;
            array_.clear();
        }
        array_.push_back(v);
    }

    size_t size() const {
        if (type_ == ArrayType) return array_.size();
        if (type_ == ObjectType) return object_.size();
        return 0;
    }

    // Serialization
    std::string dump(int indent = 2) const {
        std::ostringstream ss;
        dumpImpl(ss, indent, 0);
        return ss.str();
    }

private:
    Type type_;
    bool bool_ = false;
    int64_t int_ = 0;
    double double_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;

    void dumpImpl(std::ostringstream& ss, int indent, int depth) const {
        std::string pad(depth * indent, ' ');
        std::string padInner((depth + 1) * indent, ' ');

        switch (type_) {
            case Null: ss << "null"; break;
            case Bool: ss << (bool_ ? "true" : "false"); break;
            case Integer: ss << int_; break;
            case Number: ss << double_; break;
            case String:
                ss << "\"";
                for (char c : string_) {
                    switch (c) {
                        case '"': ss << "\\\""; break;
                        case '\\': ss << "\\\\"; break;
                        case '\n': ss << "\\n"; break;
                        case '\r': ss << "\\r"; break;
                        case '\t': ss << "\\t"; break;
                        default: ss << c;
                    }
                }
                ss << "\"";
                break;
            case ArrayType:
                if (array_.empty()) { ss << "[]"; break; }
                ss << "[\n";
                for (size_t i = 0; i < array_.size(); ++i) {
                    ss << padInner;
                    array_[i].dumpImpl(ss, indent, depth + 1);
                    if (i + 1 < array_.size()) ss << ",";
                    ss << "\n";
                }
                ss << pad << "]";
                break;
            case ObjectType:
                if (object_.empty()) { ss << "{}"; break; }
                ss << "{\n";
                {
                    // Sort keys for deterministic output
                    std::vector<std::string> keys;
                    for (const auto& kv : object_) keys.push_back(kv.first);
                    std::sort(keys.begin(), keys.end());
                    for (size_t i = 0; i < keys.size(); ++i) {
                        ss << padInner << "\"" << keys[i] << "\": ";
                        object_.at(keys[i]).dumpImpl(ss, indent, depth + 1);
                        if (i + 1 < keys.size()) ss << ",";
                        ss << "\n";
                    }
                }
                ss << pad << "}";
                break;
        }
    }
};

// =============================================================================
// JSON Parser
// =============================================================================
class Parser {
public:
    static Value parse(const std::string& input) {
        Parser p(input);
        Value result = p.parseValue();
        p.skipWhitespace();
        return result;
    }

private:
    const std::string& input_;
    size_t pos_ = 0;

    Parser(const std::string& input) : input_(input) {}

    void skipWhitespace() {
        while (pos_ < input_.size() &&
               (input_[pos_] == ' ' || input_[pos_] == '\n' ||
                input_[pos_] == '\r' || input_[pos_] == '\t'))
            ++pos_;
    }

    char peek() {
        skipWhitespace();
        return pos_ < input_.size() ? input_[pos_] : '\0';
    }

    char next() {
        if (pos_ >= input_.size())
            throw std::runtime_error("JSON: unexpected end of input");
        return input_[pos_++];
    }

    void expect(char c) {
        skipWhitespace();
        char got = next();
        if (got != c) {
            throw std::runtime_error(
                std::string("JSON: expected '") + c + "' but got '" + got + "'");
        }
    }

    Value parseValue() {
        char c = peek();
        switch (c) {
            case '"': return parseString();
            case '{': return parseObject();
            case '[': return parseArray();
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parseNumber();
                throw std::runtime_error(
                    std::string("JSON: unexpected character '") + c + "'");
        }
    }

    Value parseString() {
        expect('"');
        std::string result;
        while (pos_ < input_.size() && input_[pos_] != '"') {
            if (input_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= input_.size())
                    throw std::runtime_error("JSON: unterminated string");
                switch (input_[pos_]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    default: result += input_[pos_];
                }
            } else {
                result += input_[pos_];
            }
            ++pos_;
        }
        if (pos_ >= input_.size())
            throw std::runtime_error("JSON: unterminated string");
        ++pos_; // skip closing "
        return Value(std::move(result));
    }

    Value parseNumber() {
        skipWhitespace();
        size_t start = pos_;
        bool isFloat = false;

        if (input_[pos_] == '-') ++pos_;
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
            ++pos_;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            isFloat = true;
            ++pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
                ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-'))
                ++pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
                ++pos_;
        }

        std::string numStr = input_.substr(start, pos_ - start);
        if (isFloat)
            return Value(std::stod(numStr));
        else
            return Value(static_cast<int64_t>(std::stoll(numStr)));
    }

    Value parseBool() {
        skipWhitespace();
        if (input_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value(true);
        }
        if (input_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value(false);
        }
        throw std::runtime_error("JSON: invalid boolean");
    }

    Value parseNull() {
        skipWhitespace();
        if (input_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return Value();
        }
        throw std::runtime_error("JSON: invalid null");
    }

    Value parseArray() {
        expect('[');
        Array arr;
        if (peek() == ']') { next(); return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parseValue());
            char c = peek();
            if (c == ']') { next(); break; }
            expect(',');
        }
        return Value(std::move(arr));
    }

    Value parseObject() {
        expect('{');
        Object obj;
        if (peek() == '}') { next(); return Value(std::move(obj)); }
        while (true) {
            Value key = parseString();
            expect(':');
            Value val = parseValue();
            obj[key.asString()] = std::move(val);
            char c = peek();
            if (c == '}') { next(); break; }
            expect(',');
        }
        return Value(std::move(obj));
    }
};

// Convenience function
inline Value parse(const std::string& input) {
    return Parser::parse(input);
}

} // namespace json
} // namespace aipack
