// MiniVectorDB - AST definitions.
#pragma once

#include "mvdb.h"

namespace mvdb {

struct Expr;
struct Stmt;

// Predicate node: col <op> literal, or (pred AND pred), (pred OR pred), NOT pred
enum class Op : uint8_t { Eq, Lt, Le, Gt, Ge, Ne };

struct Expr {
  enum class Kind : uint8_t { ColCmp, And, Or, Not, Bool } kind = Kind::Bool;
  // ColCmp
  std::string col;
  Op op = Op::Eq;
  Value lit;
  bool litIsCol = false;  // (future) comparison between columns
  std::string litCol;
  // And/Or/Not
  std::vector<std::unique_ptr<Expr>> children;
};

// SELECT item
struct SelItem {
  bool star = false;
  std::string col;                       // plain column projection
  bool isDist = false;                   // L2_DISTANCE(col, '[...]')
  std::string distCol;
  std::string distLit;                   // query vector literal text
  std::string alias;                     // optional AS alias
};

struct OrderKey {
  std::string col;          // column name, or "" when isDist
  bool isDist = false;
  std::string distCol;
  std::string distLit;      // query vector literal text
  bool desc = false;
};

struct Assign {
  std::string col;
  Value val;
};

struct Stmt {
  enum class Kind : uint8_t {
    CreateTable, CreateVecIndex, Insert, Select, Update, Delete,
    Begin, Commit, Rollback
  } kind;

  // CreateTable
  std::string tbl;
  std::vector<Column> cols;
  int pk_col = -1;
  int vec_col = -1;

  // CreateVecIndex
  std::string idxName;
  std::string vecColName;

  // Insert
  std::vector<std::string> colNames;   // empty = all columns in order
  std::vector<std::vector<Value>> rows;

  // Select
  std::vector<SelItem> items;
  std::unique_ptr<Expr> where;
  std::vector<OrderKey> order;
  int64_t limit = -1;                   // -1 = no LIMIT

  // Update
  std::vector<Assign> sets;

  // Delete: uses tbl + where
};

struct ParseResult {
  std::unique_ptr<Stmt> stmt;
  Err err = Err::OK;
};

}  // namespace mvdb
