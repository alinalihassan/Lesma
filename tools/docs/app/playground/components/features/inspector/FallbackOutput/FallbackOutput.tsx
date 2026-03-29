import React from 'react'

import { mergeStyleSets, useTheme } from '@fluentui/react'
import type { StatusState } from '@/playground/store'
import { EvalEventKind } from '@/playground/services/api/models/run'
import { ansiToPlaygroundHtml, containsAnsiEscapes } from '@/playground/utils/ansi'

interface Props {
  status?: StatusState
  fontFamily: string
  fontSize: number
}

/**
 * Fallback console without terminal escape sequence emulation.
 */
export const FallbackOutput: React.FC<Props> = ({ fontFamily, fontSize, status }) => {
  const theme = useTheme()
  const styles = mergeStyleSets({
    root: {
      boxSizing: 'border-box',
      flex: '1 1 0%',
      minHeight: 0,
      overflowY: 'auto',
      padding: '0 15px',
    },
    content: {
      whiteSpace: 'pre-wrap',
      display: 'block',
      width: '100%',
      font: 'inherit',
      border: 'none',
      margin: 0,
    },
    stderrPlain: {
      color: theme.palette.red,
    },
    programExitMsg: {
      marginTop: '1rem',
      display: 'inline-block',
      color: theme.semanticColors.disabledText,
    },
  })

  return (
    <div className={styles.root} style={{ fontFamily, fontSize: `${fontSize}px` }}>
      <div className={styles.content}>
        {status?.events?.map((ev, i) => {
          const msg = ev.Message ?? ''
          if (containsAnsiEscapes(msg)) {
            return (
              <span
                key={i}
                // eslint-disable-next-line react/no-danger -- Anser output from run stderr/stdout
                dangerouslySetInnerHTML={{ __html: ansiToPlaygroundHtml(msg) }}
              />
            )
          }
          const isStderr = ev.Kind === EvalEventKind.Stderr
          return (
            <span key={i} className={isStderr ? styles.stderrPlain : undefined}>
              {msg}
            </span>
          )
        })}
      </div>
      {!status?.running && <span className={styles.programExitMsg}>Program exited.</span>}
    </div>
  )
}
