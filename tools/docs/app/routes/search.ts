import { createFromSource } from 'fumadocs-core/search/server';
import { source } from '@/lib/source';

const server = createFromSource(source, {
  language: 'english',
});

/** Serves the static Orama export for `useDocsSearch({ type: 'static' })` (`fetch('/api/search')`). */
export async function loader() {
  return server.staticGET();
}
