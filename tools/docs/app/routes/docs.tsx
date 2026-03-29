import type { Route } from './+types/docs';
import { DocsLayout } from 'fumadocs-ui/layouts/docs';
import {
  DocsBody,
  DocsDescription,
  DocsPage,
  DocsTitle,
  MarkdownCopyButton,
  ViewOptionsPopover,
} from 'fumadocs-ui/layouts/docs/page';
import { DocsSiteHeader } from '@/components/site-header';
import { cn } from '@/lib/cn';
import { getPageMarkdownUrl, source } from '@/lib/source';
import browserCollections from 'collections/browser';
import { baseOptions } from '@/lib/layout.shared';
import { docsSidebarBelowHeaderGridTemplate } from '@/lib/docs-layout-grid';
import { docsGithubBlobBase } from '@/lib/shared';
import { useFumadocsLoader } from 'fumadocs-core/source/client';
import { useMDXComponents } from '@/components/mdx';

export async function loader({ params }: Route.LoaderArgs) {
  const slugs = params['*'].split('/').filter((v) => v.length > 0);
  const page = source.getPage(slugs);
  if (!page) throw new Response('Not found', { status: 404 });

  return {
    path: page.path,
    markdownUrl: getPageMarkdownUrl(page).url,
    pageTree: await source.serializePageTree(source.getPageTree()),
  };
}

const clientLoader = browserCollections.docs.createClientLoader({
  component(
    { toc, frontmatter, default: Mdx },
    // you can define props for the component
    {
      markdownUrl,
      path,
    }: {
      markdownUrl: string;
      path: string;
    },
  ) {
    return (
      <DocsPage toc={toc}>
        <title>{frontmatter.title}</title>
        <meta name="description" content={frontmatter.description} />
        <DocsTitle>{frontmatter.title}</DocsTitle>
        <DocsDescription>{frontmatter.description}</DocsDescription>
        <div className="flex flex-row gap-2 items-center border-b -mt-4 pb-6">
          <MarkdownCopyButton markdownUrl={markdownUrl} />
          <ViewOptionsPopover
            markdownUrl={markdownUrl}
            githubUrl={`${docsGithubBlobBase}/${path}`}
          />
        </div>
        <DocsBody>
          <Mdx components={useMDXComponents()} />
        </DocsBody>
      </DocsPage>
    );
  },
});

export default function Page({ loaderData }: Route.ComponentProps) {
  const { pageTree, path, markdownUrl } = useFumadocsLoader(loaderData);

  return (
    <DocsLayout
      {...baseOptions()}
      tree={pageTree}
      sidebar={{ collapsible: false }}
      slots={{ header: DocsSiteHeader }}
      containerProps={{
        className: cn(
          'lesma-docs-sidebar-below-header',
          // Match HomeLayout `#nd-home-layout` so the shared top bar max-width aligns
          '[--fd-layout-width:1400px]',
        ),
        style: {
          gridTemplate: docsSidebarBelowHeaderGridTemplate,
          // Used with --fd-docs-row-2; overrides layout utility [--fd-header-height:0px] on #nd-docs-layout
          ['--fd-header-height' as string]: '3.5rem',
        },
      }}
    >
      {clientLoader.useContent(loaderData.path, {
        markdownUrl,
        path,
      })}
    </DocsLayout>
  );
}
