import { redirect } from 'react-router';

export function redirectTo(path: string) {
  return redirect(path);
}
