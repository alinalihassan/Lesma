# Build with Node for prerender (Bun’s react-dom server shim breaks react-router prerender).
# Bun is used only for install against bun.lock.
FROM node:22-bookworm-slim AS build

WORKDIR /app

RUN apt-get update \
  && apt-get install -y --no-install-recommends curl unzip ca-certificates \
  && rm -rf /var/lib/apt/lists/* \
  && curl -fsSL https://bun.sh/install | bash
ENV PATH="/root/.bun/bin:$PATH"

COPY . .
RUN bun install --frozen-lockfile
RUN node ./node_modules/@react-router/dev/bin.js build

FROM node:22-alpine

WORKDIR /app

RUN npm install -g serve

COPY --from=build /app/build/client ./dist
COPY --from=build /app/serve.json ./serve.json

CMD ["serve", "-s", "dist", "-l", "8080", "--config", "./serve.json"]
