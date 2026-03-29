/**
 * modern-monaco's Shiki cache uses IndexedDB whenever `globalThis.indexedDB` exists. Some
 * browsers and hosted environments expose `indexedDB` but `open()` fails ("backing store"),
 * which rejects with no fallback. Clearing `indexedDB` forces the library's in-memory cache.
 */
export async function disableIndexedDbIfBroken(): Promise<void> {
  if (typeof indexedDB === 'undefined') return

  try {
    await new Promise<void>((resolve, reject) => {
      const req = indexedDB.open('__lesma_idb_probe__', 1)
      req.onerror = () => {
        reject(req.error ?? new Error('indexedDB.open failed'))
      }
      req.onsuccess = () => {
        const db = req.result
        db.close()
        try {
          indexedDB.deleteDatabase('__lesma_idb_probe__')
        } catch {
          /* ignore */
        }
        resolve()
      }
      req.onblocked = () => resolve()
    })
  } catch {
    Object.defineProperty(globalThis, 'indexedDB', {
      value: undefined,
      configurable: true,
      writable: true,
    })
  }
}
