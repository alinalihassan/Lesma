import * as path from "path";
import { workspace, ExtensionContext } from "vscode";
import * as vscode from "vscode";

import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from "vscode-languageclient/node";
import { getIndentDedentConfiguration } from "./configurations/indentDedentConfiguration";
import { checkForLesma, setLesmaCommands } from "./lesma/lesmaCommands";
import { resolveWorkspaceConfigPath } from "./utils/configPath";
import OutputConsole from "./utils/OutputConsole";

let client: LanguageClient;

function getLspServerCommand(): string {
  const config = workspace.getConfiguration("lesma");
  const compilerPath = config.get<string | null>("compilerPath");
  if (compilerPath) {
    const dir = path.dirname(resolveWorkspaceConfigPath(compilerPath));
    const exe = process.platform === "win32" ? "lesma-lsp.exe" : "lesma-lsp";
    return path.join(dir, exe);
  }
  return process.platform === "win32" ? "lesma-lsp.exe" : "lesma-lsp";
}

export function activate(context: ExtensionContext) {
  OutputConsole.clear();
  OutputConsole.println("Lesma extension activated");
  checkForLesma();
  setLesmaCommands(context);

  const lspCommand = getLspServerCommand();
  const serverOptions: ServerOptions = {
    command: lspCommand,
    args: [],
  };

  const lspOutputChannel = vscode.window.createOutputChannel("Lesma Language Server");
  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file", language: "lesma" },
      { scheme: "untitled", language: "lesma" },
    ],
    outputChannel: lspOutputChannel,
  };

  client = new LanguageClient("lesma", "Lesma", serverOptions, clientOptions);

  vscode.languages.setLanguageConfiguration(
    "lesma",
    getIndentDedentConfiguration()
  );

  context.subscriptions.push(client);
  OutputConsole.println("Lesma extension started");
  void client.start().then(() => {
    lspOutputChannel.appendLine(
      "lesma-lsp connected. Language features (diagnostics, hover, go to definition, completion) apply to **.les** files only."
    );
    lspOutputChannel.appendLine(
      "Use **Go to Definition** (F12) or **Go to Declaration** to jump to the local import binding when available. Set **lesma.compilerPath** to your `lesma` binary (same folder as `lesma-lsp`)."
    );
    // Notify the server about all Lesma documents already open (e.g. when the server
    // starts late after a rebuild). This ensures diagnostics, hover, etc. work in tabs
    // that were open before the LSP finished initializing.
    syncOpenLesmaDocumentsToServer(client);
  });
}

/** Send textDocument/didOpen for every open document that is a Lesma file.
 * Call this after the language client has started so the server gets already-open files. */
function syncOpenLesmaDocumentsToServer(client: LanguageClient): void {
  for (const doc of workspace.textDocuments) {
    if (doc.languageId !== "lesma") {
      continue;
    }
    client.sendNotification("textDocument/didOpen", {
      textDocument: {
        uri: doc.uri.toString(),
        languageId: doc.languageId,
        version: doc.version,
        text: doc.getText(),
      },
    });
  }
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) {
    return undefined;
  }
  return client.stop();
}
