import { index, route, type RouteConfig } from '@react-router/dev/routes';

export default [
  index('routes/home.tsx'),
  route('playground', 'routes/playground.tsx'),

  // Legacy Starlight URLs → new docs paths
  route('getting-started', 'routes/legacy/getting-started.tsx'),
  route('hello-world', 'routes/legacy/hello-world.tsx'),
  route('guessing-game', 'routes/legacy/guessing-game.tsx'),
  route('introduction/what-is-lesma', 'routes/legacy/what-is-lesma.tsx'),
  route('language/*', 'routes/legacy/language-star.tsx'),
  route('modules/overview', 'routes/legacy/modules-overview.tsx'),

  route('docs/*', 'routes/docs.tsx'),
  route('api/search', 'routes/search.ts'),

  // LLM integration:
  route('llms.txt', 'llms/index.ts'),
  route('llms-full.txt', 'llms/full.ts'),
  route('llms.mdx/docs/*', 'llms/mdx.ts'),

  route('*', 'routes/not-found.tsx'),
] satisfies RouteConfig;
