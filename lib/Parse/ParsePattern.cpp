//===--- ParsePattern.cpp - Swift Language Parser for Patterns ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Pattern Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExprHandle.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

/// Parse function arguments.
///   func-arguments:
///     curried-arguments | selector-arguments
///   curried-arguments:
///     pattern-tuple+
///   selector-arguments:
///     '(' selector-element ')' (identifier '(' selector-element ')')+
///   selector-element:
///      identifier '(' pattern-atom (':' type-annotation)? ('=' expr)? ')'
static ParserStatus
parseCurriedFunctionArguments(Parser &P,
                              SmallVectorImpl<Pattern *> &argPat,
                              SmallVectorImpl<Pattern *> &bodyPat) {
  // parseFunctionArguments parsed the first argument pattern.
  // Parse additional curried argument clauses as long as we can.
  while (P.Tok.is(tok::l_paren)) {
    ParserResult<Pattern> pattern = P.parsePatternTuple(/*DefArgs=*/nullptr,
                                                        /*IsLet*/true);
    if (pattern.isNull() || pattern.hasCodeCompletion())
      return pattern;

    argPat.push_back(pattern.get());
    bodyPat.push_back(pattern.get());
  }
  return makeParserSuccess();
}

/// \brief Determine the kind of a default argument given a parsed
/// expression that has not yet been type-checked.
static DefaultArgumentKind getDefaultArgKind(ExprHandle *init) {
  if (!init || !init->getExpr())
    return DefaultArgumentKind::None;

  auto magic = dyn_cast<MagicIdentifierLiteralExpr>(init->getExpr());
  if (!magic)
    return DefaultArgumentKind::Normal;

  switch (magic->getKind()) {
  case MagicIdentifierLiteralExpr::Column:
    return DefaultArgumentKind::Column;
  case MagicIdentifierLiteralExpr::File:
    return DefaultArgumentKind::File;
  case MagicIdentifierLiteralExpr::Line:
    return DefaultArgumentKind::Line;
  }
}

static void recoverFromBadSelectorArgument(Parser &P) {
  while (P.Tok.isNot(tok::eof) && P.Tok.isNot(tok::r_paren) &&
         P.Tok.isNot(tok::l_brace) && P.Tok.isNot(tok::r_brace) &&
         !P.isStartOfStmt(P.Tok) &&
         !P.isStartOfDecl(P.Tok, P.peekToken())) {
    P.skipSingle();
  }
  P.consumeIf(tok::r_paren);
}

void Parser::DefaultArgumentInfo::setFunctionContext(DeclContext *DC) {
  assert(DC->isLocalContext());
  for (auto context : ParsedContexts) {
    context->changeFunction(DC);
  }
}

static ParserStatus parseDefaultArgument(Parser &P,
                                    Parser::DefaultArgumentInfo *defaultArgs,
                                         unsigned argIndex,
                                         ExprHandle *&init) {
  SourceLoc equalLoc = P.consumeToken(tok::equal);

  // Enter a fresh default-argument context with a meaningless parent.
  // We'll change the parent to the function later after we've created
  // that declaration.
  auto initDC =
    P.Context.createDefaultArgumentContext(P.CurDeclContext, argIndex);
  Parser::ParseFunctionBody initScope(P, initDC);

  ParserResult<Expr> initR =
    P.parseExpr(diag::expected_init_value);

  // Give back the default-argument context if we didn't need it.
  if (!initScope.hasClosures()) {
    P.Context.destroyDefaultArgumentContext(initDC);

  // Otherwise, record it if we're supposed to accept default
  // arguments here.
  } else if (defaultArgs) {
    defaultArgs->ParsedContexts.push_back(initDC);
  }

  if (!defaultArgs) {
    auto inFlight = P.diagnose(equalLoc, diag::non_func_decl_pattern_init);
    if (initR.isNonNull())
      inFlight.fixItRemove(SourceRange(equalLoc, initR.get()->getEndLoc()));
  }

  if (initR.hasCodeCompletion()) {
    recoverFromBadSelectorArgument(P);
    return makeParserCodeCompletionStatus();
  }
  if (initR.isNull()) {
    recoverFromBadSelectorArgument(P);
    return makeParserError();
  }

  init = ExprHandle::get(P.Context, initR.get());
  return ParserStatus();
}

