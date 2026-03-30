import { Container } from '@cloudflare/containers';

/**
 * One named Durable Object instance so HTTP and WebSocket (LSP) traffic stay on the same container.
 */
const singletonName = 'playground';

export class PlaygroundContainer extends Container {
  defaultPort = 8080;
  sleepAfter = '10m';
}

export interface Env {
  PLAYGROUND_CONTAINER: DurableObjectNamespace<PlaygroundContainer>;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const id = env.PLAYGROUND_CONTAINER.idFromName(singletonName);
    const stub = env.PLAYGROUND_CONTAINER.get(id);
    return stub.fetch(request);
  },
} satisfies ExportedHandler<Env>;
