import React from 'react';
import clsx from 'clsx';
import { connect } from 'react-redux';

import { dispatchPanelLayoutChange, newSnippetLoadDispatcher} from '@site/src/store';
import Header from '@site/src/components/core/Header';
import CodeEditor from '@site/src/components/editor/CodeEditor';
import FlexContainer from '@site/src/components/editor/FlexContainer';
import ResizablePreview from '@site/src/components/preview/ResizablePreview';
import Layout from '@site/src/components/core/Layout/Layout';
import DocusaurusLayout from '@theme/Layout';
import StatusBar from '@site/src/components/core/StatusBar';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import styles from './styles.module.css';

const CodeContainer = connect()(({dispatch}: any) => {
  // const { snippetID } = useParams();
  // dispatch(newSnippetLoadDispatcher(snippetID));
  return (
    <CodeEditor />
  );
})


const IDE = connect(({panel}: any) => ({panelProps: panel}))(({panelProps, dispatch}: any) => {
  const {siteConfig} = useDocusaurusContext();
  return (
    <div className='container'>
    <div className={styles.Playground}>
    <Header />
    <Layout layout={panelProps.layout}>
      <FlexContainer>
        <CodeContainer />
      </FlexContainer>
      <ResizablePreview
        {...panelProps}
        onViewChange={changes => dispatch(dispatchPanelLayoutChange(changes))}
      />
    </Layout>
    <StatusBar />
  </div>
  </div>
  );
})

const Playground = connect(({panel}: any) => ({panelProps: panel}))(({panelProps, dispatch}: any) => {
  const {siteConfig} = useDocusaurusContext();
  return (
    <DocusaurusLayout title="Lesma Programming Language" description="Lesma Playground" noFooter={true}>
      <IDE/>
    </DocusaurusLayout>
  );
});

export default Playground;
