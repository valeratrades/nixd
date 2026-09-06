#include "Convert.h"
#include "SchemaDirective.h"
#include "SchemaValidation.h"

#include "nixd/CommandLine/Options.h"
#include "nixd/Controller/Controller.h"
#include "nixd/Eval/Launch.h"

#include <nixf/Basic/Diagnostic.h>
#include <nixf/Parse/Parser.h>
#include <nixf/Sema/VariableLookup.h>

#include <boost/asio/post.hpp>

#include <llvm/ADT/StringExtras.h>

#include <chrono>
#include <mutex>

using namespace lspserver;
using namespace nixd;

namespace {

/// Debouncing keeps a keystroke from launching an eval. A lit test drives the
/// server one message at a time, so there it would only add latency.
std::chrono::milliseconds debounceInterval() {
  return std::chrono::milliseconds(LitTest ? 0 : 300);
}

/// The message alone. \p llvm::toString would prefix the JSON-RPC error code,
/// which means nothing to someone reading a squiggle.
std::string errorMessage(llvm::Error E) {
  std::string R;
  llvm::handleAllErrors(
      std::move(E), [&R](const LSPError &LE) { R = LE.Message; },
      [&R](const llvm::ErrorInfoBase &EI) { R = EI.message(); });
  return R;
}

/// Accumulates the per-path messages of one validation run.
struct ForceRun {
  std::mutex Lock;
  std::vector<OptionFailure> Failures;
  std::size_t Pending;
};

Diagnostic mkWholeDocumentDiag(std::string Message) {
  return Diagnostic{
      .range = {{0, 0}, {0, 0}},
      .severity = 1,
      .code = "schema-eval",
      .source = "nixd-eval",
      .message = std::move(Message),
  };
}

std::vector<Diagnostic> mkEvalDiags(const nixf::Node &AST, std::string_view Src,
                                    llvm::ArrayRef<OptionFailure> Failures) {
  std::vector<Diagnostic> R;
  for (const OptionFailure &F : groupByMessage(Failures)) {
    std::optional<nixf::LexerCursorRange> Found =
        rangeOfOptionPath(AST, F.Path);
    // Nothing in the document to underline: an option it never mentions, or a
    // failure the paths do not agree on. Say so at the top rather than guess.
    R.emplace_back(Found ? Diagnostic{
                               .range = toLSPRange(Src, *Found),
                               .severity = 1,
                               .code = llvm::join(F.Path, "."),
                               .source = "nixd-eval",
                               .message = F.Message,
                           }
                         : mkWholeDocumentDiag(F.Message));
  }
  return R;
}

} // namespace

void Controller::removeDocument(lspserver::PathRef File) {
  Store.removeDraft(File);
  {
    std::lock_guard _(TUsLock);
    TUs.erase(File);
  }
  releaseSchemaDirective(File);
  {
    std::lock_guard _(EvalDiagsLock);
    EvalDiags.erase(File);
    EvalSerial.erase(File);
    ValidateTimers.erase(File);
  }
  publishDiagnostics(File, std::nullopt, "", {});
}

void Controller::commitEvalDiags(PathRef File,
                                 std::optional<std::int64_t> Version,
                                 std::int64_t Serial,
                                 std::vector<Diagnostic> Diags) {
  {
    std::lock_guard _(EvalDiagsLock);
    auto It = EvalSerial.find(File);
    // A newer edit has been scheduled; these describe a buffer that is gone.
    if (It == EvalSerial.end() || It->second != Serial)
      return;
    if (Diags.empty())
      EvalDiags.erase(File);
    else
      EvalDiags[File] = std::move(Diags);
  }
  std::shared_ptr<const NixTU> TU = getTU(File);
  if (!TU)
    return;
  publishDiagnostics(File, Version, TU->src(), TU->diagnostics());
}