/// Given a pattern "P" based on a pattern atom (either an identifer or _
/// pattern), rebuild and return the nested pattern around another root that
/// replaces the atom.
static Pattern *rebuildImplicitPatternAround(const Pattern *P, Pattern *NewRoot,
                                             ASTContext &C) {
  // We'll return a cloned copy of the pattern.
  Pattern *Result = P->clone(C, /*isImplicit*/true);

  class ReplaceRoot : public ASTWalker {
    Pattern *NewRoot;
  public:
    ReplaceRoot(Pattern *NewRoot) : NewRoot(NewRoot) {}

    // If we find a typed pattern, replace its subpattern with the NewRoot and
    // return.
    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      if (auto *TP = dyn_cast<TypedPattern>(P)) {
        TP->setSubPattern(NewRoot);
        return { false, TP };
      }
      
      return { true, P };
    }

    // If we get down to a named pattern "x" or any pattern "_", replace it
    // with our root.
    Pattern *walkToPatternPost(Pattern *P) override {
      if (isa<NamedPattern>(P) || isa<AnyPattern>(P))
        return NewRoot;
      return P;
    }
  };
  
  return Result->walk(ReplaceRoot(NewRoot));
}


static ParserStatus
parseSelectorArgument(Parser &P,
                      SmallVectorImpl<TuplePatternElt> &argElts,
                      SmallVectorImpl<TuplePatternElt> &bodyElts,
                      llvm::StringMap<VarDecl *> &selectorNames,
                      Parser::DefaultArgumentInfo &defaultArgs,
                      SourceLoc &rp) {
  ParserResult<Pattern> ArgPatternRes = P.parsePatternIdentifier(true);
  assert(ArgPatternRes.isNonNull() &&
         "selector argument did not start with an identifier!");
  Pattern *ArgPattern = ArgPatternRes.get();
  ArgPattern->setImplicit();
  
  // Check that a selector name isn't used multiple times, which would
  // lead to the function type having multiple arguments with the same name.
  if (NamedPattern *name = dyn_cast<NamedPattern>(ArgPattern)) {
    VarDecl *decl = name->getDecl();
    decl->setImplicit();
    StringRef id = decl->getName().str();
    auto prevName = selectorNames.find(id);
    if (prevName != selectorNames.end()) {
      P.diagnoseRedefinition(prevName->getValue(), decl);
    } else {
      selectorNames[id] = decl;
    }
  }

  if (!P.Tok.is(tok::l_paren)) {
    P.diagnose(P.Tok, diag::func_selector_without_paren);
    return makeParserError();
  }

  ParserResult<Pattern> PatternRes =
    P.parsePatternTuple(/*DefArgs=*/&defaultArgs, /*IsLet*/true);
  if (PatternRes.isNull()) {
    if (PatternRes.isParseError())
      recoverFromBadSelectorArgument(P);
    return PatternRes;
  }
  
  // The result of parsing a '(' pattern is either a ParenPattern or a
  // TuplePattern.
  if (auto *PP = dyn_cast<ParenPattern>(PatternRes.get())) {
    bodyElts.push_back(TuplePatternElt(PP->getSubPattern(), /*init*/nullptr,
                                       DefaultArgumentKind::None));
    // Return the ')' location.
    rp = PP->getRParenLoc();
  } else {
    auto *TP = cast<TuplePattern>(PatternRes.get());
    
    // Reject tuple patterns that aren't a single argument.
    if (TP->getNumFields() != 1 || TP->hasVararg()) {
      P.diagnose(TP->getLParenLoc(), diag::func_selector_with_not_one_argument);
      return makeParserError();
    }

    bodyElts.push_back(TP->getFields()[0]);

    // Return the ')' location.
    rp = TP->getRParenLoc();
  }
  

  TuplePatternElt &TPE = bodyElts.back();
  ArgPattern = rebuildImplicitPatternAround(TPE.getPattern(), ArgPattern,
                                            P.Context);
  
  argElts.push_back(TuplePatternElt(ArgPattern, TPE.getInit(),
                                    getDefaultArgKind(TPE.getInit())));
  return makeParserSuccess();
}

