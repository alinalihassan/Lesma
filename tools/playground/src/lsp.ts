import { Buffer } from "node:buffer"

function indexOfSubarray(hay: Buffer, needle: Uint8Array): number {
  outer: for (let i = 0; i <= hay.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) {
      if (hay[i + j] !== needle[j]) continue outer
    }
    return i
  }
  return -1
}

const headerEnd = new TextEncoder().encode("\r\n\r\n")

export function tryParseOneMessage(buf: Buffer): { message: Buffer; rest: Buffer } | null {
  const sep = indexOfSubarray(buf, headerEnd)
  if (sep === -1) return null

  const headerText = new TextDecoder("ascii").decode(buf.subarray(0, sep))
  let contentLength = 0
  for (const line of headerText.split(/\r\n/)) {
    const m = /^\s*content-length\s*:\s*(\d+)\s*$/i.exec(line)
    if (m) contentLength = Number.parseInt(m[1], 10)
  }
  if (!Number.isFinite(contentLength) || contentLength <= 0) {
    throw new Error("missing Content-Length")
  }

  const bodyStart = sep + headerEnd.length
  if (buf.length < bodyStart + contentLength) return null

  const message = buf.subarray(bodyStart, bodyStart + contentLength)
  const rest = buf.subarray(bodyStart + contentLength)
  return { message, rest }
}

export type LspSession = {
  pushIncoming: (data: Uint8Array) => void
  dispose: () => void
}

export function startLspSession(
  lesmaLspPath: string,
  onOutboundJson: (jsonUtf8: Uint8Array) => void,
): LspSession {
  const proc = Bun.spawn([lesmaLspPath], {
    stdin: "pipe",
    stdout: "pipe",
    stderr: "inherit",
    env: process.env as Record<string, string>,
  })

  if (!proc.stdin) {
    throw new Error("lesma-lsp: no stdin")
  }
  if (!proc.stdout) {
    throw new Error("lesma-lsp: no stdout")
  }

  let inboundBuf: Buffer = Buffer.alloc(0)
  let cancelled = false

  const pump = async () => {
    const reader = proc.stdout.getReader()
    try {
      while (!cancelled) {
        const { done, value } = await reader.read()
        if (done) break
        if (!value) continue
        inboundBuf = Buffer.concat([inboundBuf, Buffer.from(value)]) as Buffer
        for (;;) {
          try {
            const parsed = tryParseOneMessage(inboundBuf)
            if (parsed === null) break
            inboundBuf = parsed.rest as Buffer
            onOutboundJson(new Uint8Array(parsed.message))
          } catch {
            return
          }
        }
      }
    } catch {
      /* stream closed */
    }
  }
  void pump()

  const dispose = () => {
    cancelled = true
    try {
      proc.stdin?.end()
    } catch {
      /* ignore */
    }
    proc.kill()
  }

  return {
    pushIncoming(data: Uint8Array) {
      const header = new TextEncoder().encode(`Content-Length: ${data.byteLength}\r\n\r\n`)
      const stdin = proc.stdin
      if (!stdin) return
      void stdin.write(header)
      void stdin.write(data)
    },
    dispose,
  }
}
