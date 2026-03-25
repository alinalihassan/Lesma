#!/usr/bin/env bun
import { Hono } from "hono"
import { loadConfig } from "./config"
import { createApiApp } from "./routes"
import { serveStatic } from "./static"
import { startLspSession } from "./lsp"

function parseAddr(addr: string): { hostname: string; port: number } {
  if (addr.startsWith(":")) {
    return { hostname: "0.0.0.0", port: Number.parseInt(addr.slice(1), 10) }
  }
  const lastColon = addr.lastIndexOf(":")
  if (lastColon <= 0 || lastColon === addr.length - 1) {
    throw new Error(`invalid listen address ${JSON.stringify(addr)}`)
  }
  const port = Number.parseInt(addr.slice(lastColon + 1), 10)
  if (!Number.isFinite(port)) {
    throw new Error(`invalid listen address ${JSON.stringify(addr)}`)
  }
  return { hostname: addr.slice(0, lastColon), port }
}

type WsData = {
  session: ReturnType<typeof startLspSession> | null
}

const cfg = loadConfig(process.argv.slice(2))
const { hostname, port } = parseAddr(cfg.addr)

const apiApp = createApiApp(cfg)
const root = new Hono()
root.route("/api", apiApp)

Bun.serve<WsData>({
  hostname,
  port,
  fetch(req, server) {
    const url = new URL(req.url)

    if (url.pathname === "/api/v2/lsp" && req.method === "GET") {
      const ok = server.upgrade(req, {
        data: { session: null },
      })
      if (ok) return
      return new Response("WebSocket upgrade failed", { status: 500 })
    }

    if (url.pathname.startsWith("/api")) {
      return root.fetch(req)
    }

    return serveStatic(cfg.assetsDir, req)
  },
  websocket: {
    open(ws) {
      try {
        const session = startLspSession(cfg.lesmaLspBin, (jsonUtf8) => {
          const text = new TextDecoder().decode(jsonUtf8)
          ws.send(text)
        })
        ws.data.session = session
      } catch (e) {
        console.error("lesma-lsp spawn failed", e)
        ws.close()
      }
    },
    message(ws, message) {
      const s = ws.data.session
      if (s === null) return
      const data =
        typeof message === "string"
          ? new TextEncoder().encode(message)
          : new Uint8Array(message)
      s.pushIncoming(data)
    },
    close(ws) {
      ws.data.session?.dispose()
      ws.data.session = null
    },
  },
})

console.error(`Lesma playground server listening on http://${hostname}:${port}`)