static Pattern *getFirstSelectorPattern(ASTContext &Context,
                                        const Pattern *argPattern,
                                        SourceLoc loc) {
  Pattern *any = new (Context) AnyPattern(loc, /*Implicit=*/true);
  return rebuildImplicitPatternAround(argPattern, any, Context);
}


static ParserStatus
parseSelectorFunctionArguments(Parser &P,
                               SmallVectorImpl<Pattern *> &ArgPatterns,
                               SmallVectorImpl<Pattern *> &BodyPatterns,
                               Parser::DefaultArgumentInfo &DefaultArgs,
                               Pattern *FirstPattern) {
  SourceLoc LParenLoc;
  SourceLoc RParenLoc;
  SmallVector<TuplePatternElt, 8> ArgElts;
  SmallVector<TuplePatternElt, 8> BodyElts;

  // For the argument pattern, try to convert the first parameter pattern to
  // an anonymous AnyPattern of the same type as the body parameter.
  if (ParenPattern *FirstParen = dyn_cast<ParenPattern>(FirstPattern)) {
    BodyElts.push_back(TuplePatternElt(FirstParen->getSubPattern()));
    LParenLoc = FirstParen->getLParenLoc();
    RParenLoc = FirstParen->getRParenLoc();
    ArgElts.push_back(TuplePatternElt(
        getFirstSelectorPattern(P.Context,
                                FirstParen->getSubPattern(),
                                FirstParen->getLoc())));
  } else if (TuplePattern *FirstTuple = dyn_cast<TuplePattern>(FirstPattern)) {
    LParenLoc = FirstTuple->getLParenLoc();
    RParenLoc = FirstTuple->getRParenLoc();
    if (FirstTuple->getNumFields() != 1) {
      P.diagnose(P.Tok, diag::func_selector_with_not_one_argument);
    }

    if (FirstTuple->getNumFields() >= 1) {
      const TuplePatternElt &FirstElt = FirstTuple->getFields()[0];
      BodyElts.push_back(FirstElt);
      ArgElts.push_back(TuplePatternElt(
          getFirstSelectorPattern(P.Context,
                                  FirstElt.getPattern(),
                                  FirstTuple->getLoc()),
        FirstElt.getInit(),
        FirstElt.getDefaultArgKind()));
    } else {
      // Recover by creating a '(_: ())' pattern.
      TuplePatternElt FirstElt(
          new (P.Context) TypedPattern(
              new (P.Context) AnyPattern(FirstTuple->getLParenLoc()),
              TupleTypeRepr::create(P.Context, {},
                                    FirstTuple->getSourceRange(),
                                    SourceLoc())));
      BodyElts.push_back(FirstElt);
      ArgElts.push_back(FirstElt);
    }
  } else
    llvm_unreachable("unexpected function argument pattern!");

  assert(ArgElts.size() > 0);
  assert(BodyElts.size() > 0);

  // Parse additional selectors as long as we can.
  llvm::StringMap<VarDecl *> SelectorNames;

  ParserStatus Status;
  for (;;) {
    if (P.isAtStartOfBindingName()) {
      Status |= parseSelectorArgument(P, ArgElts, BodyElts, SelectorNames,
                                      DefaultArgs, RParenLoc);
      continue;
    }
    if (P.Tok.is(tok::l_paren)) {
      P.diagnose(P.Tok, diag::func_selector_with_curry);
      // FIXME: better recovery: just parse a tuple instead of skipping tokens.
      P.skipUntilDeclRBrace(tok::l_brace);
      Status.setIsParseError();
    }
    break;
  }

  ArgPatterns.push_back(
      TuplePattern::create(P.Context, LParenLoc, ArgElts, RParenLoc,
                           /*hasVarArg=*/false,SourceLoc(), /*Implicit=*/true));
  BodyPatterns.push_back(
      TuplePattern::create(P.Context, LParenLoc, BodyElts, RParenLoc));
  return Status;
}

