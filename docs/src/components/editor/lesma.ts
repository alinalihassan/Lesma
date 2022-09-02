import { languages } from "monaco-editor";

export const conf: languages.LanguageConfiguration = {
  comments: {
    lineComment: "#",
  },
  brackets: [
    ["{", "}"],
    ["[", "]"],
    ["(", ")"],
  ],
  autoClosingPairs: [
    { open: "{", close: "}" },
    { open: "[", close: "]" },
    { open: "(", close: ")" },
    { open: '"', close: '"', notIn: ["string"] },
    { open: "'", close: "'", notIn: ["string"] },
  ],
  surroundingPairs: [
    { open: "{", close: "}" },
    { open: "[", close: "]" },
    { open: "(", close: ")" },
    { open: '"', close: '"' },
    { open: "'", close: "'" },
  ],
  onEnterRules: [
    {
      beforeText: new RegExp(
        "^\\s*(?:def|class|for|if|elif|else|while|try|with|finally|except|async|match|case).*?\\s*$"
      ),
      action: { indentAction: languages.IndentAction.Indent },
    },
  ],
  folding: {
    offSide: true,
    markers: {
      start: new RegExp("^\\s*#region\\b"),
      end: new RegExp("^\\s*#endregion\\b"),
    },
  },
};

export const language: languages.IMonarchLanguage = {
  defaultToken: "",
  tokenPostfix: ".les",

  keywords: [
    "false",
    "null",
    "true",
    "and",
    "as",
    "assert",
    "async",
    "await",
    "break",
    "case",
    "class",
    "continue",
    "def",
    "defer",
    "else",
    "except",
    "export",
    "extern",
    "finally",
    "for",
    "from",
    "if",
    "import",
    "in",
    "is",
    "let",
    "not",
    "operator",
    "or",
    "raise",
    "return",
    "try",
    "var",
    "while",
    "with",
    "yield",

    "int",
    "int8",
    "int16",
    "int32",
    "int64",
    "float",
    "float32",
    "void",

    "bool",
    "char",
    "dict",
    "enum",
    "input",
    "list",
    "print",
    "self",
    "str",
    "super",
    "type",
  ],

  brackets: [
    { open: "{", close: "}", token: "delimiter.curly" },
    { open: "[", close: "]", token: "delimiter.bracket" },
    { open: "(", close: ")", token: "delimiter.parenthesis" },
  ],

  tokenizer: {
    root: [
      { include: "@whitespace" },
      { include: "@numbers" },
      { include: "@strings" },

      [/[,:;]/, "delimiter"],
      [/[{}[\]()]/, "@brackets"],

      [/@[a-zA-Z_]\w*/, "tag"],
      [
        /[a-zA-Z_]\w*/,
        {
          cases: {
            "@keywords": "keyword",
            "@default": "identifier",
          },
        },
      ],
    ],

    // Deal with white space, including single and multi-line comments
    whitespace: [
      [/\s+/, "white"],
      [/(^#.*$)/, "comment"],
      // [/'''/, "string", "@endDocString"],
      // [/"""/, "string", "@endDblDocString"],
    ],
    // endDocString: [
    //   [/[^']+/, "string"],
    //   [/\\'/, "string"],
    //   [/'''/, "string", "@popall"],
    //   [/'/, "string"],
    // ],
    // endDblDocString: [
    //   [/[^"]+/, "string"],
    //   [/\\"/, "string"],
    //   [/"""/, "string", "@popall"],
    //   [/"/, "string"],
    // ],

    // Recognize hex, negatives, decimals, imaginaries, longs, and scientific notation
    numbers: [
      // [/-?0x([abcdef]|[ABCDEF]|\d)+[lL]?/, "number.hex"],
      [/-?(\d*\.)?\d+([eE][+-]?\d+)?[jJ]?[lL]?/, "number"],
    ],

    // Recognize strings, including those broken across lines with \ (but not without)
    strings: [
      [/'$/, "string.escape", "@popall"],
      [/'/, "string.escape", "@stringBody"],
      [/"$/, "string.escape", "@popall"],
      [/"/, "string.escape", "@dblStringBody"],
    ],
    stringBody: [
      [/[^\\']+$/, "string", "@popall"],
      [/[^\\']+/, "string"],
      [/\\./, "string"],
      [/'/, "string.escape", "@popall"],
      [/\\$/, "string"],
    ],
    dblStringBody: [
      [/[^\\"]+$/, "string", "@popall"],
      [/[^\\"]+/, "string"],
      [/\\./, "string"],
      [/"/, "string.escape", "@popall"],
      [/\\$/, "string"],
    ],
  },
};
