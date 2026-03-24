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

/** Snippet loaded by shared ID (optional; Lesma playground may use file-based examples only). */
type SharedSnippet = SnippetBase & {
  id: string

  source?: never
}

/**
 * Snippet based on file URLs.
 */
type URLSnippet = SnippetBase & {
  source: SnippetSource
  snippetId?: never
}

export type Snippet = SharedSnippet | URLSnippet

export type Snippets = Record<string, Snippet[]>
