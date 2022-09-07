import React, {Suspense, lazy, useRef, useEffect, useCallback} from 'react';
import Spinner from '../Spinner';
import styles from './styles.module.css';
import {registerLesmaLanguageProvider} from "@site/src/config/provider";

const MonacoEditor = lazy(() => import('react-monaco-editor'));

const defaultOptions = {
  minimap: {enabled: false},
  fontSize: '13px',
  wordWrap: 'off',
  scrollBeyondLastLine: false,
  smoothScrolling: true,
  fontFamily: "Menlo, Monaco, Consolas, 'Courier New', monospace",
};

const Placeholder = () => (
  <div className={styles.placeholder}>
    <Spinner />
  </div>
);

function Code(props) {
  const editorRef = useRef(null);

  useEffect(() => {
    const handler = () => {
      if (editorRef.current) editorRef.current.layout();
    };
    window.addEventListener('resize', handler);
    return () => window.removeEventListener('resize', handler);
  }, []);

  const onEditorDidMount = useCallback(e => {
    editorRef.current = e;
    if (props.editorDidMount) props.editorDidMount();
  }, []);

  return (
    <Suspense fallback={<Placeholder />}>
      <MonacoEditor
        {...props}
        language={"lesma"}
        options={Object.assign({}, defaultOptions, props.options)}
        editorWillMount={registerLesmaLanguageProvider}
        editorDidMount={onEditorDidMount}
        theme={'vs-light'}
      />
    </Suspense>
  );
}

export default Code;
