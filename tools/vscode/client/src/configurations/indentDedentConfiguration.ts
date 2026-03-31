import { IndentAction, LanguageConfiguration } from 'vscode';

function verboseRegExp(pattern: string, flags?: string): RegExp {
	pattern = pattern.replace(/(^| {2})# .*$/gm, '');
	pattern = pattern.replace(/\s+?/g, '');
	return RegExp(pattern, flags);
}

export function getIndentDedentConfiguration(): LanguageConfiguration {
    return {
        comments: {
            lineComment: '#',
        },
        brackets: [
            ['{', '}'],
            ['[', ']'],
            ['(', ')'],
            ['<', '>'],
        ],
        autoClosingPairs: [
            { open: '{', close: '}' },
            { open: '[', close: ']' },
            { open: '(', close: ')' },
            { open: '"', close: '"' },
            { open: "'", close: "'" },
        ],
        onEnterRules: [
            // multi-line separator
            {
                beforeText: verboseRegExp(`
                    ^
                    (?! \\s+ \\\\ )
                    [^#\n]+
                    \\\\
                    $
                `),
                action: {
                    indentAction: IndentAction.Indent,
                },
            },
            // continue comments
            {
                beforeText: /^\s*#.*/,
                afterText: /.+$/,
                action: {
                    indentAction: IndentAction.None,
                    appendText: '# ',
                },
            },
            // indent on enter (block-beginning statements)
            {
                /**
                 * Match parser-driven block starters for Lesma.
                 * Notably, `func extern ...` is excluded because the parser consumes only a newline there,
                 * not a braced body.
                 */
                beforeText: verboseRegExp(`
                    ^
                    \\s*
                    (?:
                        (?:
                            export \\s+
                        )?
                        (?:
                            class |
                            enum |
                            for |
                            if |
                            while
                        )
                        \\b .*
                        |
                        (?:
                            export \\s+
                        )?
                        func
                        \\b
                        (?! \\s+ extern \\b )
                        .*
                        |
                        else
                        (?:
                            \\s+ if \\b .*
                        )?
                    )
                    \\s*
                    (?: [#] .* )?
                    $
                `),
                action: {
                    indentAction: IndentAction.Indent,
                },
            },
            // outdent on enter (block-ending statements)
            {
                /**
                 * This does not handle all cases. Notable omissions here are
                 * "return" and "raise" which are complicated by the need to
                 * only outdent when the cursor is at the end of an expression
                 * rather than, say, between the parentheses of a tail-call or
                 * exception construction. (see issue #10583)
                 */
                beforeText: verboseRegExp(`
                    ^
                    (?:
                        (?:
                            \\s+
                            (?:
                                break |
                                return |
                                continue
                            )
                        )
                    )
                    \\s*
                    (?: [#] .* )?
                    $
                `),
                action: {
                    indentAction: IndentAction.Outdent,
                },
            },
            // Note that we do not currently have an auto-dedent
            // solution for "else", "except", and "finally".
            // We had one but had to remove it (see issue #6886).
        ],
    };
}