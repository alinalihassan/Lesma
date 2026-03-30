import path from "node:path"

const INDEX = "index.html"

function hasDotDot(urlPath: string): boolean {
  return urlPath.split("/").some((p) => p === "..")
}

/**
 * Serve a static tree from `rootDir` for a URL pathname (e.g. `/` or `/assets/x.js`).
 */
export async function serveStaticDocumentRoot(
  rootDir: string,
  urlPath: string,
  req: Request,
): Promise<Response> {
  if (req.method !== "GET" && req.method !== "HEAD") {
    return new Response("Method Not Allowed", { status: 405 })
  }

  let p = decodeURIComponent(urlPath)
  if (!p.startsWith("/")) p = "/" + p
  p = path.posix.normalize(p)
  if (hasDotDot(p)) {
    return new Response("Not Found", { status: 404 })
  }

  const rel = p === "/" ? INDEX : p.slice(1)
  const filePath = path.join(rootDir, rel)

  const tryFile = async (fp: string): Promise<Response | null> => {
    const file = Bun.file(fp)
    const exists = await file.exists()
    if (!exists) return null
    const st = await file.stat()
    if (!st.isFile) return null
    if (req.method === "HEAD") {
      return new Response(null, {
        headers: { "Content-Type": file.type || "application/octet-stream" },
      })
    }
    return new Response(file)
  }

  let rsp = await tryFile(filePath)
  if (rsp !== null) return rsp

  // e.g. /playground -> playground/index.html (prerendered route directory)
  rsp = await tryFile(path.join(filePath, INDEX))
  if (rsp !== null) return rsp

  const indexPath = path.join(rootDir, INDEX)
  rsp = await tryFile(indexPath)
  if (rsp !== null) return rsp

  return new Response("Not Found", { status: 404 })
}

export async function serveStatic(rootDir: string, req: Request): Promise<Response> {
  const url = new URL(req.url)
  return serveStaticDocumentRoot(rootDir, url.pathname, req)
}

/**
 * Serve one file under `rootDir` if it exists — no directory or SPA `index.html` fallback.
 * Used for prerendered assets like `/api/search` that must not fall through to `/index.html`.
 */
export async function serveStaticFileIfExists(
  rootDir: string,
  urlPath: string,
  req: Request,
): Promise<Response | null> {
  if (req.method !== "GET" && req.method !== "HEAD") {
    return null
  }

  let p = decodeURIComponent(urlPath)
  if (!p.startsWith("/")) p = "/" + p
  p = path.posix.normalize(p)
  if (hasDotDot(p)) {
    return null
  }

  const rel = p === "/" ? INDEX : p.slice(1)
  const filePath = path.join(rootDir, rel)
  const file = Bun.file(filePath)
  const exists = await file.exists()
  if (!exists) return null
  const st = await file.stat()
  if (!st.isFile) return null

  if (req.method === "HEAD") {
    return new Response(null, {
      headers: { "Content-Type": file.type || "application/octet-stream" },
    })
  }
  return new Response(file)
}
