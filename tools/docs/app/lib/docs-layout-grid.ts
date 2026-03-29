/**
 * Docs layout grid: full-width header row, then sidebar + main + TOC (Fumadocs default
 * puts header only over the main column so the sidebar aligns with the top bar).
 *
 * Column template must stay in sync with fumadocs-ui `layouts/docs/slots/container`.
 */
export const docsSidebarBelowHeaderGridTemplate =
  '"header header header header header" "sidebar sidebar toc-popover toc toc" "sidebar sidebar main toc toc" 1fr / minmax(min-content, 1fr) var(--fd-sidebar-col) minmax(0, calc(var(--fd-layout-width,97rem) - var(--fd-sidebar-width) - var(--fd-toc-width))) var(--fd-toc-width) minmax(min-content, 1fr)';
