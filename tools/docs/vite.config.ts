import { fileURLToPath } from 'node:url';

const docsPackageRoot = fileURLToPath(new URL('.', import.meta.url));
const toolsDir = fileURLToPath(new URL('..', import.meta.url));
import { reactRouter } from '@react-router/dev/vite';
import tailwindcss from '@tailwindcss/vite';
import { defineConfig } from 'vite';
import mdx from 'fumadocs-mdx/vite';
import { nodePolyfills } from 'vite-plugin-node-polyfills';
import svgr from 'vite-plugin-svgr';
import * as MdxConfig from './source.config';
import { modernMonacoShikiLoadedLanguages } from './vite-plugins/modern-monaco-shiki-languages';

function playgroundApiProxyTarget(): string {
  const explicit = process.env.VITE_API_PROXY?.trim();
  if (explicit) {
    return explicit.replace(/\/$/, '');
  }
  const addr = process.env.LISTEN_ADDR?.trim() ?? ':8080';
  if (addr.startsWith(':')) {
    return `http://127.0.0.1${addr}`;
  }
  if (/^https?:\/\//i.test(addr)) {
    return addr.replace(/\/$/, '');
  }
  return `http://${addr}`;
}

export default defineConfig({
  optimizeDeps: {
    // Let Vite transform `modern-monaco/dist/core.mjs` so custom `registerSyntax` grammars get Shiki tokenizers.
    exclude: ['modern-monaco'],
  },
  plugins: [
    modernMonacoShikiLoadedLanguages(),
    nodePolyfills({
      include: ['buffer', 'process'],
      globals: {
        Buffer: true,
        process: true,
      },
    }),
    mdx(MdxConfig),
    tailwindcss(),
    reactRouter(),
    svgr({ svgrOptions: { icon: true } }),
  ],
  server: {
    fs: {
      // `lesma.tmLanguage.json` lives under `tools/vscode/` (sibling of this package).
      allow: [docsPackageRoot, toolsDir],
    },
    port: 5174,
    strictPort: false,
    proxy: {
      '/api': {
        target: playgroundApiProxyTarget(),
        changeOrigin: true,
        ws: true,
        // Docs search is served by this app at `/api/search` (Fumadocs + `routes/search.ts`).
        // Do not forward it to the playground API (also mounted at `/api`).
        bypass(req) {
          const path = (req.url ?? '').split('?')[0] ?? '';
          if (path === '/api/search' || path.startsWith('/api/search/')) {
            return req.url;
          }
        },
      },
    },
  },
  resolve: {
    tsconfigPaths: true,
    // Client loaders pull in `fumadocs-mdx/runtime/server`, which imports `node:path`. Use a tiny
    // ESM shim — `path-browserify` is CJS and throws `module is not defined` under Vite's runner.
    alias: {
      path: fileURLToPath(new URL('./app/shims/node-path.ts', import.meta.url)),
      'node:path': fileURLToPath(new URL('./app/shims/node-path.ts', import.meta.url)),
    },
    // Do not alias react-dom/server → server.node.js: that entry is CJS (`require`) and
    // fails under Vite 8's ESM module runner ("require is not defined"). Normal package
    // exports use the right server build per environment (client vs SSR).
  },
});
