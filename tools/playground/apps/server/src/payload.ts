import path from "node:path"

export const MAX_SNIPPET_SIZE = 64 * 1024
export const MAX_FILES = 10

const OTHER_EXT = new Set([".txt", ".json"])

export type FilesPayload = { files: Record<string, string> }

export class PayloadError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message)
    this.name = "PayloadError"
  }
}

function cleanPath(p: string): string {
  const s = path.posix.normalize(`/${p.trim()}`)
  return s.replace(/^\//, "")
}

/** Lesma playground: primary sources are `.les` only. */
export function validateFilePath(name: string, strict: boolean): { isPrimary: boolean } {
  const trimmed = name.trim()
  if (trimmed === "") {
    throw new PayloadError("file name cannot be empty", 400)
  }
  if (strict) {
    if (trimmed.startsWith("/")) {
      throw new PayloadError("file path cannot start with a slash", 400)
    }
    if (cleanPath(trimmed) !== trimmed) {
      throw new PayloadError(`invalid file name ${JSON.stringify(name)}`, 400)
    }
  }

  const base = path.posix.basename(trimmed)
  const ext = path.posix.extname(base).toLowerCase()
  if (ext === ".les") {
    return { isPrimary: true }
  }
  if (OTHER_EXT.has(ext)) {
    return { isPrimary: false }
  }

  throw new PayloadError(`invalid file name ${JSON.stringify(name)}`, 400)
}

export function validatePayload(body: unknown): FilesPayload {
  if (body === null || typeof body !== "object" || Array.isArray(body)) {
    throw new PayloadError("invalid JSON body", 400)
  }
  const files = (body as { files?: unknown }).files
  if (files === null || typeof files !== "object" || Array.isArray(files)) {
    throw new PayloadError("empty request", 400)
  }
  const entries = Object.entries(files as Record<string, string>)
  if (entries.length === 0) {
    throw new PayloadError("empty request", 400)
  }
  if (entries.length > MAX_FILES) {
    throw new PayloadError(`too many files (max: ${MAX_FILES})`, 400)
  }

  let hasPrimary = false
  for (const [name, src] of entries) {
    if (typeof src !== "string") {
      throw new PayloadError(`invalid contents for ${JSON.stringify(name)}`, 400)
    }
    const { isPrimary } = validateFilePath(name, true)
    if (isPrimary) hasPrimary = true
    if (src.trim() === "") {
      throw new PayloadError(`empty file ${JSON.stringify(name)}`, 400)
    }
  }

  if (!hasPrimary) {
    throw new PayloadError("no Lesma source files (.les)", 400)
  }

  return { files: files as Record<string, string> }
}

export async function readJsonBody(req: Request): Promise<unknown> {
  const len = req.headers.get("content-length")
  if (len !== null) {
    const n = Number.parseInt(len, 10)
    if (Number.isFinite(n) && n > MAX_SNIPPET_SIZE) {
      throw new PayloadError(`code snippet too large (max ${MAX_SNIPPET_SIZE} bytes)`, 413)
    }
  }

  const buf = await req.arrayBuffer()
  if (buf.byteLength > MAX_SNIPPET_SIZE) {
    throw new PayloadError(`code snippet too large (max ${MAX_SNIPPET_SIZE} bytes)`, 413)
  }

  const text = new TextDecoder().decode(buf)
  try {
    return JSON.parse(text) as unknown
  } catch {
    throw new PayloadError("invalid JSON", 400)
  }
}
