#pragma once
#include <string>
#include <memory>
#include <vector>
#include <cstdint>
#include <stdexcept>

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct ArrayType;
struct DynArrayType;
struct TupleType;
struct NullableType;
struct StructType;

struct Type {
    enum class Kind {
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float32, Float64,
        Bool, Char, String, Void,
        Array,       // ArrayType:      elem + size
        DynArray,    // DynArrayType:   elem
        Tuple,       // TupleType:      elems
        Nullable,    // NullableType:   elem (внутренний тип)
        Struct,      // StructType:     name
        TypeParam,   // TypeParamType:  name (T, U, K, V ...)
        GenericInst, // GenericInstType: base + args (Stack<int32>)
    };

    Kind kind;

    bool is_signed_int()   const { return kind==Kind::Int8||kind==Kind::Int16||kind==Kind::Int32||kind==Kind::Int64; }
    bool is_unsigned_int() const { return kind==Kind::UInt8||kind==Kind::UInt16||kind==Kind::UInt32||kind==Kind::UInt64; }
    bool is_int()          const { return is_signed_int() || is_unsigned_int(); }
    bool is_float()        const { return kind==Kind::Float32 || kind==Kind::Float64; }
    bool is_numeric()      const { return is_int() || is_float(); }
    bool is_bool()         const { return kind==Kind::Bool; }
    bool is_char()         const { return kind==Kind::Char; }
    bool is_string()       const { return kind==Kind::String; }
    bool is_void()         const { return kind==Kind::Void; }
    bool is_array()        const { return kind==Kind::Array; }
    bool is_dynarray()     const { return kind==Kind::DynArray; }
    bool is_tuple()        const { return kind==Kind::Tuple; }
    bool is_nullable()     const { return kind==Kind::Nullable; }
    bool is_struct()       const { return kind==Kind::Struct || kind==Kind::GenericInst; }
    bool is_type_param()   const { return kind==Kind::TypeParam; }
    bool is_generic_inst() const { return kind==Kind::GenericInst; }

    int int_bits() const {
        switch (kind) {
        case Kind::Int8:  case Kind::UInt8:  return 8;
        case Kind::Int16: case Kind::UInt16: return 16;
        case Kind::Int32: case Kind::UInt32: return 32;
        case Kind::Int64: case Kind::UInt64: return 64;
        default: return 0;
        }
    }

    bool operator==(const Type& o) const;
    bool operator!=(const Type& o) const { return !(*this == o); }

    std::string to_string() const;

    static TypePtr make(Kind k);
    static TypePtr make_array(TypePtr elem, int64_t n);
    static TypePtr make_dynarray(TypePtr elem);
    static TypePtr make_tuple(std::vector<TypePtr> elems);
    static TypePtr make_nullable(TypePtr inner);
    static TypePtr make_struct(const std::string& name);
    static TypePtr make_type_param(const std::string& name);
    static TypePtr make_generic_inst(const std::string& base, std::vector<TypePtr> args);
};

struct ArrayType       : Type { TypePtr elem; int64_t size = 0; };
struct DynArrayType    : Type { TypePtr elem; };
struct TupleType       : Type { std::vector<TypePtr> elems; };
struct NullableType    : Type { TypePtr elem; };
struct StructType      : Type { std::string name; };
struct TypeParamType   : Type { std::string name; };
struct GenericInstType : Type { std::string base; std::vector<TypePtr> args; };

inline const TypePtr& type_elem(const Type& t) {
    switch (t.kind) {
    case Type::Kind::Array:    return static_cast<const ArrayType&>(t).elem;
    case Type::Kind::DynArray: return static_cast<const DynArrayType&>(t).elem;
    case Type::Kind::Nullable: return static_cast<const NullableType&>(t).elem;
    default: throw std::logic_error("type_elem: not an array/nullable type");
    }
}
inline TypePtr& type_elem(Type& t) {
    switch (t.kind) {
    case Type::Kind::Array:    return static_cast<ArrayType&>(t).elem;
    case Type::Kind::DynArray: return static_cast<DynArrayType&>(t).elem;
    case Type::Kind::Nullable: return static_cast<NullableType&>(t).elem;
    default: throw std::logic_error("type_elem: not an array/nullable type");
    }
}

inline const std::string& struct_name(const Type& t) {
    return static_cast<const StructType&>(t).name;
}
inline std::string sname(const Type& t) {
    if (t.kind == Type::Kind::GenericInst) return t.to_string();
    return static_cast<const StructType&>(t).name;
}
inline const std::vector<TypePtr>& tuple_elems(const Type& t) {
    return static_cast<const TupleType&>(t).elems;
}

