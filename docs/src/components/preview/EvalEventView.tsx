import React from 'react';
import './EvalEventView.css';

const imageSectionPrefix = 'IMAGE:';
const base64RegEx = /^[A-Za-z0-9+/]+[=]{0,2}$/;

interface ViewData {
  message: string,
  kind: string,
  delay: number
  showDelay: boolean
}

const isImageLine = (message: string) => {
  if (!message?.startsWith(imageSectionPrefix)) {
    return [false, null];
  }

  const payload = message.substring(imageSectionPrefix.length).trim();
  return [base64RegEx.test(payload), payload];
};

export default class EvalEventView extends React.Component<ViewData> {
  get delay() {
    const msec = this.props.delay;
    return `T+${msec.toFixed(3)}s`
  }

  get domClassName() {
    return `evalEvent__msg evalEvent__msg--${this.props.kind}`;
  }

  render() {
    const { message, showDelay } = this.props;
    const [isImage, payload] = isImageLine(message);
    return <div className="evalEvent">
      { isImage ? (
        <img src={`data:image;base64,${payload}`} alt=""/>
      ) : (
        <pre className={this.domClassName}>{message}</pre>
      )}
      <span className="evalEvent__delay">{showDelay ? `[${this.delay}]` : ''}</span>
    </div>
  }
}
