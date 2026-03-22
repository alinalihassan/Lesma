import * as vscode from "vscode";

export default class OutputConsole {
  private static readonly channel: vscode.OutputChannel =
    vscode.window.createOutputChannel("Lesma");

  public static print(msg: string): void {
    this.channel.append(msg);
  }

  public static println(msg: string): void {
    this.channel.appendLine(msg);
  }

  public static clear(): void {
    this.channel.clear();
  }

  public static show(): void {
    this.channel.show(true);
  }
}
