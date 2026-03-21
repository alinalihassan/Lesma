# Lesma VS Code Extension

This directory contains the VS Code extension for Lesma inside the main
compiler repository.

## Features

- Syntax highlighting for `.les`
- Language configuration for auto-close and indent/dedent behavior
- Native LSP integration via `lesma-lsp`
- Commands for installing Lesma and running the active file

## Native language server

The extension launches `lesma-lsp` over stdio. Set `lesma.compilerPath` to your
`lesma` executable and the extension will resolve `lesma-lsp` from the same
directory.

Language features only apply to `.les` files. For C++ development in the
compiler itself, use `clangd` or another C++ extension.

## Building

Run all commands from `tools/vscode`:

- `npm install`
- `npm run compile`
- `npm run lint`

For local development in VS Code:

1. Open `tools/vscode`.
2. Start the `watch` task.
3. Launch the `Launch Client` debug configuration.
4. Open a `.les` file in the Extension Development Host.

## Troubleshooting

Open `Output -> Lesma Language Server` in VS Code. A healthy startup logs
`lesma-lsp connected` after the server launches.
