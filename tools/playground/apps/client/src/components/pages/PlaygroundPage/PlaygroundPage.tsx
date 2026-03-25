import React, { lazy, useEffect, useRef } from 'react'
import { useDispatch } from 'react-redux'

import { dispatchInitWorkspace } from '~/store/workspace/dispatchers/snippet'
import { Header } from '~/components/layout/Header/Header'
import { StatusBar } from '~/components/layout/StatusBar/StatusBar'

import styles from './PlaygroundPage.module.css'
import { SuspenseBoundary } from '~/components/elements/misc/SuspenseBoundary/SuspenseBoundary'

const LazyPlaygroundContent = lazy(async () => await import('./PlaygroundContainer'))

export const PlaygroundPage: React.FC = () => {
  const dispatch = useDispatch()
  const containerRef = useRef<HTMLDivElement>(null)
  useEffect(() => {
    dispatch(dispatchInitWorkspace())
  }, [dispatch])

  return (
    <div ref={containerRef} className={styles.Playground}>
      <Header />
      <SuspenseBoundary errorLabel="Failed to load workspace" preloaderText="Loading workspace...">
        <LazyPlaygroundContent parentRef={containerRef} />
      </SuspenseBoundary>
      <StatusBar />
    </div>
  )
}
