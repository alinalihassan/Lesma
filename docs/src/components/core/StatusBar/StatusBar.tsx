import React from 'react';
import { connect } from 'react-redux';
import {editor} from 'monaco-editor';
import config, {RuntimeType} from '@site/src/services/config';
import EllipsisText from '@site/src/components/utils/EllipsisText';
import StatusBarItem from '@site/src/components/core/StatusBar/StatusBarItem';
import './StatusBar.css';

interface Props {
  loading?: true
  lastError?: string
  runtime?: RuntimeType
  markers?: editor.IMarkerData[]
  dispatch?: Function
}

const getStatusItem = ({loading, lastError}) => {
  if (loading) {
    return (
      <StatusBarItem icon="Build">
        <EllipsisText>
          Loading
        </EllipsisText>
      </StatusBarItem>
    );
  }

  if (lastError) {
    return (
      <StatusBarItem
        icon="NotExecuted"
        hideTextOnMobile
        disabled
      >
      Build failed
      </StatusBarItem>
    )
  }
  return null;
}

const StatusBar: React.FC<Props> = ({
  loading, lastError, runtime, markers, dispatch
}) => {
  // const [ runSelectorModalVisible, setRunSelectorModalVisible ] = useState(false);
  const className = loading ? 'StatusBar StatusBar--busy' : 'StatusBar';
  return (
    <>
      <div className={className}>
        <div className="StatusBar__side-left">
          <StatusBarItem
            icon="ErrorBadge"
            title="No Problems"
            button
          >
            {markers?.length ?? 0} Errors
          </StatusBarItem>
          {getStatusItem({loading, lastError})}
        </div>
        <div className="StatusBar__side-right">
          {/* <StatusBarItem
            icon="Code"
            title="Select environment"
            disabled={loading}
            onClick={() => setRunSelectorModalVisible(true)}
            hideTextOnMobile
            button
          >
            Environment: {RuntimeType.toString(runtime)}
          </StatusBarItem> */}
          <StatusBarItem
            icon="Feedback"
            title="Send feedback"
            href={config.issueUrl}
            iconOnly
          />
          <StatusBarItem
            imageSrc="/github-mark-light-32px.png"
            title="GitHub"
            href={config.githubUrl}
            iconOnly
          />
        </div>
      </div>
    </>
  );
};

export default connect(({status: {loading, lastError, markers}, settings: {runtime}}: any) => (
  {loading, lastError, markers, runtime}
))(StatusBar);
