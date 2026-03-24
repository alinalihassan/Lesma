# Lesma playground image (Bun Hono server + Vite client).
# Build from the repository root (requires vcpkg submodule):
#   docker build -f tools/playground/Dockerfile -t lesma-playground .
#
# C/C++ stage: silkeh/clang (Debian bookworm + LLVM 21 from apt.llvm.org).
# Note: silkeh/clang is commonly amd64-only; on Apple Silicon use: docker build --platform=linux/amd64 ...

FROM silkeh/clang:21-bookworm AS lesma-build
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl git \
    zip unzip ninja-build pkg-config \
    liblld-21-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN if [ -d vcpkg/.git ]; then git -C vcpkg fetch --unshallow 2>/dev/null || true; fi
RUN ./vcpkg/bootstrap-vcpkg.sh

RUN LLVM_CMAKE="$(llvm-config --cmakedir)" \
    && LLD_CMAKE="/usr/lib/llvm-21/lib/cmake/lld" \
    && cmake -S . -B build/Release -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DLLVM_DIR="${LLVM_CMAKE}" \
        -DLLD_DIR="${LLD_CMAKE}" \
        -DLESMA_BUILD_CLI=ON \
        -DLESMA_BUILD_LSP=ON \
        -DLESMA_BUILD_TESTS=OFF \
        -DLESMA_BUILD_BENCHMARKS=OFF \
        -DLESMA_BUILD_DIST=OFF

RUN cmake --build build/Release --target lesma lesma-lsp -j"$(nproc)"

FROM oven/bun:1-bookworm AS bun-workspace
WORKDIR /app
COPY tools/playground/package.json tools/playground/bun.lock ./
COPY tools/playground/apps/server/package.json ./apps/server/
COPY tools/playground/apps/client/package.json ./apps/client/
RUN bun install --frozen-lockfile
COPY tools/playground/apps/server ./apps/server
COPY tools/playground/apps/client ./apps/client
RUN bun run --cwd apps/client build

FROM oven/bun:1-bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --home-dir /opt/lesma --shell /usr/sbin/nologin playground

COPY --from=lesma-build /src/build/Release/lesma /usr/local/bin/lesma
COPY --from=lesma-build /src/build/Release/lesma-lsp /usr/local/bin/lesma-lsp
COPY --from=lesma-build /src/src/stdlib /opt/lesma/.lesma/stdlib/

COPY --from=bun-workspace /app /app
RUN rm -rf /app/apps/client/node_modules 2>/dev/null || true

RUN chown -R playground:playground /opt/lesma /app

USER playground
WORKDIR /app
ENV HOME=/opt/lesma
ENV APP_ASSETS_DIR=/app/public

EXPOSE 8080
ENTRYPOINT ["bun", "run", "--cwd", "/app/apps/server", "start"]
CMD ["--addr=:8080", "--static-dir=/app/apps/client/build"]
