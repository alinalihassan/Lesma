export const appName = 'Lesma';
export const docsRoute = '/docs';
export const docsImageRoute = '/og/docs';
export const docsContentRoute = '/llms.mdx/docs';

export const gitConfig = {
  user: 'alinalihassan',
  repo: 'Lesma',
  branch: 'main',
};

/** Blob URL prefix for “Edit on GitHub” / View options (paths under `tools/docs/content/docs`). */
export const docsGithubBlobBase = `https://github.com/${gitConfig.user}/${gitConfig.repo}/blob/${gitConfig.branch}/tools/docs/content/docs`;

/** In-app playground route (same SPA as docs). */
export const playgroundNavPath = '/playground' as const;

