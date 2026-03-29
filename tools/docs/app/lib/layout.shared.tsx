import type { BaseLayoutProps, LinkItemType } from 'fumadocs-ui/layouts/shared';
import { BookOpen, Gamepad2 } from 'lucide-react';
import { gitConfig, playgroundNavPath } from './shared';

const navTitle = (
  <>
    <img src="/logo.svg" alt="" width={32} height={32} className="size-8" />
    <span className="font-medium">Lesma</span>
  </>
);

/**
 * @param includeInMenu — When false, links use `on: 'nav'` so they appear in the top bar only
 * (Fumadocs docs sidebar + mobile drawer use `menuItems`). Home / playground keep `true` so small
 * screens still get Documentation + Playground in the header menu.
 */
function mainNavLinks(includeInMenu: boolean): LinkItemType[] {
  const play = playgroundNavPath;
  const placement = includeInMenu ? {} : { on: 'nav' as const };

  return [
    {
      type: 'main',
      text: 'Documentation',
      url: '/docs',
      icon: <BookOpen className="size-4" />,
      ...placement,
    },
    {
      type: 'main',
      text: 'Playground',
      url: play,
      external: false,
      icon: <Gamepad2 className="size-4" />,
      ...placement,
    },
  ];
}

export function baseOptions(): BaseLayoutProps {
  return {
    nav: {
      title: navTitle,
      url: '/',
    },
    links: mainNavLinks(true),
    githubUrl: `https://github.com/${gitConfig.user}/${gitConfig.repo}`,
  };
}

/** Docs layout: same chrome as {@link baseOptions}, but main links are header-only (not in the doc sidebar). */
export function docsBaseOptions(): BaseLayoutProps {
  return {
    nav: {
      title: navTitle,
      url: '/',
    },
    links: mainNavLinks(false),
    githubUrl: `https://github.com/${gitConfig.user}/${gitConfig.repo}`,
  };
}
