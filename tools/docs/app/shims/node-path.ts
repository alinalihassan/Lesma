/**
 * Browser-safe subset of Node's `path` (POSIX-style). Used via Vite alias for `path` / `node:path`
 * so `fumadocs-mdx/runtime/server` and `fumadocs-core` can run in the client bundle without CJS
 * (`path-browserify` uses `module.exports`, which breaks Vite's ESM module runner).
 */

function splitSegments(p: string): string[] {
  return p.split(/[/\\]/).filter((s) => s.length > 0 && s !== '.');
}

export function join(...paths: string[]): string {
  const out: string[] = [];
  for (const raw of paths) {
    if (raw === '') continue;
    const parts = splitSegments(raw.replace(/\\/g, '/'));
    if (raw.startsWith('/') || raw.startsWith('\\')) {
      out.length = 0;
    }
    for (const seg of parts) {
      if (seg === '..') out.pop();
      else out.push(seg);
    }
  }
  return out.join('/');
}

export function dirname(p: string): string {
  const n = p.replace(/\\/g, '/').replace(/\/+$/, '');
  const i = n.lastIndexOf('/');
  if (i === -1) return '.';
  if (i === 0) return '/';
  return n.slice(0, i);
}

export function basename(p: string, ext?: string): string {
  const n = p.replace(/\\/g, '/');
  const base = n.slice(Math.max(n.lastIndexOf('/'), 0) + 1);
  return ext !== undefined && base.endsWith(ext) ? base.slice(0, -ext.length) : base;
}

export function extname(p: string): string {
  const b = basename(p);
  const i = b.lastIndexOf('.');
  return i <= 0 ? '' : b.slice(i);
}

const path = { join, dirname, basename, extname };
export default path;
