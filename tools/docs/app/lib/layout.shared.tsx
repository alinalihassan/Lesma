import type { BaseLayoutProps } from 'fumadocs-ui/layouts/shared';
import { BookOpen, Gamepad2 } from 'lucide-react';
import { gitConfig, playgroundNavHref } from './shared';

export function baseOptions(): BaseLayoutProps {
  const play = playgroundNavHref();

  return {
    nav: {
      title: (
        <>
          <img src="/logo.svg" alt="" width={32} height={32} className="size-8" />
          <span className="font-medium">Lesma</span>
        </>
      ),
      url: '/',
    },
    links: [
      {
        type: 'main',
        text: 'Documentation',
        url: '/docs',
        icon: <BookOpen className="size-4" />,
      },
      {
        type: 'main',
        text: 'Playground',
        url: play,
        external: false,
        icon: <Gamepad2 className="size-4" />,
      },
    ],
    githubUrl: `https://github.com/${gitConfig.user}/${gitConfig.repo}`,
  };
}
