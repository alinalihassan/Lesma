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

/**
 * Nav target for “Playground” in the header. Default: `/playground` in this SPA.
 * Set `VITE_PLAYGROUND_URL` to use a separate deployment (absolute URL).
 */
export function playgroundNavHref(): string {
  const url = import.meta.env.VITE_PLAYGROUND_URL?.trim();
  if (url) {
    return url.replace(/\/$/, '');
  }
  return '/playground';
}

