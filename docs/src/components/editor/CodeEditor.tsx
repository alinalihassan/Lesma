import React from 'react';
import MonacoEditor from 'react-monaco-editor';
import {editor} from 'monaco-editor';
import * as monaco from 'monaco-editor';
import { attachCustomCommands } from './commands';

import {
  Connect,
  // formatFileDispatcher,
  newFileChangeAction,
  runFileDispatcher,
  newSnippetLoadDispatcher
} from '@site/src/store';
import { LANGUAGE_LESMA, stateToOptions } from './props';

interface CodeEditorState {
  code?: string
  loading?: boolean
}

@Connect(s => ({
  code: s.editor.code,
  darkMode: s.settings.darkMode,
  loading: s.status?.loading,
  options: s.monaco,
}))
export default class CodeEditor extends React.Component<any, CodeEditorState> {
  editorDidMount(editorInstance: editor.IStandaloneCodeEditor, _: monaco.editor.IEditorConstructionOptions) {
    const actions = [
      {
        id: 'clear',
        label: 'Reset contents',
        contextMenuGroupId: 'navigation',
        run: () => {
          this.props.dispatch(newSnippetLoadDispatcher());
        }
      },
      {
        id: 'run-code',
        label: 'Run Code',
        contextMenuGroupId: 'navigation',
        keybindings: [
          monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter
        ],
        run: (ed, ...args) => {
          this.props.dispatch(runFileDispatcher);
        }
      },
      // {
      //   id: 'format-code',
      //   label: 'Format Code',
      //   contextMenuGroupId: 'navigation',
      //   keybindings: [
      //     monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.KeyF
      //   ],
      //   run: (ed, ...args) => {
      //     this.props.dispatch(formatFileDispatcher);
      //   }
      // }
    ];

    // Register custom actions
    actions.forEach(action => editorInstance.addAction(action));
    attachCustomCommands(editorInstance);
    editorInstance.focus();
  }

  onChange(newValue: string, _: editor.IModelContentChangedEvent) {
    this.props.dispatch(newFileChangeAction(newValue));
  }

  render() {
    const options = stateToOptions(this.props.options);
    return (
      <MonacoEditor
        language={LANGUAGE_LESMA}
        theme={this.props.darkMode ? 'vs-dark' : 'vs-light'}
        value={this.props.code}
        options={options}
        onChange={(newVal, e) => this.onChange(newVal, e)}
        editorDidMount={(e, m: any) => this.editorDidMount(e, m)}
      />
    );
  }
}
