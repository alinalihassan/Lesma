import * as fs from "fs";
import * as vscode from "vscode";
import which from "which";

import { resolveWorkspaceConfigPath } from "./configPath";

export default class SystemCommands {
  private static getLesmaLangConfiguration() {
    return vscode.workspace.getConfiguration("lesma");
  }

  /**
   * Set the extension-configuration of compilerPath.
   */
  public static async updateLesmaCommandPath(path: string) {
    const config = SystemCommands.getLesmaLangConfiguration();
    await config.update("compilerPath", path, true);
  }

  /**
   * Returns all possible file paths that may point to the Lesma compiler.
   * If an extension-configuration of compilerPath exists, then it is the first element
   * of the returned array.
   */
  public static async getAllPossibleLesmaCommandPaths(): Promise<string[]> {
    const compilerPaths: string[] = [];
    const config = SystemCommands.getLesmaLangConfiguration();

    // Prefer the extension-set compilerPath first, and then remaining possible paths
    // as fallback.
    const rawCompilerPath = config.get<string | null>("compilerPath");
    const exeChoice =
      typeof rawCompilerPath === "string"
        ? resolveWorkspaceConfigPath(rawCompilerPath)
        : null;
    if (exeChoice !== null) {
      compilerPaths.push(exeChoice);
    }

    const foundLesma = await which("lesma", {
      all: true,
      nothrow: true,
    });

    if (foundLesma !== null) {
      compilerPaths.push(...Array.from(foundLesma));
    }

    const paths = compilerPaths.filter(
      (x): x is string => typeof x === "string" && fs.existsSync(x)
    );

    if (paths.length > 0) {
      const configuredResolves =
        typeof rawCompilerPath === "string" &&
        fs.existsSync(resolveWorkspaceConfigPath(rawCompilerPath));
      // Do not replace a valid workspace-relative setting with an absolute path.
      if (rawCompilerPath === null || rawCompilerPath === "" || !configuredResolves) {
        void SystemCommands.updateLesmaCommandPath(paths[0]);
      }
    }

    return paths;
  }

  /**
   * Retrieves the extension-set compilerPath, if it exists.
   */
  public static async getLesmaCommandPath(): Promise<string | null> {
    const config = SystemCommands.getLesmaLangConfiguration();
    // Prefer an explicit Lesma compiler path when one is configured.
    const lesmaCompilerPath = config.get("compilerPath");
    if (typeof lesmaCompilerPath === "string") {
      const resolved = resolveWorkspaceConfigPath(lesmaCompilerPath);
      if (fs.existsSync(resolved)) {
        return resolved;
      }
    }
    return null;
  }
}