ParserStatus
Parser::parseFunctionArguments(SmallVectorImpl<Pattern *> &ArgPatterns,
                               SmallVectorImpl<Pattern *> &BodyPatterns,
                               DefaultArgumentInfo &DefaultArgs,
                               bool &HasSelectorStyleSignature) {
  // Parse the first function argument clause.
  ParserResult<Pattern> FirstPattern = parsePatternTuple(&DefaultArgs,
                                                         /*IsLet*/ true);
  if (FirstPattern.isNull()) {
    // Recover by creating a '()' pattern.
    auto EmptyTuplePattern =
        TuplePattern::create(Context, Tok.getLoc(), {}, Tok.getLoc());
    ArgPatterns.push_back(EmptyTuplePattern);
    BodyPatterns.push_back(EmptyTuplePattern);
  }

  // FIXME: more strict check would be to look for l_paren as well.
  if (isAtStartOfBindingName()) {
    // This looks like a selector-style argument.  Try to convert the first
    // argument pattern into a single argument type and parse subsequent
    // selector forms.
    HasSelectorStyleSignature = true;
    return ParserStatus(FirstPattern) |
           parseSelectorFunctionArguments(*this, ArgPatterns, BodyPatterns,
                                          DefaultArgs, FirstPattern.get());
  } else {
    ArgPatterns.push_back(FirstPattern.get());
    BodyPatterns.push_back(FirstPattern.get());
    return ParserStatus(FirstPattern) |
           parseCurriedFunctionArguments(*this, ArgPatterns, BodyPatterns);
  }
}

/// parseFunctionSignature - Parse a function definition signature.
///   func-signature:
///     func-arguments func-signature-result?
///   func-signature-result:
///     '->' type-annotation
///
/// Note that this leaves retType as null if unspecified.
ParserStatus
Parser::parseFunctionSignature(SmallVectorImpl<Pattern *> &argPatterns,
                               SmallVectorImpl<Pattern *> &bodyPatterns,
                               DefaultArgumentInfo &defaultArgs,
                               TypeRepr *&retType,
                               bool &HasSelectorStyleSignature) {
  HasSelectorStyleSignature = false;

  ParserStatus Status;
  // We force first type of a func declaration to be a tuple for consistency.
  if (Tok.is(tok::l_paren))
    Status = parseFunctionArguments(argPatterns, bodyPatterns, defaultArgs,
                                    HasSelectorStyleSignature);
  else {
    diagnose(Tok, diag::func_decl_without_paren);
    Status = makeParserError();

    // Recover by creating a '() -> ?' signature.
    auto *EmptyTuplePattern =
        TuplePattern::create(Context, Tok.getLoc(), {}, Tok.getLoc());
    argPatterns.push_back(EmptyTuplePattern);
    bodyPatterns.push_back(EmptyTuplePattern);
  }

  // If there's a trailing arrow, parse the rest as the result type.
  if (Tok.is(tok::arrow) || Tok.is(tok::colon)) {
    if (!consumeIf(tok::arrow)) {
      // FixIt ':' to '->'.
      diagnose(Tok, diag::func_decl_expected_arrow)
          .fixItReplace(SourceRange(Tok.getLoc()), "->");
      consumeToken(tok::colon);
    }

    ParserResult<TypeRepr> ResultType =
        parseTypeAnnotation(diag::expected_type_function_result);
    if (ResultType.hasCodeCompletion())
      return ResultType;
    retType = ResultType.getPtrOrNull();
    if (!retType) {
      Status.setIsParseError();
      return Status;
    }
  } else {
    // Otherwise, we leave retType null.
    retType = nullptr;
  }

  return Status;
}

