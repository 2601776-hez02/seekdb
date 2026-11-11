// MiniVectorDB - SQL lexer.
#pragma once

#include "mvdb.h"

namespace mvdb {

enum class Tok : uint8_t {
  End, Ident, IntLit, FloatLit, StrLit,
  LParen, RParen, Comma, Dot, Star, Semi, Eq, Lt, Le, Gt, Ge, Ne,
  // keywords
  KCreate, KTable, KPrimary, KKey, KVector, KIndex, KInsert, KInto, KValues,
  KSelect, KFrom, KWhere, KAnd, KOr, KOrder, KBy, KAsc, KDesc, KLimit,
  KUpdate, KSet, KDelete, KBegin, KCommit, KRollback, KOn, KAs, KAlter,
  KBigInt, KVarchar, KUnsigned, KNot,
};

struct Token {
  Tok t = Tok::End;
  std::string text;   // raw text for idents/literals
  int line = 0;
};

class Lexer {
 public:
  Lexer() : src_(nullptr), pos_(0) {}
  explicit Lexer(const std::string& src) : src_(&src), pos_(0) {}

  // Lex one token. Returns false on lexical error (error token consumed).
  bool Next(Token& tok) {
    // skip whitespace and comments
    while (pos_ < src_->size()) {
      char c = (*src_)[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pos_++; continue; }
      if (c == '-' && pos_ + 1 < src_->size() && (*src_)[pos_+1] == '-') {
        while (pos_ < src_->size() && (*src_)[pos_] != '\n') pos_++;
        continue;
      }
      break;
    }
    tok.text.clear();
    if (pos_ >= src_->size()) { tok.t = Tok::End; return true; }
    char c = (*src_)[pos_];
    auto emit = [&](Tok t, size_t len) { tok.t = t; tok.text = src_->substr(pos_, len); pos_ += len; return true; };
    switch (c) {
      case '(': return emit(Tok::LParen, 1);
      case ')': return emit(Tok::RParen, 1);
      case ',': return emit(Tok::Comma, 1);
      case '.': return emit(Tok::Dot, 1);
      case '*': return emit(Tok::Star, 1);
      case ';': return emit(Tok::Semi, 1);
      case '=': return emit(Tok::Eq, 1);
      case '<':
        if (pos_ + 1 < src_->size() && (*src_)[pos_+1] == '=') return emit(Tok::Le, 2);
        if (pos_ + 1 < src_->size() && (*src_)[pos_+1] == '>') return emit(Tok::Ne, 2);
        return emit(Tok::Lt, 1);
      case '>':
        if (pos_ + 1 < src_->size() && (*src_)[pos_+1] == '=') return emit(Tok::Ge, 2);
        return emit(Tok::Gt, 1);
      case '!':
        if (pos_ + 1 < src_->size() && (*src_)[pos_+1] == '=') return emit(Tok::Ne, 2);
        return false;
      case '\'': case '"': return LexString(tok, c);
      case '`': return LexIdent(tok, '`');
      default:
        if (isdigit((unsigned char)c) ||
            (c == '-' && pos_ + 1 < src_->size() && isdigit((unsigned char)(*src_)[pos_+1])) ||
            (c == '+' && pos_ + 1 < src_->size() && isdigit((unsigned char)(*src_)[pos_+1]))) {
          return LexNumber(tok);
        }
        if (isalpha((unsigned char)c) || c == '_') return LexIdent(tok, '\0');
        return false;
    }
  }

 private:
  bool LexString(Token& tok, char quote) {
    size_t i = pos_ + 1;
    std::string out;
    while (i < src_->size()) {
      char c = (*src_)[i];
      if (c == '\\' && i + 1 < src_->size()) {
        char e = (*src_)[i + 1];
        switch (e) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '0': out.push_back('\0'); break;
          case '\\': out.push_back('\\'); break;
          case '\'': out.push_back('\''); break;
          case '"': out.push_back('"'); break;
          default: out.push_back('\\'); out.push_back(e); break;
        }
        i += 2;
        continue;
      }
      if (c == quote) {
        if (i + 1 < src_->size() && (*src_)[i + 1] == quote) { out.push_back(quote); i += 2; continue; }
        tok.t = Tok::StrLit;
        tok.text = std::move(out);
        pos_ = i + 1;
        return true;
      }
      out.push_back(c);
      i++;
    }
    return false;  // unterminated
  }

