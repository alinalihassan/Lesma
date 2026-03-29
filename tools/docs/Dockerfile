# Build from the repository root so `tools/vscode/syntaxes/lesma.tmLanguage.json` is available to the docs package:
#   docker build -f tools/docs/Dockerfile -t lesma-docs .
#
# Build with Node for prerender (Bun’s react-dom server shim breaks react-router prerender).
# Bun is used only for install against bun.lock.
FROM node:22-bookworm-slim AS build

WORKDIR /repo

RUN apt-get update \
  && apt-get install -y --no-install-recommends curl unzip ca-certificates \
  && rm -rf /var/lib/apt/lists/* \
  && curl -fsSL https://bun.sh/install | bash
ENV PATH="/root/.bun/bin:$PATH"

COPY tools/docs ./tools/docs
COPY tools/vscode/syntaxes/lesma.tmLanguage.json ./tools/vscode/syntaxes/lesma.tmLanguage.json

WORKDIR /repo/tools/docs
RUN bun install --frozen-lockfile
RUN node ./node_modules/@react-router/dev/bin.js build

FROM node:22-alpine

WORKDIR /app

RUN npm install -g serve

COPY --from=build /repo/tools/docs/build/client ./dist
COPY --from=build /repo/tools/docs/serve.json ./serve.json

CMD ["serve", "-s", "dist", "-l", "8080", "--config", "./serve.json"]
