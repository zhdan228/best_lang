/*
 * Система типов BestLang.
 *
 * Type — иммутабельный объект, передаётся через shared_ptr.
 * Номинальная типизация: два типа совместимы только если совпадают имена.
 * Для составных типов (массивы, кортежи, nullable) — структурная эквивалентность.
 */

#pragma once
#include <string>
#include <memory>
#include <vector>
#include <cstdint>

struct Type {
    enum class Kind {
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float32, Float64,
        Bool, String, Void,
        Array,    // массив фиксированного размера: elem_type + array_size
        DynArray, // динамический массив: только elem_type
        Tuple,    // кортеж: tuple_elems
        Nullable, // nullable тип T?: elem_type
        Struct,   // структура: struct_name
    };

    Kind kind;
    std::shared_ptr<Type>              elem_type;       // Array, DynArray, Nullable (тип элемента)
    int64_t                            array_size = 0;  // Array (размер)
    std::vector<std::shared_ptr<Type>> tuple_elems;     // Tuple (элементы)
    std::string                        struct_name;     // Struct (имя)

    // Фабричные методы
    static std::shared_ptr<Type> make(Kind k) {
        auto t = std::make_shared<Type>(); t->kind = k; return t;
    }
    static std::shared_ptr<Type> make_array(std::shared_ptr<Type> elem, int64_t n) {
        auto t = std::make_shared<Type>();
        t->kind = Kind::Array; t->elem_type = std::move(elem); t->array_size = n;
        return t;
    }
    static std::shared_ptr<Type> make_dynarray(std::shared_ptr<Type> elem) {
        auto t = std::make_shared<Type>();
        t->kind = Kind::DynArray; t->elem_type = std::move(elem);
        return t;
    }
    static std::shared_ptr<Type> make_tuple(std::vector<std::shared_ptr<Type>> elems) {
        auto t = std::make_shared<Type>();
        t->kind = Kind::Tuple; t->tuple_elems = std::move(elems);
        return t;
    }
    static std::shared_ptr<Type> make_nullable(std::shared_ptr<Type> inner) {
        auto t = std::make_shared<Type>();
        t->kind = Kind::Nullable; t->elem_type = std::move(inner);
        return t;
    }
    static std::shared_ptr<Type> make_struct(const std::string& name) {
        auto t = std::make_shared<Type>();
        t->kind = Kind::Struct; t->struct_name = name;
        return t;
    }

    // Предикаты
    bool is_signed_int()   const { return kind==Kind::Int8||kind==Kind::Int16||kind==Kind::Int32||kind==Kind::Int64; }
    bool is_unsigned_int() const { return kind==Kind::UInt8||kind==Kind::UInt16||kind==Kind::UInt32||kind==Kind::UInt64; }
    bool is_int()          const { return is_signed_int() || is_unsigned_int(); }
    bool is_float()        const { return kind==Kind::Float32 || kind==Kind::Float64; }
    bool is_numeric()      const { return is_int() || is_float(); }
    bool is_bool()         const { return kind==Kind::Bool; }
    bool is_string()       const { return kind==Kind::String; }
    bool is_void()         const { return kind==Kind::Void; }
    bool is_array()        const { return kind==Kind::Array; }
    bool is_dynarray()     const { return kind==Kind::DynArray; }
    bool is_tuple()        const { return kind==Kind::Tuple; }
    bool is_nullable()     const { return kind==Kind::Nullable; }
    bool is_struct()       const { return kind==Kind::Struct; }

    // Разрядность целочисленных типов
    int int_bits() const {
        switch (kind) {
        case Kind::Int8:  case Kind::UInt8:  return 8;
        case Kind::Int16: case Kind::UInt16: return 16;
        case Kind::Int32: case Kind::UInt32: return 32;
        case Kind::Int64: case Kind::UInt64: return 64;
        default: return 0;
        }
    }

    bool operator==(const Type& o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::Array)
            return array_size == o.array_size && *elem_type == *o.elem_type;
        if (kind == Kind::DynArray || kind == Kind::Nullable)
            return *elem_type == *o.elem_type;
        if (kind == Kind::Tuple) {
            if (tuple_elems.size() != o.tuple_elems.size()) return false;
            for (size_t i = 0; i < tuple_elems.size(); ++i)
                if (*tuple_elems[i] != *o.tuple_elems[i]) return false;
            return true;
        }
        if (kind == Kind::Struct) return struct_name == o.struct_name;
        return true;
    }
    bool operator!=(const Type& o) const { return !(*this == o); }

    // Текстовое представление типа
    std::string to_string() const {
        switch (kind) {
        case Kind::Int8:    return "int8";
        case Kind::Int16:   return "int16";
        case Kind::Int32:   return "int32";
        case Kind::Int64:   return "int64";
        case Kind::UInt8:   return "uint8";
        case Kind::UInt16:  return "uint16";
        case Kind::UInt32:  return "uint32";
        case Kind::UInt64:  return "uint64";
        case Kind::Float32: return "float32";
        case Kind::Float64: return "float64";
        case Kind::Bool:    return "bool";
        case Kind::String:  return "string";
        case Kind::Void:    return "void";
        case Kind::Array:
            return "[" + elem_type->to_string() + "; " + std::to_string(array_size) + "]";
        case Kind::DynArray:
            return "[" + elem_type->to_string() + "]";
        case Kind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < tuple_elems.size(); ++i) {
                if (i) s += ", ";
                s += tuple_elems[i]->to_string();
            }
            return s + ")";
        }
        case Kind::Nullable:
            return elem_type->to_string() + "?";
        case Kind::Struct:
            return struct_name;
        }
        return "?";
    }
};

using TypePtr = std::shared_ptr<Type>;

// Глобальные синглтоны для часто используемых типов
inline TypePtr TYPE_INT8    = Type::make(Type::Kind::Int8);
inline TypePtr TYPE_INT16   = Type::make(Type::Kind::Int16);
inline TypePtr TYPE_INT32   = Type::make(Type::Kind::Int32);
inline TypePtr TYPE_INT64   = Type::make(Type::Kind::Int64);
inline TypePtr TYPE_UINT8   = Type::make(Type::Kind::UInt8);
inline TypePtr TYPE_UINT16  = Type::make(Type::Kind::UInt16);
inline TypePtr TYPE_UINT32  = Type::make(Type::Kind::UInt32);
inline TypePtr TYPE_UINT64  = Type::make(Type::Kind::UInt64);
inline TypePtr TYPE_FLOAT32 = Type::make(Type::Kind::Float32);
inline TypePtr TYPE_FLOAT64 = Type::make(Type::Kind::Float64);
inline TypePtr TYPE_BOOL    = Type::make(Type::Kind::Bool);
inline TypePtr TYPE_STRING  = Type::make(Type::Kind::String);
inline TypePtr TYPE_VOID    = Type::make(Type::Kind::Void);
