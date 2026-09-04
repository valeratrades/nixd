#include "SchemaDirective.h"

#include "nixd/Controller/Controller.h"
#include "nixd/Eval/Launch.h"

#include <nixf/Basic/Diagnostic.h>
#include <nixf/Parse/Parser.h>
#include <nixf/Sema/VariableLookup.h>

#include <boost/asio/post.hpp>

#include <mutex>

using namespace lspserver;
using namespace nixd;

void Controller::removeDocument(lspserver::PathRef File) {
  Store.removeDraft(File);
  {
    std::lock_guard _(TUsLock);
    TUs.erase(File);
  }
  releaseSchemaDirective(File);
  publishDiagnostics(File, std::nullopt, "", {});
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

    {
      std::lock_guard G(TUsLock);
      TUs.insert_or_assign(
          File, std::make_shared<NixTU>(std::move(Diagnostics), std::move(AST),
                                        std::nullopt, std::move(VLA), Src));
    }
    updateSchemaDirective(File, *Src);
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
