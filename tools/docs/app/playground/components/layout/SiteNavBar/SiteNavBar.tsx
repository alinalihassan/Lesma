import React from 'react'
import { useDispatch, useSelector } from 'react-redux'
import { BookOpen, Gamepad2, Moon, Search, Sun } from 'lucide-react'
import { clsx } from 'clsx'

import { docsSiteBaseUrl, githubRepoUrl } from '@/playground/config/siteNav'
import { GithubMark } from './GithubMark'
import { dispatchToggleTheme, type State } from '@/playground/store'

import './SiteNavBar.css'

const iconNav = { className: 'site-nav__lucide', size: 16, strokeWidth: 2 } as const
const iconTool = { className: 'site-nav__lucide', size: 20, strokeWidth: 2 } as const

export const SiteNavBar: React.FC = () => {
  const dispatch = useDispatch()
  const darkMode = useSelector(({ settings }: State) => settings.darkMode)

  const docsBase = docsSiteBaseUrl()
  const githubUrl = githubRepoUrl()
  const docsHome = docsBase ? `${docsBase}/` : '/'
  const docsSection = docsBase ? `${docsBase}/docs` : '/docs'
  const searchHint = docsBase ? `${docsBase}/docs` : '/docs'
  const logoSrc = `${import.meta.env.BASE_URL}logo.svg`

  return (
    <header className={clsx('site-nav', darkMode && 'site-nav--dark')}>
      <div className="site-nav__inner">
        <a className="site-nav__brand" href={docsHome}>
          <img src={logoSrc} alt="" width={32} height={32} className="site-nav__logo" />
          <span>Lesma</span>
        </a>

        <ul className="site-nav__links" aria-label="Site">
          <li>
            <a className="site-nav__link" href={docsSection} rel="noreferrer">
              <BookOpen {...iconNav} aria-hidden />
              Documentation
            </a>
          </li>
          <li>
            <span
              className={clsx('site-nav__link', 'site-nav__link--active')}
              aria-current="page"
            >
              <Gamepad2 {...iconNav} aria-hidden />
              Playground
            </span>
          </li>
        </ul>

        <div className="site-nav__grow" />

        <div className="site-nav__tools">
          <a
            className="site-nav__search"
            href={searchHint}
            rel="noreferrer"
            title="Open documentation (use ⌘K there to search)"
          >
            <Search {...iconNav} aria-hidden />
            <span>Search</span>
            <span className="site-nav__search-kbd">⌘K</span>
          </a>
          <button
            type="button"
            className="site-nav__icon-btn"
            aria-label={darkMode ? 'Switch to light theme' : 'Switch to dark theme'}
            onClick={() => dispatch(dispatchToggleTheme)}
          >
            {darkMode ? <Sun {...iconTool} aria-hidden /> : <Moon {...iconTool} aria-hidden />}
          </button>
          <a
            className={clsx('site-nav__icon-btn', 'site-nav__github')}
            href={githubUrl}
            target="_blank"
            rel="noreferrer noopener"
            aria-label="GitHub repository"
          >
            <GithubMark size={iconTool.size} className="site-nav__lucide" />
          </a>
        </div>

        <div className="site-nav__mobile-tools">
          <a className="site-nav__icon-btn" href={docsSection} rel="noreferrer" aria-label="Documentation">
            <BookOpen {...iconTool} aria-hidden />
          </a>
          <span
            className={clsx('site-nav__icon-btn', 'site-nav__icon-btn--active')}
            aria-label="Playground"
            aria-current="page"
          >
            <Gamepad2 {...iconTool} aria-hidden />
          </span>
          <a
            className="site-nav__icon-btn"
            href={searchHint}
            rel="noreferrer"
            aria-label="Documentation search"
          >
            <Search {...iconTool} aria-hidden />
          </a>
          <button
            type="button"
            className="site-nav__icon-btn"
            aria-label={darkMode ? 'Switch to light theme' : 'Switch to dark theme'}
            onClick={() => dispatch(dispatchToggleTheme)}
          >
            {darkMode ? <Sun {...iconTool} aria-hidden /> : <Moon {...iconTool} aria-hidden />}
          </button>
          <a
            className={clsx('site-nav__icon-btn', 'site-nav__github')}
            href={githubUrl}
            target="_blank"
            rel="noreferrer noopener"
            aria-label="GitHub"
          >
            <GithubMark size={iconTool.size} className="site-nav__lucide" />
          </a>
        </div>
      </div>
    </header>
  )
}
