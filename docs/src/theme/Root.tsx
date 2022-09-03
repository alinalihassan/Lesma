import React from 'react';
import { Provider } from 'react-redux';
import ConnectedThemeProvider from '@site/src/components/utils/ConnectedThemeProvider';
import { initializeIcons } from '@fluentui/react/lib/Icons';
import { registerLesmaLanguageProvider } from '@site/src/components/editor/provider';
import apiClient from '@site/src/services/api';
import { configureStore } from '@site/src/store'
import config from '@site/src/services/config';

initializeIcons();
registerLesmaLanguageProvider(apiClient);

// Configure store and import config from localStorage
const store = configureStore();
config.sync();

// Default implementation, that you can customize
export default function Root({ children }) {
  return (
    <Provider store={store}>
        <ConnectedThemeProvider className="App">
          <>{children}</>
        </ConnectedThemeProvider>
    </Provider>);
}