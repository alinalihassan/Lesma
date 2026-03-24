import { Hono } from "hono"
import type { ServerConfig } from "./config"
import { runLesma } from "./lesma"
import { PayloadError, readJsonBody, validatePayload } from "./payload"

export function createApiApp(cfg: ServerConfig): Hono {
  const app = new Hono()

  app.get("/version", (c) => c.json({ version: cfg.serverVersion, APIVersion: "2" }))

  app.get("/announcement", (c) => c.json({ message: null }))

  app.get("/backends/info", (c) =>
    c.json({
      playground: { current: cfg.lesmaVersionLabel, goprev: "", gotip: "" },
      wasm: "n/a",
    }),
  )

  app.post("/v2/run", async (c) => {
    try {
      const raw = await readJsonBody(c.req.raw)
      const { files } = validatePayload(raw)
      const events = await runLesma(cfg.lesmaBin, files, cfg.runTimeoutMs)
      return c.json({ events })
    } catch (e) {
      if (e instanceof PayloadError) {
        return Response.json({ error: e.message }, { status: e.status })
      }
      return c.json({ error: e instanceof Error ? e.message : String(e) }, 500)
    }
  })

  app.post("/v2/format", (c) =>
    c.json({ error: "format is not available for Lesma in the playground" }, 501),
  )

  app.post("/v2/share", (c) =>
    c.json({ error: "snippet sharing is not available in the Lesma playground" }, 501),
  )

  app.get("/v2/share/:id", (c) =>
    c.json({ error: "snippet sharing is not available in the Lesma playground" }, 501),
  )

  app.post("/v2/compile", (c) =>
    c.json({ error: "WebAssembly Go builds are not available in the Lesma playground" }, 501),
  )

  app.get("/artifacts/*", (c) => c.json({ error: "artifact not found" }, 404))

  return app
}
