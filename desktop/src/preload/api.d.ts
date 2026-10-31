export interface VspApi {
  pickVideo: () => Promise<string | null>
  defaults: () => Promise<{ serverUrl: string; version: string }>
  pathForFile: (file: File) => string
}

declare global {
  interface Window {
    vsp: VspApi
  }
}
