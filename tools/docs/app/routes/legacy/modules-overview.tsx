import { redirectTo } from './redirect-to';

export function loader() {
  return redirectTo('/docs/language/modules');
}

export default function LegacyModulesOverview() {
  return null;
}
