import React from 'react';
import clsx from 'clsx';
import Layout from '@theme/Layout';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import useBaseUrl from '@docusaurus/useBaseUrl';
import styles from './index.module.css';

const Button = ({ children, href }) => {
  return (
    <div className="col col--2 margin--sm">
      <Link
        className="button button--outline button--primary button--lg"
        to={href}>
        {children}
      </Link>
    </div>
  );
};

function HomepageHeader() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={clsx('hero', styles.heroBanner)}>
    <div className="container text--center">
      <div className={styles.heroLogoWrapper}>
        <img className={styles.heroLogo} src={useBaseUrl('img/logo.svg')} alt="Lesma Logo" />
      </div>
      <h2 className={clsx('hero__title', styles.heroTitle)}>{siteConfig.title}</h2>
      <p className="hero__subtitle">{siteConfig.tagline}</p>
      <div
        className={clsx(styles.heroButtons, 'name', 'margin-vert--md')}>
        <Button href={useBaseUrl('docs/introduction/getting-started')}>Get Started</Button>
        <Button href={useBaseUrl('docs/introduction/what-is-lesma')}>Learn More</Button>
      </div>
    </div>
    </header>
  );
}

export default function Home(): JSX.Element {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title={`Lesma Programming Language`}
      description="Documentation for Lesma Programming Language">
      <HomepageHeader />
      <main>
        {/*<HomepageFeatures />*/}
      </main>
    </Layout>
  );
}
