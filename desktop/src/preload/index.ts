import { contextBridge, ipcRenderer, webUtils } from 'electron'
import type { VspApi } from './api'

const api: VspApi = {
  pickVideo: () => ipcRenderer.invoke('dialog:pickVideo'),
  defaults: () => ipcRenderer.invoke('app:defaults'),
  pathForFile: (file: File) => webUtils.getPathForFile(file)
}

contextBridge.exposeInMainWorld('vsp', api)