ParserStatus
Parser::parseConstructorArguments(Pattern *&ArgPattern, Pattern *&BodyPattern,
                                  DefaultArgumentInfo &DefaultArgs,
                                  bool &HasSelectorStyleSignature) {
  HasSelectorStyleSignature = false;

  // It's just a pattern. Parse it.
  if (Tok.is(tok::l_paren)) {
    ParserResult<Pattern> Params = parsePatternTuple(&DefaultArgs,
                                                     /*IsLet*/ true);

    // If we failed to parse the pattern, create an empty tuple to recover.
    if (Params.isNull()) {
      Params = makeParserResult(Params,
          TuplePattern::createSimple(Context, Tok.getLoc(), {}, Tok.getLoc()));
    }

    ArgPattern = Params.get();
    BodyPattern = ArgPattern->clone(Context);
    return Params;
  }

  if (!isAtStartOfBindingName()) {
    // Complain that we expected '(' or a parameter name.
    {
      auto diag = diagnose(Tok, diag::expected_lparen_initializer);
      if (Tok.is(tok::l_brace))
        diag.fixItInsert(Tok.getLoc(), "() ");
    }

    // Create an empty tuple to recover.
    ArgPattern = TuplePattern::createSimple(Context, Tok.getLoc(), {},
                                            Tok.getLoc());
    BodyPattern = ArgPattern->clone(Context);
    return makeParserError();
  }

  // We have the start of a binding name, so this is a selector-style
  // declaration.
  HasSelectorStyleSignature = true;

  // This is not a parenthesis, but we should provide a reasonable source range
  // for parameters.
  SourceLoc LParenLoc = Tok.getLoc();

  // Parse additional selectors as long as we can.
  llvm::StringMap<VarDecl *> selectorNames;

  ParserStatus Status;
  SmallVector<TuplePatternElt, 4> ArgElts;
  SmallVector<TuplePatternElt, 4> BodyElts;
  SourceLoc RParenLoc;
  for (;;) {
    if (isAtStartOfBindingName()) {
      Status |= parseSelectorArgument(*this, ArgElts, BodyElts, selectorNames,
                                      DefaultArgs, RParenLoc);
      continue;
    }

    if (Tok.is(tok::l_paren)) {
      // FIXME: Should we assume this is '_'?
      diagnose(Tok, diag::func_selector_with_curry);
      // FIXME: better recovery: just parse a tuple instead of skipping tokens.
      skipUntilDeclRBrace(tok::l_brace);
      Status.setIsParseError();
    }
    break;
  }

  ArgPattern = TuplePattern::create(Context, LParenLoc, ArgElts, RParenLoc);
  BodyPattern = TuplePattern::create(Context, LParenLoc, BodyElts,
                                     RParenLoc);
  return Status;
}

/// Parse a pattern.
///   pattern ::= pattern-atom
///   pattern ::= pattern-atom ':' type-annotation
///   pattern ::= 'var' pattern
///   pattern ::= 'let' pattern
ParserResult<Pattern> Parser::parsePattern(bool isLet) {
  // If this is a let or var pattern parse it.
  if (Tok.is(tok::kw_let) || Tok.is(tok::kw_var))
    return parsePatternVarOrLet();
  
  // First, parse the pattern atom.
  ParserResult<Pattern> Result = parsePatternAtom(isLet);

  // Now parse an optional type annotation.
  if (consumeIf(tok::colon)) {
    if (Result.isNull()) {
      // Recover by creating AnyPattern.
      Result = makeParserErrorResult(new (Context) AnyPattern(PreviousLoc));
    }

    ParserResult<TypeRepr> Ty = parseTypeAnnotation();
    if (Ty.hasCodeCompletion())
      return makeParserCodeCompletionResult<Pattern>();

    if (Ty.isNull())
      Ty = makeParserResult(new (Context) ErrorTypeRepr(PreviousLoc));

    Result = makeParserResult(Result,
        new (Context) TypedPattern(Result.get(), Ty.get()));
  }

  return Result;
}

ParserResult<Pattern> Parser::parsePatternVarOrLet() {
  assert((Tok.is(tok::kw_let) || Tok.is(tok::kw_var)) && "expects let or var");
  bool isLet = Tok.is(tok::kw_let);
  SourceLoc varLoc = consumeToken();

  // 'var' and 'let' patterns shouldn't nest.
  if (InVarOrLetPattern)
    diagnose(varLoc, diag::var_pattern_in_var, unsigned(isLet));

  // In our recursive parse, remember that we're in a var/let pattern.
  llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
    T(InVarOrLetPattern, isLet ? IVOLP_InLet : IVOLP_InVar);

  ParserResult<Pattern> subPattern = parsePattern(isLet);
  if (subPattern.isNull())
    return nullptr;
  return makeParserResult(new (Context) VarPattern(varLoc, subPattern.get()));
}

/// \brief Determine whether this token can start a binding name, whether an
/// identifier or the special discard-value binding '_'.
bool Parser::isAtStartOfBindingName() {
  return Tok.is(tok::kw__)
    || (Tok.is(tok::identifier) && !isStartOfDecl(Tok, peekToken()));
}

