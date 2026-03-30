import React, { lazy } from 'react'
import { SuspenseBoundary } from '@/playground/components/elements/misc/SuspenseBoundary/SuspenseBoundary'

const LazyWorkspace = lazy(async () => await import('./Workspace'))

export const LazyLoadedWorkspace: React.FC = () => (
  <SuspenseBoundary errorLabel="Failed to load workspace" preloaderText="Loading workspace...">
    <LazyWorkspace />
  </SuspenseBoundary>
)
