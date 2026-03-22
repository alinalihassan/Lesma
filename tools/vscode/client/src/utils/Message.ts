import * as vscode from 'vscode';

export default class Message {

    public static error(...msg: string[]): void {
        vscode.window.showErrorMessage(msg.join(' '));
    }

    public static info(...msg: string[]): void {
        vscode.window.showInformationMessage(msg.join(' '));
    }

    public static warn(...msg: string[]): void {
        vscode.window.showWarningMessage(msg.join(' '));
    }

}