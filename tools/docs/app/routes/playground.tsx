import type { Route } from './+types/playground';
import { HomeLayout } from 'fumadocs-ui/layouts/home';
import { HomeSiteHeader } from '@/components/site-header';
import { baseOptions } from '@/lib/layout.shared';
import { playgroundEmbedSrc } from '@/lib/shared';

export function meta({}: Route.MetaArgs) {
  return [{ title: 'Playground — Lesma' }];
}

export default function PlaygroundEmbed() {
  const src = playgroundEmbedSrc();

  return (
    <HomeLayout {...baseOptions()} slots={{ header: HomeSiteHeader }}>
      <div className="flex min-h-0 w-full flex-1 flex-col">
        <iframe
          title="Lesma playground"
          src={src}
          className="bg-fd-background block h-[calc(100vh-3.5rem)] w-full shrink-0 border-0"
          allow="clipboard-read; clipboard-write"
        />
      </div>
    </HomeLayout>
  );
}
