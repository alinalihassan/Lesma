import environment from '@/playground/environment'
import { Client } from './client'

const createClient = () => {
  const apiUrl = `${environment.apiUrl}/api`
  return new Client(apiUrl)
}

const instance = createClient()
export default instance
