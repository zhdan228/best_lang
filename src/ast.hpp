/*
 * Узлы абстрактного синтаксического дерева (AST) BestLang.
 *
 * Иерархия: Expr (выражения), Stmt (инструкции), TopDecl (объявления верхнего уровня).
 * Каждый узел хранит позицию в исходнике (SrcLoc) для диагностики.
 * После семантического анализа поле type у выражений заполняется выведенным типом.
 */

#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>

// Позиция в исходном файле (хранится в каждом узле AST)
struct SrcLoc {
    std::string file;
    uint32_t line = 0;
    uint32_t col  = 0;
};

// ── Выражения 

struct Expr {
    enum class Kind {
        IntLit, FloatLit, BoolLit, StringLit, NullLit,
        Ident, NamespaceAccess,
        BinOp, UnaryOp, Cast,
        Call, Index, Field, TupleIndex,
        ArrayLit, StructLit, TupleLit
    };
    Kind    kind;
    SrcLoc  loc;
    TypePtr type; // заполняется семантическим анализатором
};
using ExprPtr = std::unique_ptr<Expr>;

// Литералы
struct IntLitExpr   : Expr { int64_t value; std::string suffix; };
struct FloatLitExpr : Expr { double  value; std::string suffix; };
struct BoolLitExpr  : Expr { bool    value; };
struct StringLitExpr: Expr { std::string value; };
struct NullLitExpr  : Expr {}; // значение null

// Имена
struct IdentExpr : Expr { std::string name; };
struct NamespaceAccessExpr : Expr {
    std::string ns_name;
    std::string member;
};

// Бинарный оператор
enum class BinOpKind {
    Add, Sub, Mul, Div, Mod,
    And, Or,
    Eq, NEq, Lt, Le, Gt, Ge,
    StrConcat, // конкатенация строк через +
};
struct BinOpExpr : Expr {
    BinOpKind op;
    ExprPtr   lhs, rhs;
};

// Унарный оператор
enum class UnaryOpKind { Neg, Not };
struct UnaryOpExpr : Expr {
    UnaryOpKind op;
    ExprPtr     operand;
};

// Приведение типа: expr as T
struct CastExpr : Expr {
    ExprPtr  operand;
    TypePtr  target;
};

// Вызов функции; named_args — именованные аргументы вида name=expr (например end="" у print)
struct NamedArg {
    std::string name;
    ExprPtr     value;
};
struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    std::vector<NamedArg> named_args;
};

// Индексирование массива: arr[i]
struct IndexExpr : Expr {
    ExprPtr arr;
    ExprPtr idx;
};

// Доступ к полю структуры: obj.field
struct FieldExpr : Expr {
    ExprPtr     object;
    std::string field_name;
    int         field_idx = -1; // заполняется семантикой
};

// Доступ к элементу кортежа: t.0, t.1
struct TupleIndexExpr : Expr {
    ExprPtr object;
    int     index = 0;
};

// Составные литералы
struct ArrayLitExpr : Expr {
    std::vector<ExprPtr> elements;
};
struct StructLitField {
    std::string name;
    ExprPtr     value;
};
struct StructLitExpr : Expr {
    std::string type_name;
    std::vector<StructLitField> fields;
};
struct TupleLitExpr : Expr {
    std::vector<ExprPtr> elements;
};

// ── Инструкции 
