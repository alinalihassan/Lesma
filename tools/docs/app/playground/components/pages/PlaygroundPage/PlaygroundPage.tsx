import React, { lazy, useLayoutEffect, useRef } from 'react'
import { useDispatch } from 'react-redux'

import { dispatchInitWorkspace } from '@/playground/store/workspace/dispatchers/snippet'
import { Header } from '@/playground/components/layout/Header/Header'
import { StatusBar } from '@/playground/components/layout/StatusBar/StatusBar'

import styles from './PlaygroundPage.module.css'
import { SuspenseBoundary } from '@/playground/components/elements/misc/SuspenseBoundary/SuspenseBoundary'

const LazyPlaygroundContent = lazy(async () => await import('./PlaygroundContainer'))

export const PlaygroundPage: React.FC = () => {
  const dispatch = useDispatch()
  const containerRef = useRef<HTMLDivElement>(null)
  useLayoutEffect(() => {
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
