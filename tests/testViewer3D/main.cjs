const {app, BrowserWindow, ipcMain, dialog} = require('electron')
const path = require('node:path')
const fs = require('node:fs')

let win

ipcMain.handle('pick-log-dir', async () => {
  const r = await dialog.showOpenDialog(win, {properties: ['openDirectory'], defaultPath: process.cwd()})
  if (r.canceled || r.filePaths.length === 0) {
    return null
  }
  return r.filePaths[0]
})

ipcMain.handle('list-logs', async (_e, dir) => {
  const entries = await fs.promises.readdir(dir)
  return entries.filter((n) => n.endsWith('.meshlog.json'))
})

ipcMain.handle('read-log', async (_e, dir, name) => {
  const buf = await fs.promises.readFile(path.join(dir, name), 'utf8')
  return buf
})

function createWindow() {
  win = new BrowserWindow({
    width         : 1400,
    height        : 900,
    webPreferences: {
      contextIsolation: false,
      nodeIntegration : true,
      sandbox         : false,
    },
  })
  win.loadFile('index.html')
  win.webContents.openDevTools()
}

app.whenReady().then(createWindow)

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})
