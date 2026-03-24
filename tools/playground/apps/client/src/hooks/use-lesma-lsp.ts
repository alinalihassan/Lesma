import { LSPClient, languageServerExtensions, type Transport } from '@codemirror/lsp-client'
import type { Extension } from '@codemirror/state'
import { useEffect, useState } from 'react'

import environment from '~/environment'

function workspaceFileUri(relPath: string): string {
  const parts = relPath.split('/').map((seg) => encodeURIComponent(seg))
  return `file:///workspace/${parts.join('/')}`
}

function webSocketTransport(ws: WebSocket): Transport {
  const handlers: Array<(value: string) => void> = []
  ws.onmessage = (ev: MessageEvent<string | ArrayBuffer>) => {
    const data = typeof ev.data === 'string' ? ev.data : new TextDecoder().decode(ev.data)
    for (const h of handlers) {
      h(data)
    }
  }
  return {
    send(message: string) {
      ws.send(message)
    },
    subscribe(handler: (value: string) => void) {
      handlers.push(handler)
    },
    unsubscribe(handler: (value: string) => void) {
      const i = handlers.indexOf(handler)
      if (i >= 0) {
        handlers.splice(i, 1)
      }
    },
  }
}

/**
 * CodeMirror extension wiring the editor to lesma-lsp over WebSocket (when the active file is `.les`).
 */
export function useLesmaLspExtension(filePath: string | undefined | null): Extension[] {
  const [ext, setExt] = useState<Extension[]>([])

  useEffect(() => {
    if (!filePath?.endsWith('.les')) {
      setExt([])
      return
    }

    let disposed = false
    const client = new LSPClient({
      rootUri: 'file:///workspace',
      extensions: languageServerExtensions(),
    })

    const ws = new WebSocket(environment.lspWebSocketUrl)
    ws.binaryType = 'arraybuffer'

    const onOpen = () => {
      if (disposed) {
        return
      }
      client.connect(webSocketTransport(ws))
      void client.initializing
        .then(() => {
          if (disposed) {
            return
          }
          const uri = workspaceFileUri(filePath)
          setExt([client.plugin(uri, 'lesma')])
        })
        .catch(() => {
          if (!disposed) {
            setExt([])
          }
        })
    }

    ws.addEventListener('open', onOpen)

    return () => {
      disposed = true
      ws.removeEventListener('open', onOpen)
      client.disconnect()
      ws.close()
      setExt([])
    }
  }, [filePath])

  return ext
}
