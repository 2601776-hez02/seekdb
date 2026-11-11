// MiniVectorDB - recursive descent SQL parser.
#pragma once

#include "ast.h"
#include "lexer.h"

namespace mvdb {

class Parser {
 public:
  explicit Parser(const std::string& src) {
    src_ = src;
    size_t e = src_.find_last_not_of(" \t\r\n;");
    if (e != std::string::npos) src_ = src_.substr(0, e + 1);
    lx_ = Lexer(src_);
    Advance();  // tok_
    Advance();  // peek_
  }

  ParseResult Parse() {
    ParseResult r;
    if (lexError_) { r.err = Err::SyntaxError; return r; }
    if (tok_.t == Tok::End) { r.err = Err::SyntaxError; return r; }
    std::unique_ptr<Stmt> s(new Stmt());
    switch (tok_.t) {
      case Tok::KCreate: r.err = ParseCreate(s.get()); break;
      case Tok::KInsert: r.err = ParseInsert(s.get()); break;
      case Tok::KSelect: r.err = ParseSelect(s.get()); break;
      case Tok::KUpdate: r.err = ParseUpdate(s.get()); break;
      case Tok::KDelete: r.err = ParseDelete(s.get()); break;
      case Tok::KBegin: s->kind = Stmt::Kind::Begin; Advance(); r.err = ExpectEnd(); break;
      case Tok::KCommit: s->kind = Stmt::Kind::Commit; Advance(); r.err = ExpectEnd(); break;
      case Tok::KRollback: s->kind = Stmt::Kind::Rollback; Advance(); r.err = ExpectEnd(); break;
      case Tok::KAlter: r.err = Err::UnsupportedSQL; break;
      default: r.err = Err::UnsupportedSQL; break;
    }
    if (r.err == Err::OK) r.stmt = std::move(s);
    return r;
  }

 private:
  std::string src_;
  Lexer lx_;
  Token tok_, peek_;
  bool lexError_ = false;

  void Advance() {
    tok_ = peek_;
    if (!lx_.Next(peek_)) {
      lexError_ = true;
      peek_.t = Tok::End;
    }
  }

  bool At(Tok t) const { return tok_.t == t; }
  bool PeekAt(Tok t) const { return peek_.t == t; }
  bool Accept(Tok t) { if (At(t)) { Advance(); return true; } return false; }
  Err Expect(Tok t) { return Accept(t) ? Err::OK : Err::SyntaxError; }
  Err ExpectEnd() {
    if (lexError_) return Err::SyntaxError;
    while (tok_.t == Tok::Semi) Advance();   // tolerate trailing semicolons
    if (tok_.t != Tok::End) return Err::SyntaxError;  // one statement per request
    return Err::OK;
  }
  Err ExpectIdent(std::string& out) {
    if (tok_.t != Tok::Ident) return Err::SyntaxError;
    out = tok_.text;
    Advance();
    return Err::OK;
  }

  // CREATE ...
  Err ParseCreate(Stmt* s) {
    Advance();  // CREATE
    if (At(Tok::KTable)) { Advance(); return ParseCreateTable(s); }
    if (At(Tok::KVector)) { Advance(); return ParseCreateVecIndex(s); }
    return Err::UnsupportedSQL;  // CREATE INDEX / other DDL
  }

