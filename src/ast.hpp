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

struct Stmt {
    enum class Kind {
        VarDecl, Assign,
        ExprStmt,
        If, While, ForRange, ForC, Block,
        Break, Continue,
        Return,
        Empty
    };
    Kind   kind;
    SrcLoc loc;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt : Stmt { std::vector<StmtPtr> stmts; };

// Объявление переменной: var/val name: type = expr
struct VarDeclStmt : Stmt {
    std::string  name;
    TypePtr      ann_type; // аннотация типа (может быть nullptr — тогда выводится)
    ExprPtr      init;
    bool         is_val;   // true → val (иммутабельная)
};

// L-значение для присваивания
struct LValue {
    enum class Kind { Ident, Index, Field };
    Kind kind;
    SrcLoc loc;
    std::string name;               // для Ident
    std::unique_ptr<LValue> base;   // база для Index и Field
    ExprPtr idx;                    // индекс для Index
    std::string field;              // имя поля для Field
    int field_idx = -1;             // индекс поля (заполняется семантикой)
};
using LValuePtr = std::unique_ptr<LValue>;

struct AssignStmt : Stmt {
    LValuePtr target;
    ExprPtr   value;
};

struct ExprStmt : Stmt { ExprPtr expr; };

struct IfStmt : Stmt {
    ExprPtr  cond;
    StmtPtr  then_branch;
    StmtPtr  else_branch; // nullptr если нет else
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;
};

// for i in start..end { body }
struct ForRangeStmt : Stmt {
    std::string var_name;
    TypePtr     var_type;  // аннотация типа (обычно int32)
    ExprPtr     start;
    ExprPtr     end;
    StmtPtr     body;
};

// for init; cond; step { body }
struct ForCStmt : Stmt {
    StmtPtr  init;
    ExprPtr  cond;
    StmtPtr  step;
    StmtPtr  body;
};

struct BreakStmt    : Stmt {};
struct ContinueStmt : Stmt {};
struct EmptyStmt    : Stmt {};

struct ReturnStmt : Stmt {
    ExprPtr value; // nullptr для void-функций
};

// ── Объявления верхнего уровня 

struct TopDecl {
    enum class Kind { Fun, Struct, TypeAlias, Namespace, GlobalVar, Impl };
    Kind   kind;
    SrcLoc loc;
};
using TopDeclPtr = std::unique_ptr<TopDecl>;

struct Param {
    std::string name;
    TypePtr     type;
    SrcLoc      loc;
};

struct FunDecl : TopDecl {
    std::string        name;
    std::vector<Param> params;
    TypePtr            ret_type;
    std::unique_ptr<BlockStmt> body;
};

struct FieldDecl {
    std::string name;
    TypePtr     type;
    SrcLoc      loc;
};

struct StructDecl : TopDecl {
    std::string             name;
    std::vector<FieldDecl>  fields;
};

struct TypeAliasDecl : TopDecl {
    std::string name;
    TypePtr     aliased;
};

struct NamespaceDecl : TopDecl {
    std::string             name;
    std::vector<TopDeclPtr> members;
};

struct GlobalVarDecl : TopDecl {
    std::string name;
    TypePtr     ann_type;
    ExprPtr     init;
    bool        is_val;
};

// impl TypeName { fun method(self: TypeName, ...): R { ... } }
struct ImplDecl : TopDecl {
    std::string            type_name;
    std::vector<std::unique_ptr<FunDecl>> methods;
};

// Программа
struct Program {
    std::vector<TopDeclPtr> decls;
};
