import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import copy from 'copy-to-clipboard'

import { DefaultButton, useTheme } from '@fluentui/react'

import type { ITerminalAddon, ITerminalOptions, Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { CanvasAddon } from '@xterm/addon-canvas'
import { WebglAddon } from '@xterm/addon-webgl'

import type { StatusState } from '@/playground/store'
import type { EvalEvent } from '@/playground/services/api/models/run'
import { RenderingBackend } from '@/playground/store/terminal/types'
import { XTerm } from '@/playground/components/utils/XTerm/XTerm'
import { useXtermTheme } from '@/playground/components/utils/XTerm/hooks'

import { formatEvalEvent } from './format'
import { createDebounceResizeObserver } from './utils'

import './Console.css'

const RESIZE_DELAY = 50

const config: ITerminalOptions = {
  convertEol: true,
}

export interface ConsoleProps {
  status?: StatusState
  fontFamily: string
  fontSize: number
  backend: RenderingBackend
}

const getAddonFromBackend = (backend: RenderingBackend): ITerminalAddon | null => {
  switch (backend) {
    case RenderingBackend.WebGL:
      return new WebglAddon()
    case RenderingBackend.Canvas:
      return new CanvasAddon()
    default:
      return null
  }
}

const CopyButton: React.FC<{
  onClick?: () => void
  hidden?: boolean
}> = ({ onClick, hidden }) => {
  const theme = useTheme()
  const styles = useMemo(
    () => ({
      root: {
        color: theme?.palette.neutralPrimary,
        marginLeft: 'auto',
        marginTop: '4px',
        marginRight: '2px',
        padding: '4px 8px',
        minWidth: 'initial',
      },
      rootHovered: {
        color: theme?.palette.neutralDark,
      },
    }),
    [theme],
  )
  return (
    <DefaultButton
      className="app-Console__copy"
      iconProps={{ iconName: 'Copy' }}
      ariaLabel="Copy"
      onClick={onClick}
      styles={styles}
      hidden={hidden}
    />
  )
}

/** Run output (stdout/stderr) using xterm.js. */
export const Console: React.FC<ConsoleProps> = ({ fontFamily, fontSize, status, backend }) => {
  const theme = useXtermTheme()
  const [isFocused, setIsFocused] = useState(false)
  const [xtermHost, setXtermHost] = useState<XTerm | null>(null)

  const fitAddonRef = useRef(new FitAddon())
  const eventsWrittenRef = useRef(0)

  const handleXTermRef = useCallback((instance: XTerm | null) => {
    setXtermHost(instance)
  }, [])

  const events = status?.events
  const terminal: Terminal | null = xtermHost?.terminal ?? null
  const terminalRef = useRef<Terminal | null>(null)
  terminalRef.current = terminal

  const resizeObserver = useMemo(
    () =>
      createDebounceResizeObserver(() => {
        const t = terminalRef.current
        if (t) {
          try {
            fitAddonRef.current.fit()
          } catch {
            /* ignore */
          }
        }
      }, RESIZE_DELAY),
    [fitAddonRef],
  )

  const copySelection = useCallback(() => {
    if (!terminal) {
      return
    }

    const shouldTrim = !terminal.hasSelection()
    if (!terminal.hasSelection()) {
      terminal.selectAll()
    }

    const str = terminal.getSelection()
    terminal.clearSelection()

    // TODO: notify about copy result
    copy(shouldTrim ? str.trim() : str)
  }, [terminal])

  useEffect(() => {
    const term = xtermHost?.terminal
    const list: EvalEvent[] | undefined = events

    if (!list || list.length === 0) {
      eventsWrittenRef.current = 0
      if (term) {
        try {
          term.clear()
          term.reset()
        } catch {
          /* ignore */
        }
      }
      return
    }

    if (!term) {
      return
    }

    if (eventsWrittenRef.current > list.length) {
      eventsWrittenRef.current = 0
      try {
        term.clear()
        term.reset()
      } catch {
        /* ignore */
      }
    }

    for (let i = eventsWrittenRef.current; i < list.length; i++) {
      try {
        term.write(formatEvalEvent(list[i]))
      } catch {
        /* ignore */
      }
    }
    eventsWrittenRef.current = list.length
    try {
      term.scrollToBottom()
      fitAddonRef.current.fit()
    } catch {
      /* ignore */
    }
  }, [xtermHost, events])

  useEffect(() => {
    const el = xtermHost?.terminalRef.current
    if (!el) {
      resizeObserver.disconnect()
      return
    }

    resizeObserver.observe(el)
    return () => {
      resizeObserver.disconnect()
    }
  }, [xtermHost, resizeObserver])

  useEffect(() => {
    if (!terminal) {
      return
    }

    terminal.options = {
      theme,
      fontSize,
      fontFamily,
    }
    try {
      fitAddonRef.current.fit()
    } catch {
      /* ignore */
    }
  }, [theme, terminal, fontFamily, fontSize])

  useEffect(() => {
    if (!terminal) {
      return
    }

    const addon = getAddonFromBackend(backend)
    if (!addon) {
      return
    }

    terminal.loadAddon(addon)
    return () => {
      addon.dispose()
    }
  }, [terminal, backend])

  useEffect(() => {
    if (!terminal?.textarea) {
      return
    }

    terminal.textarea.addEventListener('focus', () => {
      setIsFocused(true)
    })

    terminal.textarea.addEventListener('blur', () => {
      setTimeout(() => {
        setIsFocused(false)
      }, 150)
    })

    return () => {
      setIsFocused(false)
    }
  }, [terminal?.textarea, setIsFocused])

  return (
    <div className="app-Console" style={{ '--terminal-bg': theme.background } as any}>
      <CopyButton hidden={!isFocused} onClick={copySelection} />
      <XTerm
        ref={handleXTermRef}
        className="app-Console__xterm"
        addons={[fitAddonRef.current]}
        options={{
          ...config,
          theme,
          fontSize,
          fontFamily,
        }}
      />
    </div>
  )
}
