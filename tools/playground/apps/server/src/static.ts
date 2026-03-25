import path from "node:path"

const INDEX = "index.html"

function hasDotDot(urlPath: string): boolean {
  return urlPath.split("/").some((p) => p === "..")
}

export async function serveStatic(rootDir: string, req: Request): Promise<Response> {
  const url = new URL(req.url)
  if (req.method !== "GET" && req.method !== "HEAD") {
    return new Response("Method Not Allowed", { status: 405 })
  }

  let p = decodeURIComponent(url.pathname)
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

  const indexPath = path.join(rootDir, INDEX)
  rsp = await tryFile(indexPath)
  if (rsp !== null) return rsp

  return new Response("Not Found", { status: 404 })
}
