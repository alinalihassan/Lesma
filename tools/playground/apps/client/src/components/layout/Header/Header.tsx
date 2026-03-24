import React, { useCallback, useState } from 'react'
import { CommandBar, type ICommandBarItemProps, useTheme } from '@fluentui/react'
import { useDispatch, useSelector } from 'react-redux'

import type { Snippet } from '~/services/examples'
import { ConnectedSettingsModal, type SettingsChanges } from '~/components/features/settings/SettingsModal'
import { AboutModal } from '~/components/modals/AboutModal'
import { ExamplesModal } from '~/components/features/examples/ExamplesModal'
import { SharePopup } from '~/components/utils/SharePopup'

import { dispatchTerminalSettingsChange } from '~/store/terminal'
import {
  dispatchLoadSnippet,
  dispatchLoadSnippetFromSource,
  dispatchShareSnippet,
} from '~/store/workspace/dispatchers'
import {
  dispatchToggleTheme,
  newMonacoParamsChangeDispatcher,
  newSettingsChangeDispatcher,
  newUIStateChangeAction,
  runFileDispatcher,
  type State,
} from '~/store'

import './Header.css'

/**
 * Unique class name for share button to use as popover target.
 */
const BTN_SHARE_CLASSNAME = 'Header__btn--share'

export const Header: React.FC = () => {
  const dispatch = useDispatch()
  const theme = useTheme()
  const [showSettings, setShowSettings] = useState(false)
  const [showAbout, setShowAbout] = useState(false)
  const [showExamples, setShowExamples] = useState(false)

  const darkMode = useSelector(({ settings }: State) => settings.darkMode)
  const isDisabled = useSelector(({ status }: State) => Boolean(status?.loading || status?.running))
  const hideThemeToggle = useSelector(({ settings }: State) => settings.useSystemTheme)
  const sharedSnippetName = useSelector(({ ui }: State) => (ui?.shareCreated ? ui?.snippetId : undefined))
  const commandBarStateKey = isDisabled ? 'disabled' : 'enabled'

  const onSettingsClose = useCallback(
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

      setShowSettings(false)
    },
    [dispatch],
  )

  const onSnippetSelected = useCallback(
    (snippet: Snippet) => {
      setShowExamples(false)
      if (snippet.source) {
        dispatch(dispatchLoadSnippetFromSource(snippet.source))
        return
      }

      if (snippet.id) {
        dispatch(dispatchLoadSnippet(snippet.id))
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
      key: 'share',
      cacheKey: `share-${commandBarStateKey}`,
      text: 'Share',
      className: BTN_SHARE_CLASSNAME,
      iconProps: { iconName: 'Share' },
      disabled: isDisabled,
      onClick: () => {
        dispatch(dispatchShareSnippet())
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
    {
      key: 'about',
      text: 'About',
      ariaLabel: 'About',
      iconProps: { iconName: 'Info' },
      onClick: () => {
        setShowAbout(true)
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
      <SharePopup
        visible={!!sharedSnippetName?.length}
        target={`.${BTN_SHARE_CLASSNAME}`}
        snippetId={sharedSnippetName}
        onDismiss={() => {
          dispatch(newUIStateChangeAction({ shareCreated: false }))
        }}
      />
      <ConnectedSettingsModal onClose={onSettingsClose} isOpen={showSettings} />
      <AboutModal
        isOpen={showAbout}
        onClose={() => {
          setShowAbout(false)
        }}
        onTitleClick={() => {
          setShowAbout(false)
          onSnippetSelected({
            label: 'Hello world',
            source: {
              basePath: 'hello',
              files: ['main.les'],
            },
          })
        }}
      />
      <ExamplesModal
        isOpen={showExamples}
        onDismiss={() => setShowExamples(false)}
        onSelect={(s) => onSnippetSelected(s)}
      />
    </header>
  )
}
