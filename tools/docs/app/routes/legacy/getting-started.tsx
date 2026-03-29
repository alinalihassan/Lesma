import { redirectTo } from './redirect-to';

export function loader() {
  return redirectTo('/docs/getting-started/install');
}

export default function LegacyGettingStarted() {
  return null;
}
