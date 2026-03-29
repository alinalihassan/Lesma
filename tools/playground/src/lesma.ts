import path from "node:path"

export type CompileEvent = {
  Message: string
  Kind: string
  Delay: number
}

function isLesFile(name: string): boolean {
  return path.extname(name).toLowerCase() === ".les"
}

function pickMainRel(files: Record<string, string>): string {
  const les = Object.keys(files).filter(isLesFile)
  if (les.length === 0) {
    throw new Error("no .les files in workspace")
  }
  const main = les.find((n) => path.basename(n) === "main.les")
  if (main) return main
  return [...les].sort()[0]
}

function assertSafeRel(rel: string, tmpRoot: string): string {
  const norm = rel.split(path.sep).join(path.posix.sep)
  const clean = path.posix.normalize(`/${norm}`).replace(/^\//, "")
  if (clean === "" || clean.includes("..") || path.posix.isAbsolute(clean)) {
    throw new Error(`invalid file name ${JSON.stringify(rel)}`)
  }
  const full = path.resolve(tmpRoot, clean)
  const rootWithSep = tmpRoot.endsWith(path.sep) ? tmpRoot : tmpRoot + path.sep
  if (full !== tmpRoot && !full.startsWith(rootWithSep)) {
    throw new Error(`invalid file name ${JSON.stringify(rel)}`)
  }
  return clean
}

async function rmTree(dir: string): Promise<void> {
  const { rm } = await import("node:fs/promises")
  await rm(dir, { recursive: true, force: true })
}

export async function runLesma(
  lesmaPath: string,
  files: Record<string, string>,
  timeoutMs: number,
  debug?: string[],
): Promise<CompileEvent[]> {
  let mainRel: string
  try {
    mainRel = pickMainRel(files)
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e)
    return [{ Message: msg, Kind: "stderr", Delay: 0 }]
  }

  const tmpDir = path.join(
    Bun.env.TMPDIR ?? "/tmp",
    `lesma-playground-${crypto.randomUUID()}`,
  )

  try {
    for (const [name, content] of Object.entries(files)) {
      const rel = assertSafeRel(name, tmpDir)
      const full = path.join(tmpDir, rel)
      await Bun.write(full, content)
    }

    const mainPath = path.join(tmpDir, assertSafeRel(mainRel, tmpDir))

    const controller = new AbortController()
    const timer =
      timeoutMs > 0
        ? setTimeout(() => {
            controller.abort()
          }, timeoutMs)
        : null

    let stdout = ""
    let stderr = ""
    let runErr: Error | null = null

    try {
      const spawnArgs = [lesmaPath, "run"]
      if (debug !== undefined && debug.length > 0) {
        spawnArgs.push("-d", ...debug)
      }
      spawnArgs.push(mainPath)

      const proc = Bun.spawn(spawnArgs, {
        cwd: tmpDir,
        stdout: "pipe",
        stderr: "pipe",
        stdin: "ignore",
        signal: controller.signal,
      })

      const [outText, errText, exitCode] = await Promise.all([
        new Response(proc.stdout).text(),
        new Response(proc.stderr).text(),
        proc.exited,
      ])

      stdout = outText
      stderr = errText

      if (exitCode !== 0 && runErr === null) {
        runErr = new Error(`lesma exited with code ${exitCode}`)
      }
    } catch (e) {
      if (e instanceof Error && e.name === "AbortError") {
        runErr = new Error("lesma run timed out")
      } else {
        runErr = e instanceof Error ? e : new Error(String(e))
      }
    } finally {
      if (timer) clearTimeout(timer)
    }

    const events: CompileEvent[] = []
    if (stdout.length > 0) {
      events.push({ Message: stdout, Kind: "stdout", Delay: 0 })
    }
    if (stderr.length > 0) {
      events.push({ Message: stderr, Kind: "stderr", Delay: 0 })
    }
    if (events.length === 0 && runErr !== null) {
      events.push({ Message: (runErr as Error).message, Kind: "stderr", Delay: 0 })
    }
    return events
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e)
    return [{ Message: msg, Kind: "stderr", Delay: 0 }]
  } finally {
    await rmTree(tmpDir)
  }
}
