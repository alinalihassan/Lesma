import React from 'react'
import { createRoot } from 'react-dom/client'
import './fonts/fontsource'
import { initializeIcons } from './icons'
import { App } from './App'
import './index.css'

// Polyfills
import 'core-js/actual/promise/all-settled'
import 'core-js/actual/array/flat-map'

initializeIcons()

const rootElement = document.getElementById('root')

if (!rootElement) {
  throw new Error('Root element not found')
}

createRoot(rootElement).render(<App />)

if ('serviceWorker' in navigator) {
  void navigator.serviceWorker.getRegistrations().then((r) => r.forEach((reg) => reg.unregister()))
}
