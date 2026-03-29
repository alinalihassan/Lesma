import React, { useContext } from 'react'
import { type ITheme, ThemeContext } from '@fluentui/react'
import { PanelAction, type PanelActionProps } from '../PanelAction/PanelAction'
import './PanelHeader.css'

interface Props {
  /** Shown when `children` is not provided (uppercase styling). */
  label?: string
  /** Left side of the header (e.g. tabs). Replaces `label` when set. */
  children?: React.ReactNode
  commands?: Record<string, PanelActionProps>
}

export const PanelHeader: React.FC<Props> = ({ label, commands, children }) => {
  const theme = useContext(ThemeContext)
  const {
    palette: { neutralLight, neutralDark, neutralQuaternaryAlt },
  } = theme as ITheme

  return (
    <div
      className="PanelHeader"
      style={
        {
          backgroundColor: neutralLight,
          color: neutralDark,
          '--pg-panel-action-hover-bg': neutralQuaternaryAlt,
        } as any
      }
    >
      <div className="PanelHeader__side--left">
        {children ?? (label ? <span className="PanelHeader__title">{label}</span> : null)}
      </div>
      <ul className="PanelHeader__commands">
        {commands
          ? Object.entries(commands)
              .map(([key, props]) => ({ key, ...props }))
              .filter(({ hidden }) => !hidden)
              .map(({ key, ...props }) => (
                <li key={key}>
                  <PanelAction {...props} />
                </li>
              ))
          : null}
      </ul>
    </div>
  )
}
