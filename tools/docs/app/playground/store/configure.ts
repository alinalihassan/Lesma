import { applyMiddleware, compose, createStore, type Store } from 'redux';
import thunk from 'redux-thunk';

import { type Action } from './actions/actions';
import { getInitialState, rootReducer } from './reducers';
import { type State } from './state';

const composeEnhancers = (window as any).__REDUX_DEVTOOLS_EXTENSION_COMPOSE__ || compose;

export function configureStore(): Store<State, Action> {
  const preloadedState = getInitialState();
  return createStore(
    rootReducer,
    preloadedState as any,
    composeEnhancers(applyMiddleware(thunk)),
  );
}
