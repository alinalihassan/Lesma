import { expect, test, describe } from 'vitest'
import { splitStringUrls } from './parser'

const cases = [
  {
    label: 'no URLs',
    input: `main.les:8:28: illegal character U+003F '?' (and 1 more errors)`,
    want: [
      {
        isUrl: false,
        content: `main.les:8:28: illegal character U+003F '?' (and 1 more errors)`,
      },
    ],
  },
  {
    label: 'single URL at end',
    input: 'Some compiler output referencing documentation. See: https://example.com/docs/compiler-errors',
    want: [
      {
        isUrl: false,
        content: 'Some compiler output referencing documentation. See: ',
      },
      {
        isUrl: true,
        content: 'https://example.com/docs/compiler-errors',
      },
    ],
  },
  {
    label: 'URL at the middle of string',
    input: 'This func is deprecated, see: https://github.com/foo/bar. Also some other string',
    want: [
      {
        isUrl: false,
        content: 'This func is deprecated, see: ',
      },
      {
        isUrl: true,
        content: 'https://github.com/foo/bar',
      },
      {
        isUrl: false,
        content: '. Also some other string',
      },
    ],
  },
  {
    label: 'URL at start of a string',
    input: 'https://google.com - no such hostname',
    want: [
      {
        isUrl: true,
        content: 'https://google.com',
      },
      {
        isUrl: false,
        content: ' - no such hostname',
      },
    ],
  },
  {
    label: 'complex string',
    input:
      'import: resolving package localhost.localdomain/foo/bar\n' +
      'main.les:6:2: cannot find module localhost.localdomain/foo/bar: ' +
      'reading https://proxy.example.org/localhost.localdomain/foo/bar/@v/list: 404 Not Found\n' +
      '\tserver response: not found: localhost.localdomain/foo/bar@latest: unrecognized path ' +
      '"localhost.localdomain/foo/bar": fetch: Get "https://localhost.localdomain/foo/bar?go-get=1": dial tcp: ' +
      'lookup localhost.localdomain on 8.8.8.8:53: no such host',
    want: [
      {
        isUrl: false,
        content:
          'import: resolving package localhost.localdomain/foo/bar\n' +
          'main.les:6:2: cannot find module localhost.localdomain/foo/bar: ' +
          'reading ',
      },
      {
        isUrl: true,
        content: 'https://proxy.example.org/localhost.localdomain/foo/bar/@v/list',
      },
      {
        isUrl: false,
        content:
          ': 404 Not Found\n' +
          '\tserver response: not found: localhost.localdomain/foo/bar@latest: unrecognized path ' +
          '"localhost.localdomain/foo/bar": fetch: Get "',
      },
      {
        isUrl: true,
        content: 'https://localhost.localdomain/foo/bar?go-get=1',
      },
      {
        isUrl: false,
        content: '": dial tcp: lookup localhost.localdomain on 8.8.8.8:53: no such host',
      },
    ],
  },
]

describe.each(cases)('extractStringUrls parses string', ({ label, input, want }) => {
  test(`input with ${label}`, () => {
    const result = splitStringUrls(input)
    expect(result).toEqual(want)
  })
})
