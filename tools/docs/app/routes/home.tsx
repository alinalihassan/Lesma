import type { Route } from './+types/home';
import { Card, Cards } from 'fumadocs-ui/components/card';
import { HomeLayout } from 'fumadocs-ui/layouts/home';
import { BookOpen, Download, Gamepad2, Hammer } from 'lucide-react';
import { Link } from 'react-router';
import { HomeSiteHeader } from '@/components/site-header';
import { baseOptions } from '@/lib/layout.shared';
import { gitConfig, playgroundNavPath } from '@/lib/shared';

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
  const playHref = playgroundNavPath;
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
                title="A Tour of Lesma"
                description="A longer example with comments: no main function, imports, types, classes, and control flow."
                href="/docs/getting-started/tour"
              />
              <Card
                title="Your First Program"
                description="Create a small program and run it with the `lesma` CLI."
                href="/docs/getting-started/first-program"
              />
              <Card
                title="Editor & Language Server"
                description="VS Code extension and `lesma-lsp` for diagnostics, completion, and navigation."
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
                title="Literals & Expressions"
                description="Numbers, strings, booleans, operators, and how expressions are typed."
                href="/docs/language/literals"
              />
              <Card
                title="Variables & Assignment"
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
            <h2 className="mb-2 text-2xl font-semibold">Command line</h2>
            <p className="text-fd-muted-foreground mb-6 max-w-2xl">
              Run and compile programs with the `lesma` CLI, including optimization flags for release
              builds.
            </p>
            <Cards className="grid gap-4 sm:grid-cols-2">
              <Card
                icon={<Hammer className="size-5" />}
                title="CLI Reference"
                description="`lesma run`, `lesma compile`, optimization levels, and common workflows."
                href="/docs/tooling/cli"
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
              Short recipes for everyday tasks using the standard library: files, console I/O, math,
              randomness, and sleep.
            </p>
            <Cards className="grid gap-4 sm:grid-cols-2">
              <Card
                title="Reading and Writing Files"
                description="Use the `file` type to read text, write bytes, and delete paths."
                href="/docs/guides/working-with-files"
              />
              <Card
                title="Console Input and Output"
                description="`print`, `input`, and C-style `printf` when you need precise formatting."
                href="/docs/guides/console-io"
              />
              <Card
                title="Random, Time, and Sleep"
                description="Inclusive `random`, wall-clock `time`, and `sleep` from the `time` module."
                href="/docs/guides/random-time-and-sleep"
              />
              <Card
                title="Using Math"
                description="Import `abs`, rounding, and trig helpers from the `math` module."
                href="/docs/guides/using-math"
              />
            </Cards>
            <p className="mt-4">
              <Link className="text-fd-primary text-sm font-medium" to="/docs/guides/working-with-files">
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