void Controller::scheduleValidation(PathRef File,
                                    std::optional<std::int64_t> Version) {
  {
    std::lock_guard _(ConfigLock);
    if (!Config.schemaDirective.enable || !Config.schemaDirective.validate)
      return;
  }

  // Only a document naming its own module is known to be a module at all.
  bool HasSchema;
  {
    std::lock_guard _(DocOptionsLock);
    HasSchema = DocSchema.contains(File);
  }

  std::int64_t Serial;
  {
    std::lock_guard _(EvalDiagsLock);
    Serial = ++EvalSerial[File];
    if (!HasSchema) {
      ValidateTimers.erase(File);
      if (!EvalDiags.erase(File))
        return;
    }
  }
  // The directive is gone, and took its diagnostics with it.
  if (!HasSchema) {
    if (std::shared_ptr<const NixTU> TU = getTU(File))
      publishDiagnostics(File, Version, TU->src(), TU->diagnostics());
    return;
  }

  std::lock_guard _(EvalDiagsLock);
  auto &Timer = ValidateTimers[File];
  if (!Timer)
    Timer = std::make_unique<boost::asio::steady_timer>(Pool);
  Timer->expires_after(debounceInterval());
  Timer->async_wait([this, File = std::string(File), Version,
                     Serial](const boost::system::error_code &EC) {
    // Cancelled by a later edit, or the timer is going away.
    if (EC)
      return;
    validateDocument(File, Version, Serial);
  });
}

void Controller::validateDocument(PathRef File,
                                  std::optional<std::int64_t> Version,
                                  std::int64_t Serial) {
  std::string Nixpkgs;
  {
    std::lock_guard _(ConfigLock);
    Nixpkgs = NixpkgsExpr;
  }
  if (Nixpkgs.empty())
    return;

  std::string ModulePath;
  std::shared_ptr<AttrSetClientProc> Worker;
  {
    std::lock_guard _(DocOptionsLock);
    auto It = DocSchema.find(File);
    if (It == DocSchema.end())
      return;
    ModulePath = It->second;
    if (auto W = DocOptions.find(ModulePath); W != DocOptions.end())
      Worker = W->second;
  }
  AttrSetClient *Client = Worker ? Worker->client() : nullptr;
  if (!Client)
    return;

  std::optional<DraftStore::Draft> D = Store.getDraft(File);
  if (!D)
    return;

  const std::string Base =
      mkModuleExpr(Nixpkgs, ModulePath, File, *D->Contents);

  auto OnPaths = [this, File = std::string(File), Version, Serial, Base, Worker,
                  Client](llvm::Expected<EvalStringResponse> Resp) {
    if (!Resp) {
      // The merged module did not evaluate at all, so no option path is
      // implicated and there is nothing in particular to underline.
      commitEvalDiags(File, Version, Serial,
                      {mkWholeDocumentDiag(errorMessage(Resp.takeError()))});
      return;
    }
    std::optional<std::vector<std::vector<std::string>>> Paths =
        parseFailingPaths(*Resp);
    if (!Paths) {
      elog("schema-validate: {0}: malformed failing-path list {1}", File,
           *Resp);
      return;
    }
    dropCoveredParents(*Paths);
    if (Paths->empty()) {
      commitEvalDiags(File, Version, Serial, {});
      return;
    }

    auto Run = std::make_shared<ForceRun>();
    Run->Pending = Paths->size();
    for (const std::vector<std::string> &P : *Paths) {
      auto OnForced = [this, File, Version, Serial, Run,
                       P](llvm::Expected<EvalStringResponse> Forced) {
        std::vector<OptionFailure> Done;
        {
          std::lock_guard _(Run->Lock);
          // A path that forces cleanly on the second look contributes nothing;
          // only the message matters, and success carries none.
          if (!Forced)
            Run->Failures.emplace_back(
                OptionFailure{P, errorMessage(Forced.takeError())});
          if (--Run->Pending)
            return;
          Done = std::move(Run->Failures);
        }
        std::shared_ptr<const NixTU> TU = getTU(File);
        std::shared_ptr<nixf::Node> AST = TU ? TU->ast() : nullptr;
        if (!AST)
          return;
        commitEvalDiags(File, Version, Serial,
                        mkEvalDiags(*AST, TU->src(), Done));
      };
      Client->evalString(mkForcePathExpr(Base, P), std::move(OnForced));
    }
  };
  Client->evalString(mkFailingPathsExpr(Base), std::move(OnPaths));
}

