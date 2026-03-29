import React from 'react'
import {
  Checkbox,
  Dropdown,
  getTheme,
  type IPivotStyles,
  MessageBar,
  MessageBarType,
  PivotItem,
  Text,
  TextField,
} from '@fluentui/react'

import { AnimatedPivot } from '@/playground/components/elements/tabs/AnimatedPivot/AnimatedPivot'
import { ThemeableComponent } from '@/playground/components/utils/ThemeableComponent'
import { Dialog } from '@/playground/components/elements/modals/Dialog/Dialog'
import { SettingsProperty } from './SettingsProperty'
import { DEFAULT_FONT } from '@/playground/services/fonts'
import { defaultMonacoSettings, type MonacoSettings } from '@/playground/services/config/monaco'
import type { TerminalSettings } from '@/playground/store/terminal/types'
import { connect, type MonacoParamsChanges, type SettingsState } from '@/playground/store'

import { cursorBlinkOptions, cursorLineOptions, fontOptions } from './options'
import { controlKeyLabel } from '@/playground/utils/dom'
import { Kbd } from '@/playground/components/elements/misc/Kbd/Kbd'

export interface SettingsChanges {
  monaco?: MonacoParamsChanges
  settings?: Partial<SettingsState>
  terminal?: Partial<TerminalSettings>
}

interface OwnProps {
  isOpen?: boolean
  /** Persist each change as the user edits (checkboxes, dropdowns, etc.). */
  onApplyChanges: (changes: SettingsChanges) => void
  onDismiss: () => void
}

interface StateProps {
  settings?: SettingsState
  monaco?: MonacoSettings
  terminal?: TerminalSettings
}

type Props = StateProps & OwnProps

interface SettingsModalState {
  isOpen?: boolean
  hideTerminalSettings: boolean
  hideVimSettings: boolean
}

const modalStyles = {
  main: {
    maxWidth: 520,
  },
}

const pivotStyles: Partial<IPivotStyles> = {
  itemContainer: {
    // Set height to highest of pivots. See: #371
    minHeight: 567,
  },
}

class SettingsModal extends ThemeableComponent<Props, SettingsModalState> {
  constructor(props: Props) {
    super(props)
    this.state = {
      isOpen: props.isOpen,
      hideTerminalSettings: !!this.props.terminal?.disableTerminalEmulation,
      hideVimSettings: !this.props.settings?.enableVimMode,
    }
  }

  private touchMonacoProperty(key: keyof MonacoSettings, val: any) {
    this.props.onApplyChanges({ monaco: { [key]: val } as MonacoParamsChanges })
  }

  private touchSettingsProperty(changes: Partial<SettingsState>) {
    if ('enableVimMode' in changes) {
      this.setState({ hideVimSettings: !changes.enableVimMode })
    }

    this.props.onApplyChanges({ settings: changes })
  }

  private touchTerminalSettings(changes: Partial<TerminalSettings>) {
    if ('disableTerminalEmulation' in changes) {
      this.setState({ hideTerminalSettings: !!changes.disableTerminalEmulation })
    }

    this.props.onApplyChanges({ terminal: changes })
  }

