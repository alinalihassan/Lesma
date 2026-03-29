import * as path from "path";
import * as vscode from "vscode";

/**
 * Expands VS Code-style path variables in settings values. The Configuration API returns
 * the literal string from settings.json (e.g. "${workspaceFolder}/build/Debug/lesma") and
 * does not substitute ${workspaceFolder} for extensions.
 */
export function resolveWorkspaceConfigPath(value: string): string {
  let result = value;
  const folders = vscode.workspace.workspaceFolders;
  if (!folders || folders.length === 0) {
    return result;
  }
  for (const folder of folders) {
    const token = "${workspaceFolder:" + folder.name + "}";
    result = result.split(token).join(folder.uri.fsPath);
  }
  result = result.replace(/\$\{workspaceFolder\}/g, folders[0].uri.fsPath);
  result = result.replace(
    /\$\{workspaceFolderBasename\}/g,
    path.basename(folders[0].uri.fsPath)
  );
  return result;
}