Controller::OptionProviders Controller::optionProviders(PathRef File) {
  {
    std::lock_guard _(DocOptionsLock);
    if (auto It = DocSchema.find(File); It != DocSchema.end()) {
      if (auto Worker = DocOptions.find(It->second); Worker != DocOptions.end())
        return {*Worker};
    }
  }
  std::lock_guard _(OptionsLock);
  return {Options.begin(), Options.end()};
}

void Controller::releaseSchemaDirective(PathRef File) {
  std::lock_guard _(DocOptionsLock);
  if (!DocSchema.erase(File))
    return;
  for (auto It = DocOptions.begin(); It != DocOptions.end();) {
    const bool Referenced = llvm::any_of(DocSchema, [&It](const auto &Entry) {
      return Entry.second == It->first;
    });
    It = Referenced ? std::next(It) : DocOptions.erase(It);
  }
}

void Controller::updateSchemaDirective(PathRef File, std::string_view Src) {
  std::string Nixpkgs;
  {
    std::lock_guard _(ConfigLock);
    if (!Config.schemaDirective.enable)
      return;
    Nixpkgs = NixpkgsExpr;
  }
  if (Nixpkgs.empty())
    return;

  std::optional<std::string> Resolved;
  if (auto Directive = parseSchemaDirective(Src))
    Resolved = resolveSchemaPath(*Directive, File, WorkspaceRoot);

  std::shared_ptr<AttrSetClientProc> Launched;
  {
    std::lock_guard _(DocOptionsLock);
    auto It = DocSchema.find(File);
    const llvm::StringRef Old = It == DocSchema.end() ? "" : It->second;
    if (Old == Resolved.value_or(""))
      return;

    if (!Resolved) {
      DocSchema.erase(File);
    } else {
      DocSchema[File] = *Resolved;
      auto &Worker = DocOptions[*Resolved];
      if (!Worker) {
        Worker = startOption(*Resolved);
        Launched = Worker;
      }
    }
  }
  if (!Launched)
    return;

  if (AttrSetClient *Client = Launched->client()) {
    evalExprWithProgress(*Client,
                         "((" + Nixpkgs + ").lib.evalModules { modules = [ " +
                             *Resolved + " ]; }).options",
                         *Resolved);
  }
}

void Controller::actOnDocumentAdd(PathRef File,
                                  std::optional<int64_t> Version) {
  auto Action = [this, File = std::string(File), Version]() {
    auto Draft = Store.getDraft(File);
    std::shared_ptr<const std::string> Src = Draft->Contents;
    assert(Draft && "Added document is not in the store?");

    std::vector<nixf::Diagnostic> Diagnostics;
    std::shared_ptr<nixf::Node> AST =
        nixf::parse(*Draft->Contents, Diagnostics);

    if (!AST) {
      {
        std::lock_guard G(TUsLock);
        publishDiagnostics(File, Version, *Src, Diagnostics);
        TUs.insert_or_assign(
            File, std::make_shared<NixTU>(std::move(Diagnostics),
                                          std::move(AST), std::nullopt,
                                          /*VLA=*/nullptr, Src));
      }
      // The directive is a first-line comment, still meaningful in a file that
      // does not parse yet.
      updateSchemaDirective(File, *Src);
      return;
    }

    auto VLA = std::make_unique<nixf::VariableLookupAnalysis>(Diagnostics);
    VLA->runOnAST(*AST, llvm::sys::path::filename(File) == "flake.nix");

    publishDiagnostics(File, Version, *Src, Diagnostics);

    // A buffer that does not parse cannot be spliced into a module expression.
    const bool Parsed = llvm::none_of(Diagnostics, [](const auto &D) {
      return getLSPSeverity(D.kind()) == 1;
    });

    {
      std::lock_guard G(TUsLock);
      TUs.insert_or_assign(
          File, std::make_shared<NixTU>(std::move(Diagnostics), std::move(AST),
                                        std::nullopt, std::move(VLA), Src));
    }
    updateSchemaDirective(File, *Src);
    if (Parsed)
      scheduleValidation(File, Version);
  };
  Action();
}

