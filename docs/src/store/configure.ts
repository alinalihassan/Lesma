import { createBrowserHistory } from 'history'
import { compose, createStore, Store } from 'redux'

import { createRootReducer, getInitialState } from './reducers'
import { Action } from "./actions";
import { State } from "./state";

const composeEnhancers = (window as any).__REDUX_DEVTOOLS_EXTENSION_COMPOSE__ || compose;

export const history = createBrowserHistory();

export function configureStore(): Store<State, Action> {
  const preloadedState = getInitialState();
  return createStore(
    createRootReducer(history),
    preloadedState as any,
    composeEnhancers(),
  );
}
