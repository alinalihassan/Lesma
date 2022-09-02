import React from 'react';
import { MessageBar, MessageBarType } from '@fluentui/react';

import ThemeableComponent from '@site/src/components/utils/ThemeableComponent';
import { getDefaultFontFamily } from '@site/src/services/fonts';
import { Connect } from '@site/src/store';
import { RuntimeType } from '@site/src/services/config';
import { EvalEvent } from '@site/src/services/api';
import EvalEventView from './EvalEventView';
import './Preview.css';

export interface PreviewProps {
  lastError?: string | null;
  events?: EvalEvent[]
  loading?: boolean
  runtime?: RuntimeType
}

@Connect(s => ({ darkMode: s.settings.darkMode, runtime: s.settings.runtime, ...s.status }))
export default class Preview extends ThemeableComponent<PreviewProps> {
  get styles() {
    const { palette } = this.theme;
    return {
      backgroundColor: palette.neutralLight,
      color: palette.neutralDark,
      fontFamily: getDefaultFontFamily(),
    }
  }

  render() {
    let content;
    if (this.props.lastError) {
      content = (
        <MessageBar messageBarType={MessageBarType.error} isMultiline={true}>
          <b className='app-preview__label'>Error</b>
          <pre className='app-preview__errors'>
            {this.props.lastError}
          </pre>
        </MessageBar>
      )
    } else if (this.props.events) {
      content = this.props.events.map(({Message, Delay, Kind}, k) => (
        <EvalEventView
          key={k}
          message={Message}
          delay={Delay}
          kind={Kind}
          showDelay={true}
        />
      ));

      content.push(<div className="app-preview__epilogue" key="exit">Program exited.</div>)
    } else {
      content = <span>Press "Run" to compile program.</span>;
    }

    return <div className="app-preview" style={this.styles}>
      <div className='app-preview__content'>
        {content}
      </div>
    </div>;
  }
}