import { redirectTo } from './redirect-to';

export function loader() {
  return redirectTo('/docs');
}

export default function LegacyWhatIsLesma() {
  return null;
}
