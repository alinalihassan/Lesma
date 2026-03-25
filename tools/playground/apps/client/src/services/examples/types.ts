interface SnippetBase {
  /**
   * label is snippet label
   */
  label: string

  /**
   * Custom icon for snippet.
   */
  icon?: string

  /**
   * Custom icon color for snippet.
   */
  iconColor?: string
}

export interface SnippetSource {
  /**
   * Base path for snippet resources.
   */
  basePath: string

  /**
   * List of files.
   */
  files: string[]
}

export type Snippet = SnippetBase & {
  source: SnippetSource
}

export type Snippets = Record<string, Snippet[]>
