#pragma once

// A small JSON reader for fuserd's answers (UTF-8 in, UTF-8 strings out). Values are copied into a tree; a missing
// key or index yields a null value, so lookups can be chained.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace jsonl
{
    struct Value
    {
        enum class T
        {
            Null,
            Bool,
            Num,
            Str,
            Arr,
            Obj
        } t = T::Null;
        bool b = false;
        double n = 0;
        std::string s;
        std::vector<Value> a;
        std::vector<std::pair<std::string, Value>> o;

        auto operator[](const char* key) const -> const Value&
        {
            for (const auto& [k, v] : o)
                if (k == key) return v;
            return null();
        }
        auto at(size_t i) const -> const Value& { return i < a.size() ? a[i] : null(); }
        auto size() const -> size_t { return t == T::Arr ? a.size() : t == T::Obj ? o.size() : 0; }
        auto is_null() const -> bool { return t == T::Null; }
        auto str() const -> std::string { return t == T::Str ? s : std::string{}; }
        auto num() const -> double { return t == T::Num ? n : 0; }
        auto i64() const -> int64_t { return static_cast<int64_t>(num()); }
        auto boolean() const -> bool { return t == T::Bool && b; }
        static auto null() -> const Value&
        {
            static const Value v{};
            return v;
        }
    };

    namespace detail
    {
        struct Parser
        {
            const std::string& in;
            size_t i = 0;
            int depth = 0;

            auto ws() -> void
            {
                while (i < in.size() && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) ++i;
            }
            auto lit(const char* word) -> bool
            {
                const std::string w(word);
                if (in.compare(i, w.size(), w) != 0) return false;
                i += w.size();
                return true;
            }
            static auto utf8(std::string& out, uint32_t cp) -> void
            {
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
                else if (cp < 0x10000)
                    out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                else
                    out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            auto hex4(uint32_t* out) -> bool
            {
                if (i + 4 > in.size()) return false;
                uint32_t v = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const char c = in[i++];
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
                    else return false;
                }
                *out = v;
                return true;
            }
            auto string(std::string& out) -> bool
            {
                if (i >= in.size() || in[i] != '"') return false;
                ++i;
                while (i < in.size())
                {
                    const char c = in[i++];
                    if (c == '"') return true;
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (i >= in.size()) return false;
                    const char e = in[i++];
                    switch (e)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        uint32_t cp = 0;
                        if (!hex4(&cp)) return false;
                        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= in.size() && in[i] == '\\' && in[i + 1] == 'u')
                        {
                            i += 2;
                            uint32_t lo = 0;
                            if (!hex4(&lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: return false;
                    }
                }
                return false;
            }
            auto value(Value& v) -> bool
            {
                if (++depth > 64) return false;
                ws();
                if (i >= in.size()) return false;
                bool ok = false;
                const char c = in[i];
                if (c == '{')
                {
                    v.t = Value::T::Obj;
                    ++i;
                    ws();
                    if (i < in.size() && in[i] == '}') ++i, ok = true;
                    else
                        for (;;)
                        {
                            ws();
                            std::string key;
                            if (!string(key)) break;
                            ws();
                            if (i >= in.size() || in[i] != ':') break;
                            ++i;
                            Value item;
                            if (!value(item)) break;
                            v.o.emplace_back(std::move(key), std::move(item));
                            ws();
                            if (i < in.size() && in[i] == ',') { ++i; continue; }
                            if (i < in.size() && in[i] == '}') { ++i; ok = true; }
                            break;
                        }
                }
                else if (c == '[')
                {
                    v.t = Value::T::Arr;
                    ++i;
                    ws();
                    if (i < in.size() && in[i] == ']') ++i, ok = true;
                    else
                        for (;;)
                        {
                            Value item;
                            if (!value(item)) break;
                            v.a.push_back(std::move(item));
                            ws();
                            if (i < in.size() && in[i] == ',') { ++i; continue; }
                            if (i < in.size() && in[i] == ']') { ++i; ok = true; }
                            break;
                        }
                }
                else if (c == '"') v.t = Value::T::Str, ok = string(v.s);
                else if (lit("true")) v.t = Value::T::Bool, v.b = true, ok = true;
                else if (lit("false")) v.t = Value::T::Bool, ok = true;
                else if (lit("null")) ok = true;
                else
                {
                    const char* start = in.c_str() + i;
                    char* end = nullptr;
                    v.n = std::strtod(start, &end);
                    if (end != start)
                    {
                        v.t = Value::T::Num;
                        i += static_cast<size_t>(end - start);
                        ok = true;
                    }
                }
                --depth;
                return ok;
            }
        };
    } // namespace detail

    inline auto parse(const std::string& text, Value* out) -> bool
    {
        detail::Parser p{text};
        Value v;
        if (!p.value(v)) return false;
        p.ws();
        if (p.i != text.size()) return false;
        *out = std::move(v);
        return true;
    }
} // namespace jsonl
