#pragma once
// Minimal dependency-free JSON parser — just enough for conformance case.json
// (objects, arrays, strings, numbers, bools, null). Not a general-purpose lib.

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cjson {

struct Value;
using Array  = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    enum Type { Null, Boolean, Num, Str, Arr, Obj } type = Null;
    bool                    b   = false;
    double                  num = 0;
    std::string             str;
    std::shared_ptr<Array>  arr;
    std::shared_ptr<Object> obj;

    const Value* find(const std::string& k) const {
        if (type == Obj && obj) { auto it = obj->find(k); if (it != obj->end()) return &it->second; }
        return nullptr;
    }
    double      as_num(double d = 0) const { return type == Num ? num : d; }
    std::string as_str(const std::string& d = "") const { return type == Str ? str : d; }
    int         at_int(size_t i, int d = 0) const {
        return (type == Arr && arr && i < arr->size()) ? (int)(*arr)[i].as_num(d) : d;
    }
};

struct Parser {
    const char* p;
    const char* end;
    void ws() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }

    bool value(Value& v) {
        ws();
        if (p >= end) return false;
        switch (*p) {
            case '{': return object(v);
            case '[': return array(v);
            case '"': v.type = Value::Str; return string(v.str);
            case 't': case 'f': return boolean(v);
            case 'n': if (end - p >= 4) { p += 4; v.type = Value::Null; return true; } return false;
            default:  return number(v);
        }
    }
    bool string(std::string& s) {
        if (*p != '"') return false; ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                if (++p >= end) return false; char e = *p++;
                switch (e) { case 'n': s+='\n'; break; case 't': s+='\t'; break;
                             case '"': s+='"'; break; case '\\': s+='\\'; break;
                             case '/': s+='/'; break; default: s += e; }
            } else s += *p++;
        }
        if (p < end && *p == '"') { ++p; return true; }
        return false;
    }
    bool number(Value& v) { char* e; v.num = std::strtod(p, &e); if (e == p) return false; p = e; v.type = Value::Num; return true; }
    bool boolean(Value& v) {
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0)  { p += 4; v.type = Value::Boolean; v.b = true;  return true; }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v.type = Value::Boolean; v.b = false; return true; }
        return false;
    }
    bool array(Value& v) {
        ++p; v.type = Value::Arr; v.arr = std::make_shared<Array>(); ws();
        if (p < end && *p == ']') { ++p; return true; }
        while (p < end) {
            Value e; if (!value(e)) return false; v.arr->push_back(std::move(e)); ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return false;
        }
        return false;
    }
    bool object(Value& v) {
        ++p; v.type = Value::Obj; v.obj = std::make_shared<Object>(); ws();
        if (p < end && *p == '}') { ++p; return true; }
        while (p < end) {
            ws(); std::string k;
            if (p >= end || *p != '"' || !string(k)) return false;
            ws(); if (p >= end || *p != ':') return false; ++p;
            Value val; if (!value(val)) return false; (*v.obj)[k] = std::move(val); ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return false;
        }
        return false;
    }
};

inline bool parse(const std::string& text, Value& out) {
    Parser pr{ text.c_str(), text.c_str() + text.size() };
    return pr.value(out);
}

}  // namespace cjson
