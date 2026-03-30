import { normalizeEvalEvent, type RunResponse } from './models/run'
import type { IAPIClient, RunRequestOptions } from './interface'

export class Client implements IAPIClient {
  constructor(private readonly baseUrl: string) {}

  /**
   * Runs the workspace on the playground server (`POST /v2/run`).
   */
  async run(files: Record<string, string>, options?: RunRequestOptions): Promise<RunResponse> {
    const body: Record<string, unknown> = { files }
    if (options?.debug !== undefined && options.debug.length > 0) {
      body.debug = options.debug
    }
    if (options?.timer === true) {
      body.timer = true
    }
    const data = await this.post<RunResponse>('/v2/run', body)
    const raw = Array.isArray(data.events) ? data.events : []
    return {
      events: raw.map((ev) => normalizeEvalEvent(ev)),
    }
  }

  private async post<T>(uri: string, data: unknown): Promise<T> {
    return await this.doRequest(uri, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(data),
    })
  }

  private async doRequest<T>(uri: string, reqInit?: RequestInit): Promise<T> {
    const reqUrl = this.baseUrl + uri
    const rsp = await fetch(reqUrl, reqInit)
    if (rsp.ok) {
      return (await rsp.json()) as T
    }

    const isJson = rsp.headers.get('content-type')
    if (!isJson) {
      throw new Error(`${rsp.status} ${rsp.statusText}`)
    }

    let errBody: { error: string }
    try {
      errBody = await rsp.json()
    } catch (_) {
      errBody = {
        error: `${rsp.status} ${rsp.statusText}`,
      }
    }

    throw new Error(errBody.error)
  }
}