  Err ParseCreateTable(Stmt* s) {
    s->kind = Stmt::Kind::CreateTable;
    Err e = ExpectIdent(s->tbl);
    if (e != Err::OK) return e;
    if (!Accept(Tok::LParen)) return Err::SyntaxError;
    int pkCount = 0, vecCount = 0;
    if (At(Tok::RParen)) return Err::SyntaxError;
    while (true) {
      Column c;
      e = ExpectIdent(c.name);
      if (e != Err::OK) return e;
      if (At(Tok::KBigInt)) {
        c.type = ColType::BigInt;
        Advance();
        if (Accept(Tok::KUnsigned)) { /* accepted but ignored */ }
      } else if (At(Tok::KVarchar)) {
        c.type = ColType::VarChar;
        Advance();
        if (!Accept(Tok::LParen)) return Err::SyntaxError;
        if (tok_.t != Tok::IntLit) return Err::SyntaxError;
        int64_t n = 0;
        if (!parse_i64(tok_.text, n) || n < 1 || n > 1073741824) return Err::SyntaxError;
        c.n = n;
        Advance();
        if (!Accept(Tok::RParen)) return Err::SyntaxError;
      } else if (At(Tok::KVector)) {
        c.type = ColType::Vector;
        Advance();
        if (!Accept(Tok::LParen)) return Err::SyntaxError;
        if (tok_.t != Tok::IntLit) return Err::SyntaxError;
        int64_t d = 0;
        if (!parse_i64(tok_.text, d) || d < 1 || d > 1000000) return Err::SyntaxError;
        c.n = d;
        Advance();
        if (!Accept(Tok::RParen)) return Err::SyntaxError;
      } else {
        return Err::SyntaxError;
      }
      if (At(Tok::KPrimary)) {
        Advance();
        if (!Accept(Tok::KKey)) return Err::SyntaxError;
        if (c.type != ColType::BigInt) return Err::UnsupportedSQL;
        c.is_pk = true;
        pkCount++;
      }
      s->cols.push_back(std::move(c));
      if (Accept(Tok::Comma)) continue;
      if (Accept(Tok::RParen)) break;
      return Err::SyntaxError;
    }
    if (pkCount != 1) return Err::UnsupportedSQL;
    for (size_t i = 0; i < s->cols.size(); i++) {
      if (s->cols[i].is_pk) s->pk_col = int(i);
      if (s->cols[i].type == ColType::Vector) { vecCount++; s->vec_col = int(i); }
    }
    if (vecCount > 1) return Err::UnsupportedSQL;
    return ExpectEnd();
  }

  Err ParseCreateVecIndex(Stmt* s) {
    s->kind = Stmt::Kind::CreateVecIndex;
    if (!Accept(Tok::KIndex)) return Err::SyntaxError;
    Err e = ExpectIdent(s->idxName);
    if (e != Err::OK) return e;
    if (!Accept(Tok::KOn)) return Err::SyntaxError;
    e = ExpectIdent(s->tbl);
    if (e != Err::OK) return e;
    if (!Accept(Tok::LParen)) return Err::SyntaxError;
    e = ExpectIdent(s->vecColName);
    if (e != Err::OK) return e;
    if (!Accept(Tok::RParen)) return Err::SyntaxError;
    return ExpectEnd();
  }

  Err ParseInsert(Stmt* s) {
    s->kind = Stmt::Kind::Insert;
    Advance();  // INSERT
    if (!Accept(Tok::KInto)) return Err::SyntaxError;
    Err e = ExpectIdent(s->tbl);
    if (e != Err::OK) return e;
    if (Accept(Tok::LParen)) {
      while (true) {
        std::string n;
        e = ExpectIdent(n);
        if (e != Err::OK) return e;
        s->colNames.push_back(n);
        if (Accept(Tok::Comma)) continue;
        if (Accept(Tok::RParen)) break;
        return Err::SyntaxError;
      }
    }
    if (!Accept(Tok::KValues)) return Err::SyntaxError;
    while (true) {
      std::vector<Value> row;
      if (!Accept(Tok::LParen)) return Err::SyntaxError;
      while (true) {
        Value v;
        e = ParseLiteral(v);
        if (e != Err::OK) return e;
        row.push_back(std::move(v));
        if (Accept(Tok::Comma)) continue;
        if (Accept(Tok::RParen)) break;
        return Err::SyntaxError;
      }
      s->rows.push_back(std::move(row));
      if (Accept(Tok::Comma)) continue;
      break;
    }
    return ExpectEnd();
  }

  Err ParseLiteral(Value& v) {
    if (tok_.t == Tok::IntLit) {
      if (!parse_i64(tok_.text, v.i)) return Err::ValueOverflow;
      v.t = Value::T::BigInt;
      Advance();
      return Err::OK;
    }
    if (tok_.t == Tok::FloatLit) {
      if (!parse_f64(tok_.text, v.f)) return Err::ValueOverflow;
      v.t = Value::T::F64;
      Advance();
      return Err::OK;
    }
    if (tok_.t == Tok::StrLit) {
      v.t = Value::T::Str;
      v.s = tok_.text;
      Advance();
      return Err::OK;
    }
    return Err::SyntaxError;
  }