inline bool Type::operator==(const Type& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
    case Kind::Array: {
        auto& a = static_cast<const ArrayType&>(*this);
        auto& b = static_cast<const ArrayType&>(o);
        return a.size == b.size && *a.elem == *b.elem;
    }
    case Kind::DynArray: {
        auto& a = static_cast<const DynArrayType&>(*this);
        auto& b = static_cast<const DynArrayType&>(o);
        return *a.elem == *b.elem;
    }
    case Kind::Tuple: {
        auto& a = static_cast<const TupleType&>(*this);
        auto& b = static_cast<const TupleType&>(o);
        if (a.elems.size() != b.elems.size()) return false;
        for (size_t i = 0; i < a.elems.size(); ++i)
            if (*a.elems[i] != *b.elems[i]) return false;
        return true;
    }
    case Kind::Nullable: {
        auto& a = static_cast<const NullableType&>(*this);
        auto& b = static_cast<const NullableType&>(o);
        return *a.elem == *b.elem;
    }
    case Kind::Struct:
        return static_cast<const StructType&>(*this).name ==
               static_cast<const StructType&>(o).name;
    case Kind::TypeParam:
        return static_cast<const TypeParamType&>(*this).name ==
               static_cast<const TypeParamType&>(o).name;
    case Kind::GenericInst: {
        auto& a = static_cast<const GenericInstType&>(*this);
        auto& b = static_cast<const GenericInstType&>(o);
        if (a.base != b.base || a.args.size() != b.args.size()) return false;
        for (size_t i = 0; i < a.args.size(); ++i)
            if (*a.args[i] != *b.args[i]) return false;
        return true;
    }
    default:
        return true;
    }
}

inline std::string Type::to_string() const {
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
    case Kind::Char:    return "char";
    case Kind::String:  return "string";
    case Kind::Void:    return "void";
    case Kind::Array: {
        auto& a = static_cast<const ArrayType&>(*this);
        return "[" + a.elem->to_string() + "; " + std::to_string(a.size) + "]";
    }
    case Kind::DynArray:
        return "[" + static_cast<const DynArrayType&>(*this).elem->to_string() + "]";
    case Kind::Tuple: {
        auto& t = static_cast<const TupleType&>(*this);
        std::string s = "(";
        for (size_t i = 0; i < t.elems.size(); ++i) {
            if (i) s += ", ";
            s += t.elems[i]->to_string();
        }
        return s + ")";
    }
    case Kind::Nullable:
        return static_cast<const NullableType&>(*this).elem->to_string() + "?";
    case Kind::Struct:
        return static_cast<const StructType&>(*this).name;
    case Kind::TypeParam:
        return static_cast<const TypeParamType&>(*this).name;
    case Kind::GenericInst: {
        auto& gi = static_cast<const GenericInstType&>(*this);
        std::string s = gi.base + "<";
        for (size_t i = 0; i < gi.args.size(); ++i) {
            if (i) s += ", ";
            s += gi.args[i]->to_string();
        }
        return s + ">";
    }
    }
    return "?";
}

inline TypePtr Type::make(Kind k) {
    auto t = std::make_shared<Type>(); t->kind = k; return t;
}
inline TypePtr Type::make_array(TypePtr elem, int64_t n) {
    auto t = std::make_shared<ArrayType>();
    t->kind = Kind::Array; t->elem = std::move(elem); t->size = n;
    return t;
}
inline TypePtr Type::make_dynarray(TypePtr elem) {
    auto t = std::make_shared<DynArrayType>();
    t->kind = Kind::DynArray; t->elem = std::move(elem);
    return t;
}
inline TypePtr Type::make_tuple(std::vector<TypePtr> elems) {
    auto t = std::make_shared<TupleType>();
    t->kind = Kind::Tuple; t->elems = std::move(elems);
    return t;
}
inline TypePtr Type::make_nullable(TypePtr inner) {
    auto t = std::make_shared<NullableType>();
    t->kind = Kind::Nullable; t->elem = std::move(inner);
    return t;
}
inline TypePtr Type::make_struct(const std::string& name) {
    auto t = std::make_shared<StructType>();
    t->kind = Kind::Struct; t->name = name;
    return t;
}
inline TypePtr Type::make_type_param(const std::string& name) {
    auto t = std::make_shared<TypeParamType>();
    t->kind = Kind::TypeParam; t->name = name;
    return t;
}
inline TypePtr Type::make_generic_inst(const std::string& base, std::vector<TypePtr> args) {
    auto t = std::make_shared<GenericInstType>();
    t->kind = Kind::GenericInst; t->base = base; t->args = std::move(args);
    return t;
}

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
inline TypePtr TYPE_CHAR    = Type::make(Type::Kind::Char);
inline TypePtr TYPE_STRING  = Type::make(Type::Kind::String);
inline TypePtr TYPE_VOID    = Type::make(Type::Kind::Void);
