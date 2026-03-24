import { StreamLanguage } from '@codemirror/language'

/**
 * Lightweight Lesma highlighting (keywords, strings, comments, numbers).
 * Mirrors common tokens from tools/vscode/syntaxes/lesma.tmLanguage.json in broad strokes.
 */
export const lesmaLanguage = StreamLanguage.define({
  name: 'lesma',
  token(stream) {
    if (stream.eatSpace()) {
      return null
    }
    if (stream.match(/^#.*/, false)) {
      stream.skipToEnd()
      return 'comment'
    }
    if (stream.match(/^"(?:[^"\\]|\\.)*"/) || stream.match(/^'(?:[^'\\]|\\.)*'/)) {
      return 'string'
    }
    if (stream.match(/^(?:0x[0-9a-fA-F_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)\b/)) {
      return 'number'
    }
    if (
      stream.match(
        /^(?:def|class|enum|trait|struct|impl|import|from|as|let|if|else|elif|while|for|in|return|break|continue|pass|raise|try|except|finally|with|async|await|extern|export|pub|module|type|const|True|False|None|and|or|not|is|new|self|super|where|sizeof|typeof)\b/,
      )
    ) {
      return 'keyword'
    }
    if (stream.match(/^[A-Z][\w]*/)) {
      return 'name'
    }
    if (stream.match(/^[\w]+/)) {
      return null
    }
    stream.next()
    return null
  },
})