void Controller::createWorkDoneProgress(
    const lspserver::WorkDoneProgressCreateParams &Params) {
  if (ClientCaps.WorkDoneProgress)
    CreateWorkDoneProgress(Params, [](llvm::Expected<std::nullptr_t> Reply) {
      if (!Reply)
        elog("create workdone progress error: {0}", Reply.takeError());
    });
}

Controller::Controller(std::unique_ptr<lspserver::InboundPort> In,
                       std::unique_ptr<lspserver::OutboundPort> Out)
    : LSPServer(std::move(In), std::move(Out)) {

  // Life Cycle
  Registry.addMethod("initialize", this, &Controller::onInitialize);
  Registry.addNotification("initialized", this, &Controller::onInitialized);

  Registry.addMethod("shutdown", this, &Controller::onShutdown);

  // Text Document Synchronization
  Registry.addNotification("textDocument/didOpen", this,
                           &Controller::onDocumentDidOpen);
  Registry.addNotification("textDocument/didChange", this,
                           &Controller::onDocumentDidChange);

  Registry.addNotification("textDocument/didClose", this,
                           &Controller::onDocumentDidClose);

  // Language Features
  Registry.addMethod("textDocument/definition", this,
                     &Controller::onDefinition);
  Registry.addMethod("textDocument/documentSymbol", this,
                     &Controller::onDocumentSymbol);
  Registry.addMethod("textDocument/foldingRange", this,
                     &Controller::onFoldingRange);
  Registry.addMethod("textDocument/semanticTokens/full", this,
                     &Controller::onSemanticTokens);
  Registry.addMethod("textDocument/inlayHint", this, &Controller::onInlayHint);
  Registry.addMethod("textDocument/completion", this,
                     &Controller::onCompletion);
  Registry.addMethod("completionItem/resolve", this,
                     &Controller::onCompletionItemResolve);
  Registry.addMethod("textDocument/references", this,
                     &Controller::onReferences);
  Registry.addMethod("textDocument/documentHighlight", this,
                     &Controller::onDocumentHighlight);
  Registry.addMethod("textDocument/documentLink", this,
                     &Controller::onDocumentLink);
  Registry.addMethod("textDocument/codeAction", this,
                     &Controller::onCodeAction);
  Registry.addMethod("codeAction/resolve", this,
                     &Controller::onCodeActionResolve);
  Registry.addMethod("textDocument/hover", this, &Controller::onHover);
  Registry.addMethod("textDocument/formatting", this, &Controller::onFormat);
  Registry.addMethod("textDocument/rename", this, &Controller::onRename);
  Registry.addMethod("textDocument/prepareRename", this,
                     &Controller::onPrepareRename);

  // Workspace features
  Registry.addNotification("workspace/didChangeConfiguration", this,
                           &Controller::onDidChangeConfiguration);

  WorkspaceConfiguration = mkOutMethod<ConfigurationParams, llvm::json::Value>(
      "workspace/configuration");

  PublishDiagnostic = mkOutNotifiction<PublishDiagnosticsParams>(
      "textDocument/publishDiagnostics");
  CreateWorkDoneProgress =
      mkOutMethod<WorkDoneProgressCreateParams, std::nullptr_t>(
          "window/workDoneProgress/create");
  ShowDocument = mkOutMethod<ShowDocumentParams, ShowDocumentResult>(
      "window/showDocument");
  BeginWorkDoneProgress =
      mkOutNotifiction<ProgressParams<WorkDoneProgressBegin>>("$/progress");
  ReportWorkDoneProgress =
      mkOutNotifiction<ProgressParams<WorkDoneProgressReport>>("$/progress");
  EndWorkDoneProgress =
      mkOutNotifiction<ProgressParams<WorkDoneProgressEnd>>("$/progress");
}
