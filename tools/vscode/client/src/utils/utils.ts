import * as vscode from "vscode";
import * as path from "path";

export function getActiveFilename() {
  const filename = vscode.window.activeTextEditor?.document?.fileName;

  if (!filename) {
    vscode.window.showErrorMessage("Could not locate target file.");
    return;
  }

  const ext = path.extname(filename);
  if (ext !== ".les") {
    vscode.window.showErrorMessage("Target file must be a Lesma (.les) file.");
    return;
  }

  return filename;
}
