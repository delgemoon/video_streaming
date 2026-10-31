import type { VspApi } from '../../../preload/api'

/** Electron bridge with a graceful fallback when the renderer runs in a plain browser (dev/testing). */
export const bridge: VspApi =
  typeof window !== 'undefined' && window.vsp
    ? window.vsp
    : {
        pickVideo: async () => null,
        defaults: async () => ({ serverUrl: 'http://127.0.0.1:8080', version: 'web' }),
        pathForFile: () => ''
      }
