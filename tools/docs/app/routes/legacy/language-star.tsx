import type { Route } from './+types/language-star';
import { redirectTo } from './redirect-to';

const map: Record<string, string> = {
  literals: '/docs/language/literals',
  variables: '/docs/language/variables',
  types: '/docs/language/types',
  functions: '/docs/language/functions',
  'control-flow': '/docs/language/control-flow',
  comments: '/docs/language/comments',
};

export function loader({ params }: Route.LoaderArgs) {
  const raw = (params['*'] ?? '').replace(/\/$/, '');
  const key = raw.split('/')[0] ?? '';
  const target = map[key];
  if (!target) {
    throw new Response('Not found', { status: 404 });
  }
  return redirectTo(target);
}

export default function LegacyLanguageStar() {
  return null;
}