Pattern *Parser::createBindingFromPattern(SourceLoc loc, Identifier name,
                                          bool isLet) {
  auto *var = new (Context) VarDecl(/*static*/ false, /*IsLet*/ isLet,
                                    loc, name, Type(), CurDeclContext);
  return new (Context) NamedPattern(var);
}

/// Parse an identifier as a pattern.
ParserResult<Pattern> Parser::parsePatternIdentifier(bool isLet) {
  SourceLoc loc = Tok.getLoc();
  if (consumeIf(tok::kw__)) {
    return makeParserResult(new (Context) AnyPattern(loc));
  }
  
  StringRef text = Tok.getText();
  if (consumeIf(tok::identifier)) {
    Identifier ident = Context.getIdentifier(text);
    return makeParserResult(createBindingFromPattern(loc, ident, isLet));
  }

  return nullptr;
}

/// Parse a pattern "atom", meaning the part that precedes the
/// optional type annotation.
///
///   pattern-atom ::= identifier
///   pattern-atom ::= '_'
///   pattern-atom ::= pattern-tuple
ParserResult<Pattern> Parser::parsePatternAtom(bool isLet) {
  switch (Tok.getKind()) {
  case tok::l_paren:
    return parsePatternTuple(/*DefaultArgs*/nullptr, isLet);

  case tok::identifier:
  case tok::kw__:
    return parsePatternIdentifier(isLet);

  case tok::code_complete:
    // Just eat the token and return an error status, *not* the code completion
    // status.  We can not code complete anything here -- we expect an
    // identifier.
    consumeToken(tok::code_complete);
    return nullptr;

  default:
    if (Tok.isKeyword() &&
        (peekToken().is(tok::colon) || peekToken().is(tok::equal))) {
      diagnose(Tok, diag::expected_pattern_is_keyword, Tok.getText());
      SourceLoc Loc = Tok.getLoc();
      consumeToken();
      return makeParserErrorResult(new (Context) AnyPattern(Loc));
    }
    diagnose(Tok, diag::expected_pattern);
    return nullptr;
  }
}

std::pair<ParserStatus, Optional<TuplePatternElt>>
Parser::parsePatternTupleElement(DefaultArgumentInfo *defaultArgs, bool isLet) {
  unsigned defaultArgIndex = (defaultArgs ? defaultArgs->NextIndex++ : 0);

  // Parse the pattern.
  ParserResult<Pattern> pattern = parsePattern(isLet);
  if (pattern.hasCodeCompletion())
    return std::make_pair(makeParserCodeCompletionStatus(), Nothing);

  if (pattern.isNull())
    return std::make_pair(makeParserError(), Nothing);

  // Parse the optional initializer.
  ExprHandle *init = nullptr;
  if (Tok.is(tok::equal)) {
    parseDefaultArgument(*this, defaultArgs, defaultArgIndex, init);
  }

  return std::make_pair(
      makeParserSuccess(),
      TuplePatternElt(pattern.get(), init, getDefaultArgKind(init)));
}

/// Parse a tuple pattern.
///
///   pattern-tuple:
///     '(' pattern-tuple-body? ')'
///   pattern-tuple-body:
///     pattern-tuple-element (',' pattern-tuple-body)*

