import React, { useEffect } from 'react'
import { connect } from 'react-redux'
import { useDispatch } from 'react-redux'
import { useTheme } from 'next-themes'
import { ThemeProvider, type ThemeProviderProps } from '@fluentui/react/lib/Theme'
import { getThemeFromVariant, ThemeVariant } from '@/playground/utils/theme'
import config from '@/playground/services/config/config'
import { newSettingsChangeAction, type SettingsState, type State } from '@/playground/store'

interface Props extends ThemeProviderProps {
  settings?: SettingsState
}

const ThemeProviderContainer: React.FunctionComponent<Props> = ({ settings, children, ...props }) => {
  const dispatch = useDispatch()
  const { resolvedTheme } = useTheme()

  useEffect(() => {
    if (resolvedTheme !== 'dark' && resolvedTheme !== 'light') {
      return
    }
    const nextDark = resolvedTheme === 'dark'
    if (settings!.darkMode === nextDark) {
      return
    }
    config.darkThemeEnabled = nextDark
    dispatch(newSettingsChangeAction({ darkMode: nextDark }))
  }, [resolvedTheme, settings, dispatch])

  const isDark = settings!.darkMode

  return (
    <ThemeProvider theme={getThemeFromVariant(isDark ? ThemeVariant.dark : ThemeVariant.light)} {...props}>
      {children}
    </ThemeProvider>
  )
}

export const ConnectedThemeProvider = connect((state: State) => ({
  settings: state.settings,
}))(ThemeProviderContainer)
