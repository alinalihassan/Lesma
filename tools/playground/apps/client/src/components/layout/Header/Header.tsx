import React, { useCallback, useState } from 'react'
import { CommandBar, type ICommandBarItemProps, useTheme } from '@fluentui/react'
import { useDispatch, useSelector } from 'react-redux'

import type { Snippet } from '~/services/examples/client'
import { ConnectedSettingsModal, type SettingsChanges } from '~/components/features/settings/SettingsModal'
import { ExamplesModal } from '~/components/features/examples/ExamplesModal/ExamplesModal'
import { dispatchTerminalSettingsChange } from '~/store/terminal/dispatchers'
import { dispatchLoadSnippetFromSource } from '~/store/workspace/dispatchers/snippet'
import {
  dispatchToggleTheme,
  newMonacoParamsChangeDispatcher,
  newSettingsChangeDispatcher,
  runFileDispatcher,
  type State,
} from '~/store'

import './Header.css'

export const Header: React.FC = () => {
  const dispatch = useDispatch()
  const theme = useTheme()
  const [showSettings, setShowSettings] = useState(false)
  const [showExamples, setShowExamples] = useState(false)

  const darkMode = useSelector(({ settings }: State) => settings.darkMode)
  const isDisabled = useSelector(({ status }: State) => Boolean(status?.loading || status?.running))
  const hideThemeToggle = useSelector(({ settings }: State) => settings.useSystemTheme)
  const commandBarStateKey = isDisabled ? 'disabled' : 'enabled'

  const applySettingsChanges = useCallback(
    (changes: SettingsChanges) => {
      if (changes.monaco) {
        dispatch(newMonacoParamsChangeDispatcher(changes.monaco))
      }

      if (changes.settings) {
        dispatch(newSettingsChangeDispatcher(changes.settings))
      }

      if (changes.terminal) {
        dispatch(dispatchTerminalSettingsChange(changes.terminal))
      }
    },
    [dispatch],
  )

  const onSettingsDismiss = useCallback(() => {
    setShowSettings(false)
  }, [])

  const onSnippetSelected = useCallback(
    (snippet: Snippet) => {
      setShowExamples(false)
      if (snippet.source) {
        dispatch(dispatchLoadSnippetFromSource(snippet.source))
      }
    },
    [dispatch],
  )

  const menuItems: ICommandBarItemProps[] = [
    {
      key: 'run',
      cacheKey: `run-${commandBarStateKey}`,
      text: 'Run',
      ariaLabel: 'Run program (Ctrl+Enter)',
      title: 'Run program (Ctrl+Enter)',
      iconProps: { iconName: 'IoMdPlay' },
      disabled: isDisabled,
      buttonStyles: {
        icon: {
          color: theme.palette.green,
        },
      },
      onClick: () => {
        dispatch(runFileDispatcher)
      },
    },
    {
      key: 'explore',
      cacheKey: `explore-${commandBarStateKey}`,
      text: 'Examples',
      iconProps: {
        iconName: 'TestExploreSolid',
      },
      disabled: isDisabled,
      onClick: () => {
        setShowExamples(true)
      },
    },
    {
      key: 'settings',
      cacheKey: `settings-${commandBarStateKey}`,
      text: 'Settings',
      ariaLabel: 'Settings',
      iconProps: { iconName: 'Settings' },
      disabled: isDisabled,
      onClick: () => {
        setShowSettings(true)
      },
    },
  ]

  const asideItems: ICommandBarItemProps[] = [
    {
      key: 'toggleTheme',
      cacheKey: `toggleTheme-${darkMode ? 'dark' : 'light'}-${hideThemeToggle ? 'hidden' : 'visible'}`,
      text: 'Toggle Dark Mode',
      ariaLabel: 'Toggle Dark Mode',
      iconOnly: true,
      hidden: hideThemeToggle,
      iconProps: { iconName: darkMode ? 'Brightness' : 'ClearNight' },
      onClick: () => {
        dispatch(dispatchToggleTheme)
      },
    },
  ]

  return (
    <header className="header" style={{ backgroundColor: theme.palette.white }}>
      <img src="/lesma-logo.svg" className="header__logo" alt="Lesma" />
      <CommandBar
        className="header__commandBar"
        items={menuItems}
        farItems={asideItems.filter(({ hidden }) => !hidden)}
        ariaLabel="CodeEditor menu"
      />
      <ConnectedSettingsModal
        onApplyChanges={applySettingsChanges}
        onDismiss={onSettingsDismiss}
        isOpen={showSettings}
      />
      <ExamplesModal
        isOpen={showExamples}
        onDismiss={() => setShowExamples(false)}
        onSelect={(s) => onSnippetSelected(s)}
      />
    </header>
  )
}
