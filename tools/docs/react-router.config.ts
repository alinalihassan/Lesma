import type { Config } from '@react-router/dev/config';
import { glob } from 'node:fs/promises';
import { createGetUrl, getSlugs } from 'fumadocs-core/source';

const getUrl = createGetUrl('/docs');

export default {
  ssr: false,
  future: {
    v8_middleware: true,
  },
  async prerender({ getStaticPaths }) {
    const paths: string[] = [];
    const excluded: string[] = [];

    for (const path of getStaticPaths()) {
      if (!excluded.includes(path)) paths.push(path);
    }

    for await (const entry of glob('**/*.mdx', { cwd: 'content/docs' })) {
      const slugs = getSlugs(entry);
      paths.push(getUrl(slugs), `/llms.mdx/docs/${[...slugs, 'content.md'].join('/')}`);
    }

    const legacy = [
      '/getting-started',
      '/hello-world',
      '/guessing-game',
      '/introduction/what-is-lesma',
      '/modules/overview',
      '/language/literals',
      '/language/variables',
      '/language/types',
      '/language/functions',
      '/language/control-flow',
      '/language/comments',
    ];
    paths.push(...legacy);

    return paths;
  },
} satisfies Config;