  render() {
    const { isOpen } = this.props
    const { spacing } = getTheme()

    return (
      <Dialog label="Settings" onDismiss={() => this.props.onDismiss()} isOpen={isOpen} styles={modalStyles}>
        <AnimatedPivot aria-label="Settings" styles={pivotStyles}>
          <PivotItem itemKey="0" headerText="Editor">
            <SettingsProperty
              key="fontFamily"
              title="Font Family"
              description="Controls editor font family"
              control={
                <Dropdown
                  options={fontOptions}
                  defaultSelectedKey={this.props.monaco?.fontFamily ?? DEFAULT_FONT}
                  onChange={(_, num) => {
                    this.touchMonacoProperty('fontFamily', num?.key)
                  }}
                />
              }
            />
            <SettingsProperty
              key="fontLigatures"
              title="Font Ligatures"
              control={
                <Checkbox
                  label="Enable programming font ligatures in supported fonts"
                  defaultChecked={this.props.monaco?.fontLigatures}
                  onChange={(_, val) => {
                    this.touchMonacoProperty('fontLigatures', val)
                  }}
                />
              }
            />
            <SettingsProperty
              key="tabSize"
              title="Tab Size"
              description="The number of spaces a tab is equal to."
              control={
                <TextField
                  type="number"
                  min={1}
                  max={12}
                  defaultValue={`${this.props.monaco?.tabSize ?? 4}`}
                  onChange={(_, val) => {
                    const tabSize = Number(val)
                    if (!isNaN(tabSize)) {
                      this.touchMonacoProperty('tabSize', tabSize)
                    }
                  }}
                />
              }
            />
            <SettingsProperty
              key="enableAutoSave"
              title="Enable Auto Save"
              control={
                <Checkbox
                  label="Restore last editor contents when playground opens"
                  defaultChecked={this.props.settings?.autoSave}
                  onChange={(_, val) => {
                    this.touchSettingsProperty({ autoSave: val })
                  }}
                />
              }
            />
            <SettingsProperty
              key="enableVimMode"
              title="Enable Vim Mode"
              control={
                <Checkbox
                  label="Enables Vim motions in code editor"
                  defaultChecked={this.props.settings?.enableVimMode}
                  onChange={(_, val) => {
                    this.touchSettingsProperty({ enableVimMode: val })
                  }}
                />
              }
            />
            <SettingsProperty
              key="vimUseRelativeLineNumbers"
              title="Vim: Use Relative Line Numbers"
              control={
                <Checkbox
                  label="Render line numbers as distance in lines to cursor position."
                  defaultChecked={this.props.monaco?.vimUseRelativeLineNumbers}
                  disabled={this.state.hideVimSettings}
                  onChange={(_, val) => {
                    this.touchMonacoProperty('vimUseRelativeLineNumbers', val)
                  }}
                />
              }
            />
          </PivotItem>
          <PivotItem itemKey="1" headerText="Compiler">
            <SettingsProperty
              key="compilerDebugLexer"
              title="Debug: Lexer"
              description="Log lexer tokens when you run code (same as lesma run -d lexer)."
              control={
                <Checkbox
                  label="Enable lexer debug output"
                  defaultChecked={this.props.settings?.compilerDebugLexer}
                  onChange={(_, val) => {
                    this.touchSettingsProperty({ compilerDebugLexer: val })
                  }}
                />
              }
            />
            <SettingsProperty
              key="compilerDebugAst"
              title="Debug: AST"
              description="Print the abstract syntax tree after parsing (-d ast)."
              control={
                <Checkbox
                  label="Enable AST debug output"
                  defaultChecked={this.props.settings?.compilerDebugAst}
                  onChange={(_, val) => {
                    this.touchSettingsProperty({ compilerDebugAst: val })
                  }}
                />
              }
            />
            <SettingsProperty
              key="compilerDebugIr"
              title="Debug: LLVM IR"
              description="Dump LLVM IR before optimization (-d ir)."
              control={
                <Checkbox
                  label="Enable IR debug output"
                  defaultChecked={this.props.settings?.compilerDebugIr}
                  onChange={(_, val) => {
                    this.touchSettingsProperty({ compilerDebugIr: val })
                  }}
                />
              }
            />
          </PivotItem>
          <PivotItem itemKey="2" headerText="Terminal">
            <SettingsProperty
              title="Font Size"
              description="Controls the font size in pixels of the terminal."
              control={
                <TextField
                  type="number"
                  min={4}
                  max={128}
                  deferredValidationTime={0}
                  defaultValue={this.props.terminal?.fontSize.toString()}
                  onGetErrorMessage={(val) => {
                    const fontSize = Number(val)
                    if (isNaN(fontSize)) {
                      return 'Please enter a valid number'
                    }

                    if (fontSize < 4) {
                      return 'Please enter a number greater than 0'
                    }

                    this.touchTerminalSettings({ fontSize })
                  }}
                />
              }
            />
            <SettingsProperty
              key="termEmulationEnabled"
              title="Emulate Terminal"
              control={
                <Checkbox
                  label="Enables ANSI terminal escape sequences support using xterm.js."
                  defaultChecked={!this.props.terminal?.disableTerminalEmulation}
                  onChange={(_, val) => {
                    this.touchTerminalSettings({
                      disableTerminalEmulation: !val,
                    })
                  }}
                />
              }
            />
            <div>
              <MessageBar messageBarType={MessageBarType.warning}>
                Disable <u>Emulate Terminal</u> feature if you having troubles copying text from output.
              </MessageBar>
            </div>
          </PivotItem>
        </AnimatedPivot>
      </Dialog>
    )
  }
}

export const ConnectedSettingsModal = connect<StateProps, OwnProps>((state) => ({
  settings: state.settings,
  monaco: state.monaco,
  terminal: state.terminal.settings,
}))(SettingsModal)
