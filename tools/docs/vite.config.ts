import { reactRouter } from '@react-router/dev/vite';
import tailwindcss from '@tailwindcss/vite';
import { defineConfig } from 'vite';
import mdx from 'fumadocs-mdx/vite';
import { nodePolyfills } from 'vite-plugin-node-polyfills';
import svgr from 'vite-plugin-svgr';
import * as MdxConfig from './source.config';

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
  plugins: [
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
    port: 5174,
    strictPort: false,
    proxy: {
      '/api': {
        target: playgroundApiProxyTarget(),
        changeOrigin: true,
        ws: true,
      },
    },
  },
  resolve: {
    tsconfigPaths: true,
    // Do not alias react-dom/server → server.node.js: that entry is CJS (`require`) and
    // fails under Vite 8's ESM module runner ("require is not defined"). Normal package
    // exports use the right server build per environment (client vs SSR).
  },
});
