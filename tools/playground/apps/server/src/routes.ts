import { Hono } from "hono"
import type { ServerConfig } from "./config"
import { runLesma } from "./lesma"
import { PayloadError, readJsonBody, validatePayload } from "./payload"

export function createApiApp(cfg: ServerConfig): Hono {
  const app = new Hono()

  app.get("/version", (c) => c.json({ version: cfg.serverVersion, APIVersion: "2" }))

  app.get("/announcement", (c) => c.json({ message: null }))

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

  return app
}
