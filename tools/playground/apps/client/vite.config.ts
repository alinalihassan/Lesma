import { resolve, join } from 'path'
import react from '@vitejs/plugin-react-swc'
import { defineConfig } from 'vite'
import { nodePolyfills } from 'vite-plugin-node-polyfills'
import svgr from 'vite-plugin-svgr'
import tsConfigPaths from 'vite-tsconfig-paths'
import { createHtmlPlugin } from 'vite-plugin-html'
import 'vitest/config'

const { NODE_ENV = 'dev' } = process.env

/**
 * Where the Bun playground API listens (must match `apps/server` default :8080 unless overridden).
 * Set `VITE_API_PROXY=http://127.0.0.1:9000` if you use `LISTEN_ADDR=:9000` for the server.
 */
function playgroundApiProxyTarget(): string {
  const explicit = process.env.VITE_API_PROXY?.trim()
  if (explicit) {
    return explicit.replace(/\/$/, '')
  }
  const addr = process.env.LISTEN_ADDR?.trim() ?? ':8080'
  if (addr.startsWith(':')) {
    return `http://127.0.0.1${addr}`
  }
  if (/^https?:\/\//i.test(addr)) {
    return addr.replace(/\/$/, '')
  }
  return `http://${addr}`
}

export default defineConfig({
  resolve: {
    alias: {
      '~': resolve(__dirname, './src'),
    },
  },
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: join(__dirname, 'src/setupTests.ts'),
  },
  server: {
    port: 3000,
    host: '0.0.0.0',
    proxy: {
      '/api': {
        target: playgroundApiProxyTarget(),
        changeOrigin: true,
        ws: true,
      },
    },
  },
  build: {
    outDir: 'build',
  },
  plugins: [
    react(),
    tsConfigPaths(),
    createHtmlPlugin({
      minify: true,
      template: './src/pages/index.html',
      inject: {
        data: {
          PROD: NODE_ENV === 'production',
        },
      },
    }),
    svgr({ svgrOptions: { icon: true } }),
    nodePolyfills({
      globals: {
        Buffer: true,
        process: true,
      },
    }),
  ],
})
