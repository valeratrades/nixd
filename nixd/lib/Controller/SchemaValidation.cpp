#include "SchemaValidation.h"

#include <nixf/Basic/Nodes/Attrs.h>
#include <nixf/Basic/Nodes/Lambda.h>

#include <llvm/Support/JSON.h>

#include <algorithm>
#include <map>

using namespace lspserver;
using namespace nixd;

namespace {

/// Nix source for a list of string literals. Option path segments come from
/// `optionAttrSetToDocList`, so they are attribute names, but they are spliced
/// back into an expression and get quoted accordingly.
std::string mkNixStringList(llvm::ArrayRef<std::string> Path) {
  std::string R = "[";
  for (const std::string &Seg : Path) {
    R += " \"";
    for (char C : Seg) {
      if (C == '"' || C == '\\')
        R += '\\';
      R += C;
    }
    R += '"';
  }
  return R + " ]";
}

/// The attribute set a module's values live in: the body of the `{ lib, ... }:`
/// form, or the document itself in the bare-attrset form.
const nixf::ExprAttrs *asAttrs(const nixf::Node *N) {
  while (N) {
    switch (N->kind()) {
    case nixf::Node::NK_ExprAttrs:
      return static_cast<const nixf::ExprAttrs *>(N);
    case nixf::Node::NK_ExprLambda:
      N = static_cast<const nixf::ExprLambda &>(*N).body();
      continue;
    default:
      return nullptr;
    }
  }
  return nullptr;
}

/// Descend \p Path, returning the key range of the deepest segment present.
std::optional<nixf::LexerCursorRange>
descend(const nixf::ExprAttrs &Attrs, llvm::ArrayRef<std::string> Path) {
  const nixf::ExprAttrs *Cur = &Attrs;
  std::optional<nixf::LexerCursorRange> Deepest;
  for (const std::string &Seg : Path) {
    const auto &Static = Cur->sema().staticAttrs();
    auto It = Static.find(Seg);
    if (It == Static.end())
      break;
    Deepest = It->second.key().range();
    Cur = asAttrs(It->second.value());
    if (!Cur)
      break;
  }
  return Deepest;
}

bool isPrefixOf(llvm::ArrayRef<std::string> A, llvm::ArrayRef<std::string> B) {
  return A.size() < B.size() && std::equal(A.begin(), A.end(), B.begin());
}

} // namespace

std::string nixd::mkModuleExpr(llvm::StringRef Nixpkgs,
                               llvm::StringRef ModulePath, PathRef Doc,
                               llvm::StringRef Src) {
  return ("let __lib = (" + Nixpkgs +
          ").lib; __m = __lib.evalModules { modules = [ " + ModulePath +
          " (__lib.modules.setDefaultModuleLocation \"" + Doc + "\" (" + Src +
          "\n)) ]; }; in ")
      .str();
}

std::string nixd::mkFailingPathsExpr(llvm::StringRef Base) {
  return (Base +
          "builtins.toJSON (builtins.filter (l: !(builtins.tryEval "
          "(builtins.deepSeq (__lib.getAttrFromPath l __m.config) null))"
          ".success) (builtins.filter (l: !(builtins.elem \"_module\" l)) "
          "(map (d: d.loc) (__lib.optionAttrSetToDocList __m.options))))")
      .str();
}

std::string nixd::mkForcePathExpr(llvm::StringRef Base,
                                  llvm::ArrayRef<std::string> Path) {
  return (Base + "builtins.deepSeq (__lib.getAttrFromPath " +
          mkNixStringList(Path) + " __m.config) \"\"")
      .str();
}

std::optional<std::vector<std::vector<std::string>>>
nixd::parseFailingPaths(llvm::StringRef JSON) {
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(JSON);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return std::nullopt;
  }
  const llvm::json::Array *Outer = Parsed->getAsArray();
  if (!Outer)
    return std::nullopt;

  std::vector<std::vector<std::string>> R;
  R.reserve(Outer->size());
  for (const llvm::json::Value &Elem : *Outer) {
    const llvm::json::Array *Inner = Elem.getAsArray();
    if (!Inner)
      return std::nullopt;
    std::vector<std::string> Path;
    Path.reserve(Inner->size());
    for (const llvm::json::Value &Seg : *Inner) {
      std::optional<llvm::StringRef> S = Seg.getAsString();
      if (!S)
        return std::nullopt;
      Path.emplace_back(S->str());
    }
    R.emplace_back(std::move(Path));
  }
  return R;
}

void nixd::dropCoveredParents(std::vector<std::vector<std::string>> &Paths) {
  std::vector<std::vector<std::string>> Kept;
  for (const auto &P : Paths) {
    const bool Covered = llvm::any_of(
        Paths, [&P](const auto &Other) { return isPrefixOf(P, Other); });
    if (!Covered)
      Kept.emplace_back(P);
  }
  Paths = std::move(Kept);
}

std::vector<OptionFailure>
nixd::groupByMessage(llvm::ArrayRef<OptionFailure> Failures) {
  // Insertion-ordered so the report follows the enumeration, which follows the
  // option declarations.
  std::vector<std::string> Order;
  std::map<std::string, std::vector<std::string>> Common;

  for (const OptionFailure &F : Failures) {
    auto It = Common.find(F.Message);
    if (It == Common.end()) {
      Common.emplace(F.Message, F.Path);
      Order.emplace_back(F.Message);
      continue;
    }
    std::vector<std::string> &Prefix = It->second;
    const size_t N = std::min(Prefix.size(), F.Path.size());
    size_t I = 0;
    while (I < N && Prefix[I] == F.Path[I])
      ++I;
    Prefix.resize(I);
  }

  std::vector<OptionFailure> R;
  R.reserve(Order.size());
  for (const std::string &Msg : Order)
    R.emplace_back(OptionFailure{Common.at(Msg), Msg});
  return R;
}

std::optional<nixf::LexerCursorRange>
nixd::rangeOfOptionPath(const nixf::Node &Root,
                        llvm::ArrayRef<std::string> Path) {
  const nixf::ExprAttrs *Attrs = asAttrs(&Root);
  if (!Attrs || Path.empty())
    return std::nullopt;

  if (auto R = descend(*Attrs, Path))
    return R;

  // The document may spell the same option behind `config`.
  std::vector<std::string> Prefixed{"config"};
  Prefixed.insert(Prefixed.end(), Path.begin(), Path.end());
  return descend(*Attrs, Prefixed);
}