  // SELECT ... FROM t [WHERE p] [ORDER BY ...] [LIMIT n]
  Err ParseSelect(Stmt* s) {
    s->kind = Stmt::Kind::Select;
    Advance();  // SELECT
    while (true) {
      SelItem it;
      if (Accept(Tok::Star)) {
        it.star = true;
      } else if (At(Tok::Ident) && ieq(tok_.text, "L2_DISTANCE") && PeekAt(Tok::LParen)) {
        Advance();  // L2_DISTANCE
        Advance();  // '('
        if (tok_.t != Tok::Ident) return Err::SyntaxError;
        it.isDist = true;
        it.distCol = tok_.text;
        Advance();
        if (!Accept(Tok::Comma)) return Err::SyntaxError;
        if (tok_.t != Tok::StrLit) return Err::SyntaxError;
        it.distLit = tok_.text;
        Advance();
        if (!Accept(Tok::RParen)) return Err::SyntaxError;
        if (At(Tok::KAs)) {
          Advance();
          Err e = ExpectIdent(it.alias);
          if (e != Err::OK) return e;
        }
      } else if (tok_.t == Tok::Ident) {
        it.col = tok_.text;
        Advance();
        if (At(Tok::KAs)) {
          Advance();
          Err e = ExpectIdent(it.alias);
          if (e != Err::OK) return e;
        }
      } else {
        return Err::UnsupportedSQL;  // aggregates, subqueries, etc.
      }
      s->items.push_back(std::move(it));
      if (Accept(Tok::Comma)) continue;
      break;
    }
    if (!Accept(Tok::KFrom)) return Err::UnsupportedSQL;
    Err e = ExpectIdent(s->tbl);
    if (e != Err::OK) return e;
    if (At(Tok::KWhere)) {
      Advance();
      e = ParsePred(s->where);
      if (e != Err::OK) return e;
    }
    if (At(Tok::KOrder)) {
      Advance();
      if (!Accept(Tok::KBy)) return Err::SyntaxError;
      while (true) {
        OrderKey k;
        if (At(Tok::Ident) && ieq(tok_.text, "L2_DISTANCE") && PeekAt(Tok::LParen)) {
          Advance();
          Advance();
          if (tok_.t != Tok::Ident) return Err::SyntaxError;
          k.isDist = true;
          k.distCol = tok_.text;
          Advance();
          if (!Accept(Tok::Comma)) return Err::SyntaxError;
          if (tok_.t != Tok::StrLit) return Err::SyntaxError;
          k.distLit = tok_.text;
          Advance();
          if (!Accept(Tok::RParen)) return Err::SyntaxError;
        } else {
          if (tok_.t != Tok::Ident) return Err::SyntaxError;
          k.col = tok_.text;
          Advance();
        }
        if (At(Tok::KAsc)) { Advance(); }
        else if (At(Tok::KDesc)) { k.desc = true; Advance(); }
        s->order.push_back(std::move(k));
        if (Accept(Tok::Comma)) continue;
        break;
      }
    }
    if (At(Tok::KLimit)) {
      Advance();
      if (tok_.t != Tok::IntLit) return Err::SyntaxError;
      if (!parse_i64(tok_.text, s->limit) || s->limit < 0) return Err::SyntaxError;
      Advance();
    }
    return ExpectEnd();
  }

  // predicate grammar: or -> and -> atom
  Err ParsePred(std::unique_ptr<Expr>& out) {
    std::unique_ptr<Expr> left;
    Err e = ParseAnd(left);
    if (e != Err::OK) return e;
    while (At(Tok::KOr)) {
      Advance();
      auto node = std::unique_ptr<Expr>(new Expr());
      node->kind = Expr::Kind::Or;
      node->children.push_back(std::move(left));
      std::unique_ptr<Expr> right;
      e = ParseAnd(right);
      if (e != Err::OK) return e;
      node->children.push_back(std::move(right));
      left = std::move(node);
    }
    out = std::move(left);
    return Err::OK;
  }

  Err ParseAnd(std::unique_ptr<Expr>& out) {
    std::unique_ptr<Expr> left;
    Err e = ParseAtom(left);
    if (e != Err::OK) return e;
    while (At(Tok::KAnd)) {
      Advance();
      auto node = std::unique_ptr<Expr>(new Expr());
      node->kind = Expr::Kind::And;
      node->children.push_back(std::move(left));
      std::unique_ptr<Expr> right;
      e = ParseAtom(right);
      if (e != Err::OK) return e;
      node->children.push_back(std::move(right));
      left = std::move(node);
    }
    out = std::move(left);
    return Err::OK;
  }

