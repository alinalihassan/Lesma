import type { EditorRemote } from '~/lib/editor'

import type { editor } from 'modern-monaco/editor-core'

import type { MonacoApi } from './init-monaco'
import { workspaceFileUri } from './workspace-uri'

export class MonacoEditorRemote implements EditorRemote {
  constructor(
    private readonly monaco: MonacoApi,
    private readonly getEditor: () => editor.IStandaloneCodeEditor | null,
  ) {}

  formatDocument(_path: string): void {
    /* lesma-lsp does not advertise formatting; hook reserved for future use */
  }

  invalidateDocument(_path: string): void {
    /* no-op: Monaco models follow Redux as source of truth */
  }

  forgetDocument(path: string): void {
    const uri = this.monaco.Uri.parse(workspaceFileUri(path))
    const model = this.monaco.editor.getModel(uri)
    if (model) {
      const ed = this.getEditor()
      if (ed?.getModel() === model) {
        ed.setModel(null)
      }
      model.dispose()
    }
  }

  focus(): void {
    this.getEditor()?.focus()
  }

  dispose(): void {
    /* Editor owns lifecycle */
  }
}
