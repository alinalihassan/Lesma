import type { RunResponse, AnnouncementMessage } from './models'

export interface IAPIClient {
  run: (files: Record<string, string>) => Promise<RunResponse>

  getAnnouncementMessage: () => Promise<AnnouncementMessage | null>
}
