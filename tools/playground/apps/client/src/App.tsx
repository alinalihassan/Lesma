import React from 'react'
import { Provider } from 'react-redux'
import { ConnectedRouter } from 'connected-react-router'
import { Switch, Route, Redirect } from 'react-router-dom'

import { configureStore } from './store'
import { history } from '~/store/configure'
import config from './services/config'
import { PlaygroundPage } from '~/components/pages/PlaygroundPage'
import { ConnectedThemeProvider } from '~/components/utils/ConnectedThemeProvider'
import { ApiClientProvider } from '~/services/api'

import './App.css'

// Configure store and import config from localStorage
const store = configureStore()
config.sync()

export const App = () => {
  return (
    <Provider store={store}>
      <ApiClientProvider>
        <ConnectedRouter history={history}>
          <ConnectedThemeProvider className="App">
            <Switch>
              <Route path="/" exact component={PlaygroundPage} />
              <Route path="*" render={() => <Redirect to="/" />} />
            </Switch>
          </ConnectedThemeProvider>
        </ConnectedRouter>
      </ApiClientProvider>
    </Provider>
  )
}
