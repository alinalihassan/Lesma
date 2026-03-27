import type { MonacoApi } from './init-monaco'

const keywordPattern =
  /\b(?:and|as|break|class|continue|defer|def|else|enum|export|extern|false|for|from|func|if|impl|import|in|is|let|not|null|operator|or|pass|return|super|this|trait|true|var|while)\b/

const typePattern =
  /\b(?:bool|cstr|float|float32|float64|int|int8|int16|int32|int64|uint|uint8|uint16|uint32|uint64|void)\b/

/**
 * modern-monaco only wires Shiki tokenizers for built-in tm-grammars; custom `langs` are loaded in Shiki
 * but never get `languages.register` + `setTokensProvider` in Monaco. Register Lesma with a Monarch lexer
 * so themes (one-light / one-dark-pro) apply standard token classes.
 */
export function registerLesmaMonarchLanguage(monaco: MonacoApi): void {
  if (monaco.languages.getLanguages().some((l) => l.id === 'lesma')) {
    return
  }

  monaco.languages.register({
    id: 'lesma',
    extensions: ['.les'],
    aliases: ['Lesma'],
  })

  const interpRules = [
    [/\}/, { token: 'punctuation.definition.template-expression.end', next: '@pop' }],
    [/"/, 'string', '@string_double_inner'],
    [/'/, 'string', '@string_single'],
    [/^#.*/, 'comment'],
    [/\b(?:0x[0-9a-fA-F_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)\b/, 'number'],
    [keywordPattern, 'keyword'],
    [typePattern, 'type'],
    [/[A-Z][\w]*/, 'type.identifier'],
    [/[a-z_]\w*/, 'identifier'],
    [/[{}()\x5B\x5D]/, 'delimiter.bracket'],
    [/[<>]=?|!=|==|&&|\|\||\.\.|\+=|-=|\*=|\/=|[+\-*/%^&|~]|->/, 'operator'],
    [/\s+/, 'white'],
  ]

  monaco.languages.setMonarchTokensProvider('lesma', {
    defaultToken: '',
    tokenizer: {
      root: [
        [/^#.*/, 'comment'],
        [/"(?:[^"\\]|\\.)*$/, 'string.invalid'],
        [/'(?:[^'\\]|\\.)*$/, 'string.invalid'],
        [/"/, 'string', '@string_double'],
        [/'/, 'string', '@string_single'],
        [/\b(?:0x[0-9a-fA-F_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)\b/, 'number'],
        [keywordPattern, 'keyword'],
        [typePattern, 'type'],
        [/[A-Z][\w]*/, 'type.identifier'],
        [/[a-z_]\w*/, 'identifier'],
        [/[{}()\x5B\x5D]/, 'delimiter.bracket'],
        [/[<>]=?|!=|==|&&|\|\||\.\.|\+=|-=|\*=|\/=|[+\-*/%^&|~]|->/, 'operator'],
        [/\s+/, 'white'],
      ],
      string_double: [
        [/\$\{/, { token: 'punctuation.definition.template-expression.begin', next: '@interp' }],
        [/[^\\"$]+/, 'string'],
        [/\\./, 'string.escape'],
        [/\$/, 'string'],
        [/"/, 'string', '@pop'],
      ],
      string_double_inner: [
        [/\$\{/, { token: 'punctuation.definition.template-expression.begin', next: '@interp' }],
        [/[^\\"$]+/, 'string'],
        [/\\./, 'string.escape'],
        [/\$/, 'string'],
        [/"/, 'string', '@pop'],
      ],
      interp: interpRules,
      string_single: [
        [/[^\\']+/, 'string'],
        [/\\./, 'string.escape'],
        [/'/, 'string', '@pop'],
      ],
    },
  })

  monaco.languages.setLanguageConfiguration('lesma', {
    comments: { lineComment: '#' },
    brackets: [
      ['{', '}'],
      ['[', ']'],
      ['(', ')'],
    ],
    autoClosingPairs: [
      { open: '{', close: '}' },
      { open: '[', close: ']' },
      { open: '(', close: ')' },
      { open: '"', close: '"' },
      { open: "'", close: "'" },
    ],
    surroundingPairs: [
      { open: '{', close: '}' },
      { open: '[', close: ']' },
      { open: '(', close: ')' },
      { open: '"', close: '"' },
      { open: "'", close: "'" },
    ],
  })
}