  Err ParseAtom(std::unique_ptr<Expr>& out) {
    if (At(Tok::KNot)) {
      Advance();
      auto node = std::unique_ptr<Expr>(new Expr());
      node->kind = Expr::Kind::Not;
      std::unique_ptr<Expr> child;
      Err e = ParseAtom(child);
      if (e != Err::OK) return e;
      node->children.push_back(std::move(child));
      out = std::move(node);
      return Err::OK;
    }
    if (At(Tok::LParen)) {
      Advance();
      Err e = ParsePred(out);
      if (e != Err::OK) return e;
      if (!Accept(Tok::RParen)) return Err::SyntaxError;
      return Err::OK;
    }
    auto node = std::unique_ptr<Expr>(new Expr());
    node->kind = Expr::Kind::ColCmp;
    std::string first;
    bool firstIsCol = false;
    Value firstLit;
    Err e = TryOperand(first, firstIsCol, firstLit);
    if (e != Err::OK) return e;
    Op op;
    if (!TryOp(op)) return Err::SyntaxError;
    std::string second;
    bool secondIsCol = false;
    Value secondLit;
    e = TryOperand(second, secondIsCol, secondLit);
    if (e != Err::OK) return e;
    if (firstIsCol && !secondIsCol) {
      node->col = first;
      node->lit = secondLit;
      node->op = op;
    } else if (!firstIsCol && secondIsCol) {
      node->col = second;
      node->lit = firstLit;
      node->op = Flip(op);
    } else {
      return Err::SyntaxError;  // col-to-col or lit-to-lit
    }
    out = std::move(node);
    return Err::OK;
  }

  Err TryOperand(std::string& name, bool& isCol, Value& lit) {
    if (tok_.t == Tok::Ident) {
      name = tok_.text;
      isCol = true;
      Advance();
      return Err::OK;
    }
    if (tok_.t == Tok::IntLit || tok_.t == Tok::FloatLit || tok_.t == Tok::StrLit) {
      Err e = ParseLiteral(lit);
      if (e != Err::OK) return e;
      isCol = false;
      return Err::OK;
    }
    return Err::SyntaxError;
  }

  bool TryOp(Op& out) {
    switch (tok_.t) {
      case Tok::Eq: out = Op::Eq; Advance(); return true;
      case Tok::Lt: out = Op::Lt; Advance(); return true;
      case Tok::Le: out = Op::Le; Advance(); return true;
      case Tok::Gt: out = Op::Gt; Advance(); return true;
      case Tok::Ge: out = Op::Ge; Advance(); return true;
      case Tok::Ne: out = Op::Ne; Advance(); return true;
      default: return false;
    }
  }

  static Op Flip(Op o) {
    switch (o) {
      case Op::Lt: return Op::Gt;
      case Op::Le: return Op::Ge;
      case Op::Gt: return Op::Lt;
      case Op::Ge: return Op::Le;
      default: return o;
    }
  }

  Err ParseUpdate(Stmt* s) {
    s->kind = Stmt::Kind::Update;
    Advance();  // UPDATE
    Err e = ExpectIdent(s->tbl);
    if (e != Err::OK) return e;
    if (!Accept(Tok::KSet)) return Err::SyntaxError;
    while (true) {
      Assign a;
      e = ExpectIdent(a.col);
      if (e != Err::OK) return e;
      if (!Accept(Tok::Eq)) return Err::SyntaxError;
      e = ParseLiteral(a.val);
      if (e != Err::OK) return e;
      s->sets.push_back(std::move(a));
      if (Accept(Tok::Comma)) continue;
      break;
    }
    if (At(Tok::KWhere)) {
      Advance();
      e = ParsePred(s->where);
      if (e != Err::OK) return e;
    }
    return ExpectEnd();
  }

  Err ParseDelete(Stmt* s) {
    s->kind = Stmt::Kind::Delete;
    Advance();  // DELETE
    if (!Accept(Tok::KFrom)) return Err::SyntaxError;
    Err e = ExpectIdent(s->tbl);
    if (e != Err::OK) return e;
    if (At(Tok::KWhere)) {
      Advance();
      e = ParsePred(s->where);
      if (e != Err::OK) return e;
    }
    return ExpectEnd();
  }
};

}  // namespace mvdb
