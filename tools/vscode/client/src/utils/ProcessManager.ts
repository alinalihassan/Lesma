import * as child_process from "child_process";

type CommandResult = {
  error: child_process.ExecException | null;
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
      child_process.execFile(cmd, args, options, (error, stdout, stderr) => {
        resolve({
          error,
          stdout,
          stderr,
          exitCode: error ? error.code : 0,
        });
      });
    });
  }
}
