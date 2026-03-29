import React from 'react'

import { mergeStyleSets, useTheme } from '@fluentui/react'
import type { StatusState } from '@/playground/store'
import { EvalEventKind } from '@/playground/services/api/models/run'
import { splitImageAndText } from './utils'

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
    stderr: {
      color: theme.palette.red,
    },
    image: {
      display: 'block',
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
          const kind = ev.Kind
          const msg = ev.Message ?? ''
          if (kind === EvalEventKind.Stderr) {
            return (
              <span key={i} className={styles.stderr}>
                {msg}
              </span>
            )
          }

          // Image content and text can come mixed due to output buffering
          return splitImageAndText(msg).map(({ isImage, data }, j) => (
            <React.Fragment key={`${i}.${j}`}>
              {isImage ? (
                <img className={styles.image} key={i} src={`data:image;base64,${data}`} alt="Image output" />
              ) : (
                data
              )}
            </React.Fragment>
          ))
        })}
      </div>
      {!status?.running && <span className={styles.programExitMsg}>Program exited.</span>}
    </div>
  )
}
