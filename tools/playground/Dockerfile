# docker build -f tools/playground/Dockerfile -t lesma-web .
# Needs vcpkg submodule in context (.dockerignore keeps .git but includes vcpkg).
#
# Single SPA from tools/docs (`build/client`) + Bun `/api` on the same origin.
# Cloudflare: Workers + Containers from tools/playground (wrangler.jsonc); image_build_context is repo root.

FROM debian:bookworm-slim AS lesma-build
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg git \
    cmake ninja-build pkg-config zip unzip \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /etc/apt/trusted.gpg.d/apt.llvm.org.gpg \
    && echo "deb [signed-by=/etc/apt/trusted.gpg.d/apt.llvm.org.gpg] http://apt.llvm.org/bookworm llvm-toolchain-bookworm-21 main" \
        > /etc/apt/sources.list.d/llvm-toolchain.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang-21 llvm-21-dev liblld-21-dev lld-21 libstdc++-12-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-21 CXX=clang++-21
WORKDIR /src
COPY . .

RUN if [ -d vcpkg/.git ]; then git -C vcpkg fetch --unshallow 2>/dev/null || true; fi \
    && ./vcpkg/bootstrap-vcpkg.sh

RUN LLVM_CMAKE="$(llvm-config-21 --cmakedir)" \
    && cmake -S . -B build/Release -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
        -DLLVM_DIR="${LLVM_CMAKE}" -DLLD_DIR=/usr/lib/llvm-21/lib/cmake/lld \
        -DLESMA_BUILD_CLI=ON -DLESMA_BUILD_LSP=ON \
        -DLESMA_BUILD_TESTS=OFF -DLESMA_BUILD_BENCHMARKS=OFF -DLESMA_BUILD_DIST=OFF \
    && cmake --build build/Release --target lesma lesma-lsp -j"$(nproc)"

# Same as `tools/docs/Dockerfile`: Node runs the React Router prerender; Bun is only for `bun install` (lockfile + postinstall).
FROM node:22-bookworm-slim AS docs-build
WORKDIR /repo
RUN apt-get update \
  && apt-get install -y --no-install-recommends curl unzip ca-certificates \
  && rm -rf /var/lib/apt/lists/* \
  && curl -fsSL https://bun.sh/install | bash
ENV PATH="/root/.bun/bin:$PATH"
# Preserve `tools/docs` + `tools/vscode/syntaxes` layout: `lib/lesma-textmate-grammar.ts` imports the extension grammar.
COPY tools/docs ./tools/docs
COPY tools/vscode/syntaxes/lesma.tmLanguage.json ./tools/vscode/syntaxes/lesma.tmLanguage.json
WORKDIR /repo/tools/docs
RUN bun install --frozen-lockfile
RUN node ./node_modules/@react-router/dev/bin.js build

FROM oven/bun:1-debian AS server-workspace
WORKDIR /app
COPY tools/playground/package.json tools/playground/bun.lock ./
RUN bun install --frozen-lockfile
COPY tools/playground/src ./src
COPY tools/playground/tsconfig.json ./

FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /etc/apt/trusted.gpg.d/apt.llvm.org.gpg \
    && echo "deb [signed-by=/etc/apt/trusted.gpg.d/apt.llvm.org.gpg] http://apt.llvm.org/bookworm llvm-toolchain-bookworm-21 main" \
        > /etc/apt/sources.list.d/llvm-toolchain.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        libllvm21 libz3-4 libzstd1 libicu72 \
    && apt-get purge -y curl gnupg \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/*

COPY --from=server-workspace /usr/local/bin/bun /usr/local/bin/bun

RUN useradd --create-home --home-dir /opt/lesma --shell /usr/sbin/nologin playground

COPY --from=lesma-build /src/build/Release/lesma /usr/local/bin/lesma
COPY --from=lesma-build /src/build/Release/lesma-lsp /usr/local/bin/lesma-lsp
COPY --from=lesma-build /src/src/stdlib /opt/lesma/.lesma/stdlib/

COPY --from=server-workspace /app /app
COPY --from=docs-build /repo/tools/docs/build/client /app/site-static
RUN chown -R playground:playground /opt/lesma /app

USER playground
WORKDIR /app
ENV HOME=/opt/lesma
ENV APP_ASSETS_DIR=/app/site-static

EXPOSE 8080
ENTRYPOINT ["bun", "run", "start"]
CMD ["--addr=:8080", "--static-dir=/app/site-static"]
