const { contextBridge, ipcRenderer, webUtils } = require('electron');
contextBridge.exposeInMainWorld('flowdrop', {
  chooseFiles: () => ipcRenderer.invoke('choose-files'),
  fileInfo: paths => ipcRenderer.invoke('file-info', paths),
  convertImage: options => ipcRenderer.invoke('convert-image', options),
  openFolder: path => ipcRenderer.invoke('open-folder', path),
  setTheme: mode => ipcRenderer.invoke('set-theme', mode),
  pathForFile: file => webUtils.getPathForFile(file),
  onFilesFromShell: callback => ipcRenderer.on('files-from-shell', (_event, paths) => callback(paths))
});
