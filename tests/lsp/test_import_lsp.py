#!/usr/bin/env python3

import json
import subprocess
import sys
from pathlib import Path


class LspClient:
    def __init__(self, server_path: Path, root_path: Path):
        self.proc = subprocess.Popen(
            [str(server_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.root_path = root_path
        self.next_id = 1

    def close(self) -> None:
        try:
            self.notify("exit", {})
        finally:
            self.proc.terminate()
            self.proc.wait(timeout=5)

    def _write_message(self, payload: dict) -> None:
        data = json.dumps(payload).encode("utf-8")
        header = f"Content-Length: {len(data)}\r\n\r\n".encode("ascii")
        assert self.proc.stdin is not None
        self.proc.stdin.write(header + data)
        self.proc.stdin.flush()

    def _read_message(self) -> dict:
        assert self.proc.stdout is not None
        headers = {}
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("LSP server closed stdout unexpectedly")
            if line == b"\r\n":
                break
            key, value = line.decode("ascii").split(":", 1)
            headers[key.strip().lower()] = value.strip()
        length = int(headers["content-length"])
        body = self.proc.stdout.read(length)
        return json.loads(body.decode("utf-8"))

    def request(self, method: str, params: dict) -> dict:
        request_id = self.next_id
        self.next_id += 1
        self._write_message({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            message = self._read_message()
            if message.get("id") == request_id:
                if "error" in message:
                    raise AssertionError(f"{method} failed: {message['error']}")
                return message.get("result")

    def notify(self, method: str, params: dict) -> None:
        self._write_message({"jsonrpc": "2.0", "method": method, "params": params})


def file_uri(path: Path) -> str:
    return path.resolve().as_uri()


def position(line: int, character: int) -> dict:
    return {"line": line, "character": character}


def text_document(path: Path) -> dict:
    return {"uri": file_uri(path)}


def open_document(client: LspClient, path: Path) -> None:
    client.notify(
        "textDocument/didOpen",
        {
            "textDocument": {
                "uri": file_uri(path),
                "languageId": "lesma",
                "version": 1,
                "text": path.read_text(),
            }
        },
    )


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def as_locations(result: dict) -> list[dict]:
    if isinstance(result, list):
        return result
    if result is None:
        return []
    return [result]


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    fixtures = repo_root / "tests" / "lsp" / "fixtures"
    server_path = Path(sys.argv[1]) if len(sys.argv) > 1 else repo_root / "build" / "Debug" / "lesma-lsp"
    client = LspClient(server_path, repo_root)
    try:
        client.request(
            "initialize",
            {
                "processId": None,
                "rootUri": file_uri(repo_root),
                "capabilities": {},
            },
        )
        client.notify("initialized", {})

        alias_main = fixtures / "import_alias_main.les"
        module_main = fixtures / "import_module_alias_main.les"
        stdlib_main = fixtures / "import_stdlib_main.les"
        source_file = fixtures / "import_source.les"
        stdlib_file = Path.home() / ".lesma" / "stdlib" / "base.les"
        math_file = Path.home() / ".lesma" / "stdlib" / "math.les"

        for path in [source_file, alias_main, module_main, stdlib_main]:
            open_document(client, path)

        alias_hover = client.request(
            "textDocument/hover",
            {
                "textDocument": text_document(alias_main),
                "position": position(2, 13),
            },
        )
        assert_true("add" in alias_hover["contents"]["value"], "Hover for aliased import should resolve source function")

        alias_definition = as_locations(
            client.request(
                "textDocument/definition",
                {
                    "textDocument": text_document(alias_main),
                    "position": position(2, 13),
                },
            )
        )
        assert_true(alias_definition, "Definition for aliased import should exist")
        assert_true(
            alias_definition[0]["uri"] == file_uri(source_file),
            "Definition for aliased import should point to imported file",
        )

        alias_refs = as_locations(
            client.request(
                "textDocument/references",
                {
                    "textDocument": text_document(alias_main),
                    "position": position(2, 13),
                    "context": {"includeDeclaration": True},
                },
            )
        )
        alias_ref_uris = {loc["uri"] for loc in alias_refs}
        assert_true(file_uri(alias_main) in alias_ref_uris, "References should include alias file usages")
        assert_true(file_uri(source_file) in alias_ref_uris, "References should include imported declaration")
        assert_true(len(alias_refs) >= 3, "Aliased import references should include import line, use, and declaration")

        module_hover = client.request(
            "textDocument/hover",
            {
                "textDocument": text_document(module_main),
                "position": position(2, 14),
            },
        )
        assert_true("abs" in module_hover["contents"]["value"], "Hover for module alias member should resolve imported function")

        module_definition = as_locations(
            client.request(
                "textDocument/definition",
                {
                    "textDocument": text_document(module_main),
                    "position": position(2, 14),
                },
            )
        )
        assert_true(module_definition, "Definition for module alias member should exist")
        assert_true(
            module_definition[0]["uri"] == file_uri(math_file),
            "Definition for module alias member should point to the imported stdlib module",
        )

        stdlib_hover = client.request(
            "textDocument/hover",
            {
                "textDocument": text_document(stdlib_main),
                "position": position(0, 1),
            },
        )
        assert_true("print" in stdlib_hover["contents"]["value"], "Hover for stdlib symbol should resolve base.les function")

        stdlib_definition = as_locations(
            client.request(
                "textDocument/definition",
                {
                    "textDocument": text_document(stdlib_main),
                    "position": position(0, 1),
                },
            )
        )
        assert_true(stdlib_definition, "Definition for stdlib symbol should exist")
        assert_true(
            stdlib_definition[0]["uri"] == file_uri(stdlib_file),
            "Definition for stdlib symbol should point to ~/.lesma/stdlib/base.les",
        )
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
