# Синтаксис языка BestLang

## 1. Общая структура программы

Программа состоит из набора верхнеуровневых объявлений.
На верхнем уровне разрешены только объявления.
Инструкции и выражения допускаются только внутри тел функций.

```
program        ::= top_level_decl* EOF
top_level_decl ::= fun_decl | struct_decl | impl_decl | type_alias | namespace_decl | var_decl ';'
```

Программа обязана содержать функцию `main` без параметров, возвращающую `int32`.

---

## 2. Объявление функций

```
fun_decl   ::= 'fun' IDENT '(' param_list? ')' ':' type block
param_list ::= param (',' param)*
param      ::= IDENT ':' type
```

Тип возврата обязателен. Для функций без возвращаемого значения используется `void`.

Примеры:

```
fun add(a: int32, b: int32): int32 {
    return a + b;
}

fun greet(name: string): void {
    print("Hello, " + name);
}
```

---

## 3. Объявления типов

### 3.1. Синонимы типов

```
type_alias ::= 'type' IDENT '=' type ';'
```

Синоним не создаёт новый тип, а вводит альтернативное имя для существующего.

Примеры:

```
type Meters = float64;
type Matrix4 = [float64; 16];
```

### 3.2. Структуры

```
struct_decl ::= 'struct' IDENT '{' field_decl* '}'
field_decl  ::= IDENT ':' type ';'
```

Пример:

```
struct Point {
    x: float64;
    y: float64;
}
```

---

## 3.2. Методы структур (impl)

```
impl_decl ::= 'impl' IDENT '{' fun_decl* '}'
```

Добавляет методы к структуре. Первый параметр метода — `self: ТипСтруктуры`.
Вложенные `impl` внутри `impl` не допускаются. Вызов: `объект.метод(аргументы)`.

Пример:

```
impl Point {
    fun length_sq(self: Point): float64 {
        return self.x * self.x + self.y * self.y;
    }
}
val p = Point { x: 3.0, y: 4.0 };
print(p.length_sq());   // 25.0
```

---

## 4. Пространства имён

```
namespace_decl ::= 'namespace' IDENT '{' top_level_decl* '}'
```

Вложенные пространства имён не поддерживаются.
Доступ к элементам через оператор `::`.

Пример:

```
namespace Math {
    val PI: float64 = 3.141592653589793;
}

var r: float64 = Math::PI;
```

---

## 5. Типы

```
type      ::= base_type | array_type | dynarray_type | tuple_type | nullable_type | IDENT

base_type ::= 'int8'  | 'int16'  | 'int32'  | 'int64'
            | 'uint8' | 'uint16' | 'uint32' | 'uint64'
            | 'float32' | 'float64'
            | 'bool' | 'string' | 'void'

array_type    ::= '[' type ';' INT_LIT ']'
dynarray_type ::= '[' type ']'
tuple_type    ::= '(' type (',' type)+ ')'
nullable_type ::= type '?'
```

Массив фиксированного размера: размер — часть типа. `[int32; 4]` и `[int32; 8]` — разные типы.
Динамический массив: размер определяется в рантайме, изменяется через `.push()` / `.pop()`.
Кортеж: фиксированный набор значений разных типов, доступ через `.0`, `.1`, ...
Nullable: тип `T?` означает «значение типа T или `null`».

---

## 6. Локальные связывания

```
var_decl ::= mut_kw IDENT (':' type)? '=' expr

mut_kw   ::= 'var'   (* мутабельная переменная *)
           | 'val'   (* иммутабельная переменная *)
```

Аннотация типа необязательна: если опущена, тип выводится из выражения справа.
Отсутствие одновременно аннотации и инициализирующего выражения — ошибка компиляции.

Примеры:

```
var count: int32 = 0;
val pi: float64 = 3.14159;
var name = "Alice";       // тип выведен как string
```

---

## 7. Выражения

```
expr         ::= or_expr ('as' type)?

or_expr      ::= and_expr ('or' and_expr)*
and_expr     ::= not_expr ('and' not_expr)*
not_expr     ::= 'not' not_expr | cmp_expr
cmp_expr     ::= add_expr (cmp_op add_expr)*
cmp_op       ::= '==' | '!=' | '<' | '>' | '<=' | '>='
add_expr     ::= mul_expr (('+' | '-') mul_expr)*
mul_expr     ::= unary_expr (('*' | '/' | '%') unary_expr)*
unary_expr   ::= '-' unary_expr | postfix_expr
postfix_expr ::= primary_expr postfix_op*
postfix_op   ::= '[' expr ']' | '.' IDENT | '.' INT_LIT | '(' arg_list? ')'
arg_list     ::= positional_arg (',' positional_arg)* (',' named_arg)*
positional_arg ::= expr
named_arg    ::= IDENT '=' expr

primary_expr ::= INT_LIT | FLOAT_LIT | BOOL_LIT | STRING_LIT | 'null'
               | ARRAY_LIT | STRUCT_LIT | TUPLE_LIT
               | IDENT | IDENT '::' IDENT
               | '(' expr ')'

ARRAY_LIT  ::= '[' (expr (',' expr)* ','?)? ']'
STRUCT_LIT ::= IDENT '{' (IDENT ':' expr (',' IDENT ':' expr)* ','?)? '}'
TUPLE_LIT  ::= '(' expr ',' expr (',' expr)* ')'
```

Операторы сравнения нецепочечные: `a < b < c` — синтаксическая ошибка.
Доступ к полю кортежа: `.0`, `.1`, ... (целый литерал после точки).
Именованные аргументы: `print(x, end="\n")` — только для встроенных функций.

---

## 8. Инструкции

```
block         ::= '{' statement* '}'

statement     ::= var_decl ';'
                | assign_stmt ';'
                | expr_stmt ';'
                | if_stmt
                | while_stmt
                | for_range_stmt
                | for_c_stmt
                | 'break' ';'
                | 'continue' ';'
                | return_stmt ';'
                | ';'

assign_stmt    ::= lvalue '=' expr
lvalue         ::= IDENT | lvalue '[' expr ']' | lvalue '.' IDENT

expr_stmt      ::= expr
if_stmt        ::= 'if' expr block ('else' (if_stmt | block))?
while_stmt     ::= 'while' expr block

for_range_stmt ::= 'for' IDENT 'in' expr '..' expr block
for_c_stmt     ::= 'for' var_decl ';' expr ';' (assign_stmt | expr_stmt) block

return_stmt    ::= 'return' expr?
```

Цикл `for range`: переменная проходит значения от `start` до `end-1` (конец не включается).
Цикл `for C-style`: аналог while с инициализацией и шагом в заголовке.

Единичная точка с запятой `;` — нулевая инструкция без эффекта.

---

## 9. Точка входа

Корректная программа обязана содержать объявление:

```
fun main(): int32 { ... }
```

Функция `main`:
- объявляется на верхнем уровне
- не принимает аргументов
- возвращает код завершения процесса
