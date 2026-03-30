import 'core-js/actual/promise/all-settled';
import 'core-js/actual/array/flat-map';

import { Provider } from 'react-redux';
import { ApiClientProvider } from '@/playground/services/api/provider';
import { PlaygroundPage } from '@/playground/components/pages/PlaygroundPage/PlaygroundPage';
import { ConnectedThemeProvider } from '@/playground/components/utils/ConnectedThemeProvider';
import { configureStore } from '@/playground/store/configure';
import { initializeIcons } from '@/playground/icons';

import '@/playground/fonts/fontsource';
import '@/playground/index.css';

initializeIcons();

const store = configureStore();

export function PlaygroundRoot() {
  return (
    <Provider store={store}>
      <ApiClientProvider>
        <ConnectedThemeProvider className="lesma-playground-root">
          <PlaygroundPage />
        </ConnectedThemeProvider>
      </ApiClientProvider>
    </Provider>
  );
}
