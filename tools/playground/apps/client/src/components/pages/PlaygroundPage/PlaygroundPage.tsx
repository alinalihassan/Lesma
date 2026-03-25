import React, { lazy, useEffect, useRef } from 'react'
import { useDispatch } from 'react-redux'

import { dispatchInitWorkspace } from '~/store/workspace'
import { Header } from '~/components/layout/Header'
import { StatusBar } from '~/components/layout/StatusBar'

import styles from './PlaygroundPage.module.css'
import { SuspenseBoundary } from '~/components/elements/misc/SuspenseBoundary'
import { LazyAnnouncementBanner } from '~/components/layout/AnnouncementBanner'

const LazyPlaygroundContent = lazy(async () => await import('./PlaygroundContainer'))

export const PlaygroundPage: React.FC = () => {
  const dispatch = useDispatch()
  const containerRef = useRef<HTMLDivElement>(null)
  useEffect(() => {
    dispatch(dispatchInitWorkspace())
  }, [dispatch])

  return (
    <div ref={containerRef} className={styles.Playground}>
      <LazyAnnouncementBanner />
      <Header />
      <SuspenseBoundary errorLabel="Failed to load workspace" preloaderText="Loading workspace...">
        <LazyPlaygroundContent parentRef={containerRef} />
      </SuspenseBoundary>
      <StatusBar />
    </div>
  )
}
