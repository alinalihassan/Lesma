import { lazy, Suspense } from 'react';
import type { Route } from './+types/playground';
import { HomeLayout } from 'fumadocs-ui/layouts/home';
import { HomeSiteHeader } from '@/components/site-header';
import { baseOptions } from '@/lib/layout.shared';

const PlaygroundRoot = lazy(async () => {
  const m = await import('@/playground/PlaygroundRoot');
  return { default: m.PlaygroundRoot };
});

export function meta({}: Route.MetaArgs) {
  return [{ title: 'Playground — Lesma' }];
}

export default function PlaygroundRoute() {
  return (
    <HomeLayout {...baseOptions()} slots={{ header: HomeSiteHeader }}>
      {/* `relative` so playground `.Playground { position: absolute }` is contained here, not the viewport (which hid the site header). */}
      <div className="relative flex h-[calc(100vh-3.5rem)] min-h-0 w-full flex-1 flex-col">
        <Suspense
          fallback={
            <div className="text-fd-muted-foreground flex flex-1 items-center justify-center text-sm">
              Loading playground…
            </div>
          }
        >
          <PlaygroundRoot />
        </Suspense>
      </div>
    </HomeLayout>
  );
}
