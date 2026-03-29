import { redirectTo } from './redirect-to';

export function loader() {
  return redirectTo('/docs/guides/guessing-game');
}

export default function LegacyGuessingGame() {
  return null;
}
