import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron'
import { join } from 'path'

const DEFAULT_SERVER = 'http://127.0.0.1:8080'

function createWindow(): void {
  const win = new BrowserWindow({
    width: 1380,
    height: 880,
    minWidth: 980,
    minHeight: 640,
    show: false,
    autoHideMenuBar: true,
    backgroundColor: '#0f1115',
    title: 'VSP Desktop',
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  win.on('ready-to-show', () => win.show())

  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })

  if (process.env['ELECTRON_RENDERER_URL']) {
    win.loadURL(process.env['ELECTRON_RENDERER_URL'])
  } else {
    win.loadFile(join(__dirname, '../renderer/index.html'))
  }
}

ipcMain.handle('dialog:pickVideo', async () => {
  const res = await dialog.showOpenDialog({
    title: 'Select a video to upload',
    properties: ['openFile'],
    filters: [
      { name: 'Video', extensions: ['mp4', 'mov', 'mkv', 'webm', 'avi', 'm4v', 'ts', 'mts', 'mxf', 'flv', 'wmv'] },
      { name: 'All files', extensions: ['*'] }
    ]
  })
  return res.canceled ? null : res.filePaths[0]
})

ipcMain.handle('app:defaults', () => ({
  serverUrl: process.env['VSP_SERVER_URL'] || DEFAULT_SERVER,
  version: app.getVersion()
}))

app.whenReady().then(() => {
  createWindow()
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})
