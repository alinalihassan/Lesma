import * as child_process from "child_process";

type CommandResult = {
  error: child_process.ExecFileException | null;
  stdout: string;
  stderr: string;
  exitCode: number;
};

export default class ProcessManager {
  public static async startCommand(
    cmd: string,
    args: string[] = [],
    options: child_process.ExecFileOptions = {}
  ): Promise<CommandResult> {
    return new Promise((resolve) => {
      child_process.execFile(cmd, args, { ...options, encoding: "utf8" }, (error, stdout, stderr) => {
        resolve({
          error,
          stdout,
          stderr,
          exitCode: typeof error?.code === "number" ? error.code : 0,
        });
      });
    });
  }
}
