import type { RehypeCodeOptions } from 'fumadocs-core/mdx-plugins';
import { defineConfig, defineDocs } from 'fumadocs-mdx/config';

import { lesmaTextmateGrammar } from './lib/lesma-textmate-grammar';

export const docs = defineDocs({
  dir: 'content/docs',
  docs: {
    postprocess: {
      includeProcessedMarkdown: true,
    },
  },
});

export default defineConfig({
  mdxOptions: {
    preset: 'fumadocs',
    rehypeCodeOptions: {
      engine: 'oniguruma',
      langAlias: {
        les: 'lesma',
      },
      langs: ['js', 'jsx', 'ts', 'tsx', lesmaTextmateGrammar],
    } as unknown as RehypeCodeOptions,
  },
});
