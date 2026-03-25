import { type AnnouncementMessageResponse, type AnnouncementMessage, type RunResponse } from './models'
import type { IAPIClient } from './interface'

export class Client implements IAPIClient {
  constructor(private readonly baseUrl: string) {}

  /**
   * Runs the workspace on the playground server (`POST /v2/run`).
   */
  async run(files: Record<string, string>): Promise<RunResponse> {
    return await this.post<RunResponse>('/v2/run', { files })
  }

  /**
   * Returns important announcement message to be displayed at header banner.
   */
  async getAnnouncementMessage(): Promise<AnnouncementMessage | null> {
    const { message } = await this.get<AnnouncementMessageResponse>('/announcement')
    return message
  }

  private async get<T>(uri: string): Promise<T> {
    return await this.doRequest<T>(uri, {
      headers: {
        Accept: 'application/json',
      },
    })
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
