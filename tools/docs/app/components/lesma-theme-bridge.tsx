'use client';

import { useTheme } from 'next-themes';
import { useEffect } from 'react';

import config from '@/playground/services/config/config';
import {
  LESMA_NEXT_THEME_STORAGE_KEY,
  registerLesmaThemeSetter,
  type LesmaChromeTheme,
} from '@/lib/lesma-theme-bridge';

const LEGACY_DARK_KEY = 'ui.darkTheme.enabled';

export function LesmaThemeBridge({ children }: { children: React.ReactNode }) {
  const { setTheme, resolvedTheme } = useTheme();

  useEffect(() => {
    registerLesmaThemeSetter((t: LesmaChromeTheme) => {
      setTheme(t);
    });
    return () => registerLesmaThemeSetter(null);
  }, [setTheme]);

  useEffect(() => {
    try {
      const existing = localStorage.getItem(LESMA_NEXT_THEME_STORAGE_KEY);
      if (existing === 'light' || existing === 'dark') {
        return;
      }
      const legacy = localStorage.getItem(LEGACY_DARK_KEY);
      if (legacy === 'true') {
        setTheme('dark');
      } else if (legacy === 'false') {
        setTheme('light');
      }
    } catch {
      /* ignore */
    }
  }, [setTheme]);

  useEffect(() => {
    try {
      localStorage.removeItem('ui.darkTheme.useSystem');
    } catch {
      /* ignore */
    }
  }, []);

  useEffect(() => {
    if (resolvedTheme !== 'dark' && resolvedTheme !== 'light') {
      return;
    }
    config.darkThemeEnabled = resolvedTheme === 'dark';
  }, [resolvedTheme]);

  return children;
}
