import { redirectTo } from './redirect-to';

export function loader() {
  return redirectTo('/docs/getting-started/first-program');
}

export default function LegacyHelloWorld() {
  return null;
}