ParserResult<Pattern> Parser::parsePatternTuple(DefaultArgumentInfo *defaults,
                                                bool isLet) {
  SourceLoc RPLoc, LPLoc = consumeToken(tok::l_paren);
  SourceLoc EllipsisLoc;

  // Parse all the elements.
  SmallVector<TuplePatternElt, 8> elts;
  ParserStatus ListStatus = parseList(tok::r_paren, LPLoc, RPLoc,
                                      tok::comma, /*OptionalSep=*/false,
                                      /*AllowSepAfterLast=*/false,
                                      diag::expected_rparen_tuple_pattern_list,
                                      [&] () -> ParserStatus {
    // Parse the pattern tuple element.
    ParserStatus EltStatus;
    Optional<TuplePatternElt> elt;
    std::tie(EltStatus, elt) = parsePatternTupleElement(defaults, isLet);
    if (EltStatus.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (!elt)
      return makeParserError();

    // Add this element to the list.
    elts.push_back(*elt);

    // If there is no ellipsis, we're done with the element.
    if (Tok.isNot(tok::ellipsis))
      return makeParserSuccess();
    SourceLoc ellLoc = consumeToken(tok::ellipsis);

    // An element cannot have both an initializer and an ellipsis.
    if (elt->getInit()) {
      diagnose(ellLoc, diag::tuple_ellipsis_init)
        .highlight(elt->getInit()->getExpr()->getSourceRange());
      // Return success since the error was semantic, and the caller should not
      // attempt recovery.
      return makeParserSuccess();
    }

    // An ellipsis element shall have a specified element type.
    // FIXME: This seems unnecessary.
    TypedPattern *typedPattern = dyn_cast<TypedPattern>(elt->getPattern());
    if (!typedPattern) {
      diagnose(ellLoc, diag::untyped_pattern_ellipsis)
        .highlight(elt->getPattern()->getSourceRange());
      // Return success so that the caller does not attempt recovery -- it
      // should have already happened when we were parsing the tuple element.
      return makeParserSuccess();
    }

    // Variadic elements must come last.
    // FIXME: Unnecessary restriction. It makes conversion more interesting,
    // but is not complicated to support.
    if (Tok.is(tok::r_paren)) {
      EllipsisLoc = ellLoc;
    } else {
      diagnose(ellLoc, diag::ellipsis_pattern_not_at_end);
    }

    return makeParserSuccess();
  });

  return makeParserResult(ListStatus, TuplePattern::createSimple(
                                          Context, LPLoc, elts, RPLoc,
                                          EllipsisLoc.isValid(), EllipsisLoc));
}

ParserResult<Pattern> Parser::parseMatchingPattern() {
  // TODO: Since we expect a pattern in this position, we should optimistically
  // parse pattern nodes for productions shared by pattern and expression
  // grammar. For short-term ease of initial implementation, we always go
  // through the expr parser for ambiguious productions.

  // Parse productions that can only be patterns.
  // matching-pattern ::= matching-pattern-var
  if (Tok.is(tok::kw_var) || Tok.is(tok::kw_let))
    return parseMatchingPatternVarOrLet();

  // matching-pattern ::= 'is' type
  if (Tok.is(tok::kw_is))
    return parseMatchingPatternIs();

  // matching-pattern ::= expr
  // Fall back to expression parsing for ambiguous forms. Name lookup will
  // disambiguate.
  ParserResult<Expr> subExpr = parseExpr(diag::expected_pattern);
  if (subExpr.hasCodeCompletion())
    return makeParserCodeCompletionStatus();
  if (subExpr.isNull())
    return nullptr;
  
  return makeParserResult(new (Context) ExprPattern(subExpr.get()));
}

ParserResult<Pattern> Parser::parseMatchingPatternVarOrLet() {
  assert((Tok.is(tok::kw_let) || Tok.is(tok::kw_var)) && "expects let or var");
  bool isLet = Tok.is(tok::kw_let);
  SourceLoc varLoc = consumeToken();

  // 'var' and 'let' patterns shouldn't nest.
  if (InVarOrLetPattern)
    diagnose(varLoc, diag::var_pattern_in_var, unsigned(isLet));

  // In our recursive parse, remember that we're in a var/let pattern.
  llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
    T(InVarOrLetPattern, isLet ? IVOLP_InLet : IVOLP_InVar);

  ParserResult<Pattern> subPattern = parseMatchingPattern();
  if (subPattern.isNull())
    return nullptr;
  return makeParserResult(new (Context) VarPattern(varLoc, subPattern.get()));
}

// matching-pattern ::= 'is' type
ParserResult<Pattern> Parser::parseMatchingPatternIs() {
  SourceLoc isLoc = consumeToken(tok::kw_is);
  ParserResult<TypeRepr> castType = parseType();
  if (castType.isNull() || castType.hasCodeCompletion())
    return nullptr;
  return makeParserResult(new (Context) IsaPattern(isLoc, castType.get()));
}

bool Parser::isOnlyStartOfMatchingPattern() {
  return Tok.is(tok::kw_var) || Tok.is(tok::kw_let) || Tok.is(tok::kw_is);
}
