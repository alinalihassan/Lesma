# docker build -f tools/playground/Dockerfile -t lesma-web .
# Needs vcpkg submodule in context (.dockerignore keeps .git but includes vcpkg).
#
# Serves the docs SPA at `/` and the playground UI at `/playground/` (single Bun server, same `/api`).
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

FROM oven/bun:1-debian AS docs-build
WORKDIR /docs
COPY tools/docs/ ./
RUN bun install --frozen-lockfile
RUN bun run build

FROM oven/bun:1-debian AS bun-workspace
WORKDIR /app
COPY tools/playground/package.json tools/playground/bun.lock ./
COPY tools/playground/apps/server/package.json ./apps/server/
COPY tools/playground/apps/client/package.json ./apps/client/
RUN bun install --frozen-lockfile
COPY tools/playground/apps/server ./apps/server
COPY tools/playground/apps/client ./apps/client
ENV PLAYGROUND_BASE=/playground/
RUN bun run --cwd apps/client build

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

COPY --from=bun-workspace /usr/local/bin/bun /usr/local/bin/bun

RUN useradd --create-home --home-dir /opt/lesma --shell /usr/sbin/nologin playground

COPY --from=lesma-build /src/build/Release/lesma /usr/local/bin/lesma
COPY --from=lesma-build /src/build/Release/lesma-lsp /usr/local/bin/lesma-lsp
COPY --from=lesma-build /src/src/stdlib /opt/lesma/.lesma/stdlib/

COPY --from=bun-workspace /app /app
COPY --from=docs-build /docs/build/client /app/docs-static
RUN rm -rf /app/apps/client/node_modules 2>/dev/null || true \
    && chown -R playground:playground /opt/lesma /app

USER playground
WORKDIR /app
ENV HOME=/opt/lesma
ENV APP_ASSETS_DIR=/app/apps/client/build
ENV DOCS_ASSETS_DIR=/app/docs-static

EXPOSE 8080
ENTRYPOINT ["bun", "run", "--cwd", "/app/apps/server", "start"]
CMD ["--addr=:8080", "--static-dir=/app/apps/client/build", "--docs-static-dir=/app/docs-static"]