  bool LexIdent(Token& tok, char quote) {
    size_t i = pos_ + (quote ? 1 : 0);
    size_t start = i;
    if (quote) {
      while (i < src_->size() && (*src_)[i] != quote) i++;
      if (i >= src_->size()) return false;
      tok.t = Tok::Ident;
      tok.text = src_->substr(start, i - start);
      pos_ = i + 1;
      return true;
    }
    while (i < src_->size() && (isalnum((unsigned char)(*src_)[i]) || (*src_)[i] == '_')) i++;
    tok.text = src_->substr(start, i - start);
    pos_ = i;
    std::string up = to_upper(tok.text);
    tok.t = Tok::Ident;
    if (up == "CREATE") tok.t = Tok::KCreate;
    else if (up == "TABLE") tok.t = Tok::KTable;
    else if (up == "PRIMARY") tok.t = Tok::KPrimary;
    else if (up == "KEY") tok.t = Tok::KKey;
    else if (up == "VECTOR") tok.t = Tok::KVector;
    else if (up == "INDEX") tok.t = Tok::KIndex;
    else if (up == "INSERT") tok.t = Tok::KInsert;
    else if (up == "INTO") tok.t = Tok::KInto;
    else if (up == "VALUES") tok.t = Tok::KValues;
    else if (up == "SELECT") tok.t = Tok::KSelect;
    else if (up == "FROM") tok.t = Tok::KFrom;
    else if (up == "WHERE") tok.t = Tok::KWhere;
    else if (up == "AND") tok.t = Tok::KAnd;
    else if (up == "OR") tok.t = Tok::KOr;
    else if (up == "ORDER") tok.t = Tok::KOrder;
    else if (up == "BY") tok.t = Tok::KBy;
    else if (up == "ASC") tok.t = Tok::KAsc;
    else if (up == "DESC") tok.t = Tok::KDesc;
    else if (up == "LIMIT") tok.t = Tok::KLimit;
    else if (up == "UPDATE") tok.t = Tok::KUpdate;
    else if (up == "SET") tok.t = Tok::KSet;
    else if (up == "DELETE") tok.t = Tok::KDelete;
    else if (up == "BEGIN") tok.t = Tok::KBegin;
    else if (up == "COMMIT") tok.t = Tok::KCommit;
    else if (up == "ROLLBACK") tok.t = Tok::KRollback;
    else if (up == "ON") tok.t = Tok::KOn;
    else if (up == "AS") tok.t = Tok::KAs;
    else if (up == "ALTER") tok.t = Tok::KAlter;
    else if (up == "BIGINT") tok.t = Tok::KBigInt;
    else if (up == "VARCHAR") tok.t = Tok::KVarchar;
    else if (up == "UNSIGNED") tok.t = Tok::KUnsigned;
    else if (up == "NOT") tok.t = Tok::KNot;
    return true;
  }

  bool LexNumber(Token& tok) {
    size_t i = pos_;
    if ((*src_)[i] == '+' || (*src_)[i] == '-') i++;
    bool isFloat = false;
    while (i < src_->size()) {
      char c = (*src_)[i];
      if (isdigit((unsigned char)c)) { i++; continue; }
      if (c == '.' && !isFloat) {
        // could be a float point; lookahead digit
        isFloat = true;
        i++;
        continue;
      }
      if ((c == 'e' || c == 'E') && i + 1 < src_->size() &&
          (isdigit((unsigned char)(*src_)[i+1]) ||
           (((*src_)[i+1] == '+' || (*src_)[i+1] == '-') && i + 2 < src_->size() &&
            isdigit((unsigned char)(*src_)[i+2])))) {
        isFloat = true;
        i++;
        if ((*src_)[i] == '+' || (*src_)[i] == '-') i++;
        continue;
      }
      break;
    }
    tok.text = src_->substr(pos_, i - pos_);
    tok.t = isFloat ? Tok::FloatLit : Tok::IntLit;
    pos_ = i;
    return true;
  }

  const std::string* src_;
  size_t pos_;
};

}  // namespace mvdb
