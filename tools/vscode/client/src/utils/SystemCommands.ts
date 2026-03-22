import * as fs from "fs";
import * as vscode from "vscode";
import which from "which";

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

    // Prefer the extension-set compilerPath first, and then remaining possible paths
    // as fallback.
    const exeChoice: string | null =
      SystemCommands.getLesmaLangConfiguration().get("compilerPath");
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
      // Set the first non-null compilerPath as the extension-configuration.
      SystemCommands.updateLesmaCommandPath(paths[0]);
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
    if (
      typeof lesmaCompilerPath === "string" &&
      fs.existsSync(lesmaCompilerPath)
    ) {
      return lesmaCompilerPath;
    }
    return null;
  }
}
