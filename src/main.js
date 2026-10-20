const { app, BrowserWindow, ipcMain, dialog, shell, Tray, Menu, nativeImage, nativeTheme } = require('electron');
const path = require('node:path');
const fs = require('node:fs/promises');
const fsSync = require('node:fs');
const { execFile } = require('node:child_process');
const os = require('node:os');
const sharp = require('sharp');

let win;
let tray;
let pendingPaths = getFileArguments(process.argv);

function getFileArguments(argv) {
  return argv.slice(1).filter(argument => {
    if (argument.startsWith('-') || !fsSync.existsSync(argument)) return false;
    try {
      return fsSync.statSync(argument).isFile();
    } catch {
      return false;
    }
  });
}

function sendPathsToRenderer(paths) {
  const uniquePaths = [...new Set(paths)];
  if (!uniquePaths.length) return;
  if (win && !win.isDestroyed()) {
    win.show();
    win.focus();
    win.webContents.send('files-from-shell', uniquePaths);
  } else {
    pendingPaths.push(...uniquePaths);
  }
}

function registerShellIntegration() {
  if (process.platform !== 'win32') return;
  const command = app.isPackaged
    ? `"${process.execPath}" "%*"`
    : `"${process.execPath}" "${app.getAppPath()}" "%*"`;
  const key = 'HKCU\\Software\\Classes\\*\\shell\\FlowDrop';
  execFile('reg.exe', ['ADD', key, '/ve', '/d', 'Convert with FlowDrop', '/f'], error => {
    if (error) console.error('Could not register the FlowDrop shell menu:', error.message);
  });
  execFile('reg.exe', ['ADD', key, '/v', 'MultiSelectModel', '/d', 'Player', '/f'], error => {
    if (error) console.error('Could not configure multi-file shell selection:', error.message);
  });
  execFile('reg.exe', ['ADD', `${key}\\command`, '/ve', '/d', command, '/f'], error => {
    if (error) console.error('Could not register the FlowDrop shell command:', error.message);
  });
}

// Tangerine disc drawn procedurally so no binary asset is needed.
function trayIcon() {
  const size = 32, buf = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const d = Math.hypot(x - 15.5, y - 15.5), a = Math.max(0, Math.min(1, 14 - d + 0.5)), i = (y * size + x) * 4;
    buf[i] = 40; buf[i + 1] = 120; buf[i + 2] = 245; buf[i + 3] = Math.round(a * 255); // BGRA
  }
  return nativeImage.createFromBitmap(buf, { width: size, height: size, scaleFactor: 2 });
}

async function uniqueOutput(dir, base, format) {
  let candidate = path.join(dir, `${base}-converted.${format}`);
  for (let n = 2; n < 1000; n++) {
    try { await fs.access(candidate); } catch { return candidate; }
    candidate = path.join(dir, `${base}-converted-${n}.${format}`);
  }
  throw new Error('Could not choose a free output name.');
}

function createTray() {
  if (tray) return;
  tray = new Tray(trayIcon());
  tray.setToolTip('FlowDrop');
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: 'Open FlowDrop', click: () => { if (win) { win.show(); win.focus(); } } },
    { type: 'separator' },
    { label: 'Quit', click: () => app.quit() }
  ]));
  tray.on('click', () => { if (win) { win.show(); win.focus(); } });
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', (_event, commandLine) => {
    sendPathsToRenderer(getFileArguments(commandLine));
  });
}

const hasMica = process.platform === 'win32' && Number(os.release().split('.')[2]) >= 22621;
const chrome = () => nativeTheme.shouldUseDarkColors
  ? { bg: '#1b1b1f', symbol: '#ececf0' } : { bg: '#f3f3f6', symbol: '#1b1c21' };

function updateChrome() {
  if (!win || win.isDestroyed()) return;
  const c = chrome();
  if (!hasMica) win.setBackgroundColor(c.bg);
  if (process.platform === 'win32') win.setTitleBarOverlay({ color: '#00000000', symbolColor: c.symbol, height: 40 });
}

function createWindow() {
  const c = chrome();
  win = new BrowserWindow({
    width: 1050, height: 760, minWidth: 720, minHeight: 600, show: false,
    backgroundColor: hasMica ? '#00000000' : c.bg, title: 'FlowDrop', autoHideMenuBar: true,
    titleBarStyle: 'hidden',
    titleBarOverlay: { color: '#00000000', symbolColor: c.symbol, height: 40 },
    ...(hasMica ? { backgroundMaterial: 'mica' } : {}),
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, nodeIntegration: false }
  });
  win.once('ready-to-show', () => win.show());
  nativeTheme.on('updated', updateChrome);
  win.loadFile(path.join(__dirname, 'index.html'));
  win.on('close', event => {
    if (process.platform === 'win32' && !app.isQuitting) {
      event.preventDefault();
      win.hide();
    }
  });
  win.webContents.once('did-finish-load', () => {
    sendPathsToRenderer(pendingPaths);
    pendingPaths = [];
  });
}
app.whenReady().then(() => {
  registerShellIntegration();
  createWindow();
  createTray();
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});
app.on('before-quit', () => {
  app.isQuitting = true;
});
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });

ipcMain.handle('choose-files', async () => {
  const result = await dialog.showOpenDialog(win, { properties: ['openFile', 'multiSelections'] });
  return result.canceled ? [] : result.filePaths.map(p => ({ path: p, name: path.basename(p), ext: path.extname(p).slice(1).toLowerCase(), size: 0 }));
});
ipcMain.handle('file-info', async (_event, paths) => Promise.all(paths.map(async p => {
  try { const s = await fs.stat(p); return { path: p, name: path.basename(p), ext: path.extname(p).slice(1).toLowerCase(), size: s.size }; }
  catch { return { path: p, name: path.basename(p), ext: path.extname(p).slice(1).toLowerCase(), size: 0 }; }
})));
ipcMain.handle('convert-image', async (_event, { paths, format, quality = 88 }) => {
  const results = [];
  for (const input of paths) {
    try {
      const ext = path.extname(input);
      const output = await uniqueOutput(path.dirname(input), path.basename(input, ext), format);
      let pipeline = sharp(input, { animated: format === 'gif' });
      if (format === 'jpg' || format === 'jpeg') pipeline = pipeline.jpeg({ quality });
      else if (format === 'png') pipeline = pipeline.png({ compressionLevel: 9 });
      else if (format === 'webp') pipeline = pipeline.webp({ quality });
      else if (format === 'avif') pipeline = pipeline.avif({ quality });
      else if (format === 'tiff') pipeline = pipeline.tiff({ quality });
      else if (format === 'gif') throw new Error('GIF output is not enabled in this first prototype.');
      else throw new Error(`Unsupported output format: ${format}`);
      await pipeline.toFile(output);
      results.push({ input, output, ok: true });
    } catch (e) { results.push({ input, ok: false, error: e.message }); }
  }
  return results;
});
ipcMain.handle('open-folder', async (_event, filePath) => shell.showItemInFolder(filePath));
ipcMain.handle('set-theme', (_event, mode) => {
  if (['system', 'light', 'dark'].includes(mode)) { nativeTheme.themeSource = mode; updateChrome(); }
});
