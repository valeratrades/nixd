/// \file
/// \brief Type-checking a document against the module its `#:schema` names.
///
/// A `#:schema` directive says the document is a module, which is the one thing
/// nixd otherwise cannot know (see the FIXME in \p findAttrPathForOptions). So
/// only for such a document can its own text be evaluated as a module and its
/// values checked against the option declarations.
///
/// Failing options are enumerated structurally, never by reading nix's prose:
/// \p optionAttrSetToDocList lists every declared option, and \p tryEval over
/// each says which ones do not evaluate. A second, targeted force per surviving
/// path yields that path's own message through the error channel.

#pragma once

#include "lspserver/Path.h"

#include <nixf/Basic/Nodes/Basic.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

#include <optional>
#include <string>
#include <vector>

namespace nixd {

/// \brief One option path that failed to evaluate, and why.
struct OptionFailure {
  std::vector<std::string> Path;
  std::string Message;
};

/// \brief The `let` prelude binding the merged module, shared by both queries.
///
/// \p Src is spliced in verbatim, parenthesised. That is safe only because the
/// caller has already seen the document parse, and because the directive itself
/// is a `#` comment. \p Doc is what the module system reports as the definition
/// site, so errors name the document even though its text was never on disk.
std::string mkModuleExpr(llvm::StringRef Nixpkgs, llvm::StringRef ModulePath,
                         lspserver::PathRef Doc, llvm::StringRef Src);

/// \brief Expression evaluating to a JSON array of the failing option paths.
std::string mkFailingPathsExpr(llvm::StringRef Base);

/// \brief Expression forcing one option path, so its own error surfaces.
std::string mkForcePathExpr(llvm::StringRef Base,
                            llvm::ArrayRef<std::string> Path);

/// \brief Parse the reply of \p mkFailingPathsExpr.
std::optional<std::vector<std::vector<std::string>>>
parseFailingPaths(llvm::StringRef JSON);

/// \brief Drop paths that merely fail because a descendant does.
///
/// A parent option fails whenever a child does, so the enumeration reports
/// both; underlining the parent as well is noise.
void dropCoveredParents(std::vector<std::vector<std::string>> &Paths);

/// \brief Collapse failures sharing one message onto their common ancestor.
///
/// An undeclared key makes *every* sibling under the same submodule fail, all
/// with the identical "does not exist" message. Reporting each is N copies of
/// one mistake, so a group collapses to the deepest path covering it. Failures
/// with distinct messages are their own group, and so keep their own path.
std::vector<OptionFailure>
groupByMessage(llvm::ArrayRef<OptionFailure> Failures);

/// \brief The range of \p Path's name token within \p Root.
///
/// Descends the static attributes, stopping at the deepest segment present, so
/// a path naming an option the document never sets still lands on the nearest
/// ancestor it does set. \p Path is tried both bare and behind a leading
/// `config`, mirroring what \p findAttrPathForOptions strips on the way in.
std::optional<nixf::LexerCursorRange>
rangeOfOptionPath(const nixf::Node &Root, llvm::ArrayRef<std::string> Path);

} // namespace nixd
