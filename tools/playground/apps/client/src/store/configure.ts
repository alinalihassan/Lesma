import { createBrowserHistory } from 'history'
import { applyMiddleware, compose, createStore, type Store } from 'redux'
import thunk from 'redux-thunk'
import { routerMiddleware } from 'connected-react-router'

import { createRootReducer, getInitialState } from './reducers'
import { type Action } from './actions/actions'
import { type State } from './state'

const composeEnhancers = (window as any).__REDUX_DEVTOOLS_EXTENSION_COMPOSE__ || compose

function viteRouterBasename(): string | undefined {
  const b = import.meta.env.BASE_URL
  if (b === '/' || b === '') {
    return undefined
  }
  return b.endsWith('/') ? b.slice(0, -1) : b
}

export const history = createBrowserHistory({ basename: viteRouterBasename() })

export function configureStore(): Store<State, Action> {
  const preloadedState = getInitialState()
  return createStore(
    createRootReducer(history),
    preloadedState as any,
    composeEnhancers(applyMiddleware(routerMiddleware(history), thunk)),
  )
}
