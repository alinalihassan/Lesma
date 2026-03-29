import type { Route } from './+types/home';
import { Card, Cards } from 'fumadocs-ui/components/card';
import { HomeLayout } from 'fumadocs-ui/layouts/home';
import { BookOpen, Download, Gamepad2, Hammer, Wrench } from 'lucide-react';
import { Link } from 'react-router';
import { HomeSiteHeader } from '@/components/site-header';
import { baseOptions } from '@/lib/layout.shared';
import { gitConfig, playgroundNavHref } from '@/lib/shared';

export function meta({}: Route.MetaArgs) {
  return [
    { title: 'Lesma — Documentation' },
    {
      name: 'description',
      content:
        'Learn the Lesma programming language: install, language tutorials, tooling, and guides.',
    },
  ];
}

export default function Home() {
  const playHref = playgroundNavHref();
  const playIsInternal = playHref.startsWith('/');

  return (
    <HomeLayout {...baseOptions()} slots={{ header: HomeSiteHeader }}>
      <div className="flex flex-col flex-1">
        <section className="border-b bg-fd-secondary/30">
          <div className="mx-auto flex max-w-[1100px] flex-col gap-6 px-4 py-16 text-center">
            <h1 className="text-balance text-3xl font-bold tracking-tight md:text-4xl">
              Explore Lesma
            </h1>
            <p className="text-fd-muted-foreground mx-auto max-w-2xl text-lg text-pretty">
              Lesma is a compiled, statically typed, imperative, object-oriented language that targets
              LLVM. Install the toolchain, read the language tutorials, use the CLI and editor support,
              and try code in the browser playground.
            </p>
            <div className="flex flex-wrap items-center justify-center gap-3">
              <Link
                className="bg-fd-primary text-fd-primary-foreground inline-flex items-center gap-2 rounded-full px-5 py-2.5 text-sm font-medium"
                to="/docs/getting-started/install"
              >
                <Download className="size-4" />
                Install Lesma
              </Link>
              {playIsInternal ? (
                <Link
                  className="border-fd-border inline-flex items-center gap-2 rounded-full border px-5 py-2.5 text-sm font-medium"
                  to={playHref}
                >
                  <Gamepad2 className="size-4" />
                  Open playground
                </Link>
              ) : (
                <a
                  className="border-fd-border inline-flex items-center gap-2 rounded-full border px-5 py-2.5 text-sm font-medium"
                  href={playHref}
                >
                  <Gamepad2 className="size-4" />
                  Open playground
                </a>
              )}
              <Link
                className="border-fd-border inline-flex items-center gap-2 rounded-full border px-5 py-2.5 text-sm font-medium"
                to="/docs"
              >
                <BookOpen className="size-4" />
                Browse docs
              </Link>
            </div>
          </div>
        </section>

        <div className="mx-auto w-full max-w-[1100px] space-y-16 px-4 py-14">
          <section>
            <p className="text-fd-muted-foreground mb-2 text-xs font-semibold tracking-wider uppercase">
              Foundations
            </p>
            <h2 className="mb-2 text-2xl font-semibold">Get started</h2>
            <p className="text-fd-muted-foreground mb-6 max-w-2xl">
              Install a release or build the compiler from source, then write and run your first program.
            </p>
            <Cards className="grid gap-4 sm:grid-cols-2">
              <Card
                title="Installing Lesma"
                description="Use the installer script or build from source with CMake, vcpkg, and LLVM."
                href="/docs/getting-started/install"
              />
              <Card
                title="A tour of Lesma"
                description="Syntax, compilation model, and how Lesma relates to LLVM and the standard library."
                href="/docs/getting-started/tour"
              />
              <Card
                title="Your first program"
                description="Create a small program and run it with the `lesma` CLI."
                href="/docs/getting-started/first-program"
              />
              <Card
                title="Editor & language server"
                description="VS Code extension, `lesma-lsp`, and UTF-8 positions in the LSP protocol."
                href="/docs/getting-started/editor-lsp"
              />
            </Cards>
            <p className="mt-4">
              <Link className="text-fd-primary text-sm font-medium" to="/docs/getting-started/install">
                Getting started →
              </Link>
            </p>
          </section>

          <section>
            <p className="text-fd-muted-foreground mb-2 text-xs font-semibold tracking-wider uppercase">
              Language
            </p>
            <h2 className="mb-2 text-2xl font-semibold">The Lesma language</h2>
            <p className="text-fd-muted-foreground mb-6 max-w-2xl">
              Progressive tutorials covering types, functions, control flow, classes, traits, generics,
              and modules.
            </p>
            <Cards className="grid gap-4 sm:grid-cols-2">
              <Card
                title="Literals & expressions"
                description="Numbers, strings, booleans, operators, and how expressions are typed."
                href="/docs/language/literals"
              />
              <Card
                title="Variables & assignment"
                description="Mutable bindings, patterns, and updating state."
                href="/docs/language/variables"
              />
              <Card
                title="Functions"
                description="Definitions, parameters, return types, and recursion."
                href="/docs/language/functions"
              />
              <Card
                title="Classes & OOP"
                description="Classes, constructors, methods, and `self`."
                href="/docs/language/classes"
              />
            </Cards>
            <p className="mt-4">
              <Link className="text-fd-primary text-sm font-medium" to="/docs/language/literals">
                Language documentation →
              </Link>
            </p>
          </section>

          <section>
            <p className="text-fd-muted-foreground mb-2 text-xs font-semibold tracking-wider uppercase">
              Tooling
            </p>
            <h2 className="mb-2 text-2xl font-semibold">Compiler & workflow</h2>
            <p className="text-fd-muted-foreground mb-6 max-w-2xl">
              Use `lesma run` and `lesma compile`, understand optimization flags, and find the standard
              library in the repository.
            </p>
            <Cards className="grid gap-4 sm:grid-cols-2">
              <Card
                icon={<Hammer className="size-5" />}
                title="CLI reference"
                description="Run, compile, optimization levels, and typical command-line workflows."
                href="/docs/tooling/cli"
              />
              <Card
                icon={<Wrench className="size-5" />}
                title="Project layout"
                description="How the repo is organized: compiler, stdlib, tests, and benchmarks."
                href="/docs/tooling/project-layout"
              />
            </Cards>
            <p className="mt-4">
              <Link className="text-fd-primary text-sm font-medium" to="/docs/tooling/cli">
                Tooling →
              </Link>
            </p>
          </section>

          <section>
            <p className="text-fd-muted-foreground mb-2 text-xs font-semibold tracking-wider uppercase">
              Guides
            </p>
            <h2 className="mb-2 text-2xl font-semibold">Practical guides</h2>
            <p className="text-fd-muted-foreground mb-6 max-w-2xl">
              Read compiler output, debug programs, and work through a slightly larger example.
            </p>
            <Cards className="grid gap-4 sm:grid-cols-2">
              <Card
                title="Understanding errors"
                description="How Lesma reports type and compile errors and how to narrow them down."
                href="/docs/guides/errors"
              />
              <Card
                title="Debugging"
                description="Strategies for local debugging, tests, and sanitizer builds."
                href="/docs/guides/debugging"
              />
              <Card
                title="Guessing game tutorial"
                description="A small interactive program that ties together control flow and I/O ideas."
                href="/docs/guides/guessing-game"
              />
            </Cards>
            <p className="mt-4">
              <Link className="text-fd-primary text-sm font-medium" to="/docs/guides/errors">
                Guides →
              </Link>
            </p>
          </section>

          <section className="border-fd-border rounded-xl border p-6">
            <h2 className="mb-2 text-lg font-semibold">Repository</h2>
            <p className="text-fd-muted-foreground mb-4 text-sm">
              Source code, issues, and releases live on GitHub. Contributions follow the usual pull
              request workflow.
            </p>
            <a
              className="text-fd-primary text-sm font-medium"
              href={`https://github.com/${gitConfig.user}/${gitConfig.repo}`}
              rel="noreferrer"
              target="_blank"
            >
              github.com/{gitConfig.user}/{gitConfig.repo} →
            </a>
          </section>
        </div>
      </div>
    </HomeLayout>
  );
}
