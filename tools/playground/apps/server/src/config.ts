export type ServerConfig = {
  addr: string
  /** Unified docs + playground SPA (`tools/docs` Vite `build/client`). */
  assetsDir: string
  lesmaBin: string
  lesmaLspBin: string
  runTimeoutMs: number
}

function envDurationMs(key: string, fallbackMs: number): number {
  const v = Bun.env[key]?.trim()
  if (!v) return fallbackMs
  const m = v.match(/^(\d+)(ms|s|m)?$/i)
  if (!m) return fallbackMs
  const n = Number.parseInt(m[1], 10)
  if (!Number.isFinite(n) || n < 0) return fallbackMs
  const u = (m[2] ?? "s").toLowerCase()
  if (u === "ms") return n
  if (u === "m") return n * 60_000
  return n * 1000
}

function envString(key: string, fallback: string): string {
  const v = Bun.env[key]?.trim()
  return v !== undefined && v !== "" ? v : fallback
}

export function parseArgs(argv: string[]): Partial<Pick<ServerConfig, "addr" | "assetsDir">> {
  const out: Partial<Pick<ServerConfig, "addr" | "assetsDir">> = {}
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if ((a === "--addr" || a === "-addr") && argv[i + 1]) {
      out.addr = argv[++i]
    } else if (a.startsWith("--addr=") || a.startsWith("-addr=")) {
      const eq = a.indexOf("=")
      out.addr = a.slice(eq + 1)
    } else if ((a === "--static-dir" || a === "-static-dir") && argv[i + 1]) {
      out.assetsDir = argv[++i]
    } else if (a.startsWith("--static-dir=") || a.startsWith("-static-dir=")) {
      const eq = a.indexOf("=")
      out.assetsDir = a.slice(eq + 1)
    }
  }
  return out
}

export function loadConfig(argv: string[]): ServerConfig {
  const args = parseArgs(argv)
  const cwd = process.cwd()
  return {
    addr: args.addr ?? envString("LISTEN_ADDR", envString("APP_HTTP_ADDR", ":8080")),
    assetsDir: args.assetsDir ?? envString("APP_ASSETS_DIR", `${cwd}/public`),
    lesmaBin: envString("LESMA_BIN", "lesma"),
    lesmaLspBin: envString("LESMA_LSP_BIN", "lesma-lsp"),
    runTimeoutMs: envDurationMs("LESMA_RUN_TIMEOUT", 30_000),
  }
}
