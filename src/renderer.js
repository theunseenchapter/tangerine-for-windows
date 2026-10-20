const $ = id => document.getElementById(id);
const ICONS = {
  logo:'<path d="M7 7l10 10M17 9v8H9"/>',
  convert:'<path d="M4 8h13l-3-3M20 16H7l3 3"/>',
  settings:'<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>',
  upload:'<path d="M12 16V5M7.5 9.5L12 5l4.5 4.5M5 19h14"/>',
  lock:'<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 018 0v3"/>',
  trash:'<path d="M5 7h14M10 4h4M7 7l1 12h8l1-12M10 11v5M14 11v5"/>',
  chevron:'<path d="M6 9l6 6 6-6"/>',
  check:'<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  alert:'<circle cx="12" cy="12" r="8.5"/><path d="M12 8v4.5M12 16h.01"/>',
  x:'<path d="M6 6l12 12M18 6L6 18"/>',
  folder:'<path d="M4 7a2 2 0 012-2h3.5l2 2.5H18a2 2 0 012 2V17a2 2 0 01-2 2H6a2 2 0 01-2-2z"/>',
  loader:'<path d="M12 4a8 8 0 108 8"/>',
  minus:'<path d="M6 12h12"/>'
};
const icon = (n, cls = '') => `<svg viewBox="0 0 24 24" aria-hidden="true"${cls ? ` class="${cls}"` : ''}>${ICONS[n]}</svg>`;
const hydrate = root => root.querySelectorAll('i[data-icon]').forEach(el => { el.innerHTML = icon(el.dataset.icon); });
hydrate(document);

const SUPPORTED = ['jpg','jpeg','png','webp','tif','tiff','avif','bmp','gif','heic'];
const FORMAT_NAMES = { jpg:'JPG', png:'PNG', webp:'WEBP', avif:'AVIF', tiff:'TIFF' };
const DEFAULTS = { theme:'system', format:'webp', quality:'88', reveal:true };

const store = {
  load() { try { return { ...DEFAULTS, ...JSON.parse(localStorage.getItem('flowdrop.settings') || '{}') }; } catch { return { ...DEFAULTS }; } },
  save(v) { try { localStorage.setItem('flowdrop.settings', JSON.stringify(v)); } catch {} }
};
const settings = store.load();
const persist = () => store.save(settings);

let files = [];
let nextId = 1;
let selectedFormat = FORMAT_NAMES[settings.format] ? settings.format : 'webp';
let busy = false;

const isSupported = f => SUPPORTED.includes(f.ext);
const escapeHtml = s => String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function prettySize(n) {
  if (!n) return '—';
  const u = ['B','KB','MB','GB']; let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

/* ── Toast ── */
function toast(message, type = 'ok') {
  const el = $('toast');
  $('toastText').textContent = message;
  el.classList.toggle('error', type === 'error');
  el.querySelector('i').innerHTML = icon(type === 'error' ? 'alert' : 'check');
  el.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => el.classList.remove('show'), 3200);
}

/* ── Views ── */
function showView(name) {
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.id === `view-${name}`));
  document.querySelectorAll('.nav').forEach(n => {
    const on = n.dataset.view === name;
    n.classList.toggle('active', on);
    on ? n.setAttribute('aria-current', 'page') : n.removeAttribute('aria-current');
  });
}
document.querySelectorAll('.nav').forEach(n => n.addEventListener('click', () => showView(n.dataset.view)));

/* ── File list ── */
const rows = new Map();
function statusHtml(f) {
  if (!isSupported(f)) return `<span class="status error">${icon('alert')}Unsupported</span>`;
  if (f.status === 'working') return `<span class="status working">${icon('loader', 'spin')}Converting</span>`;
  if (f.status === 'done') return `<span class="status done">${icon('check')}Done</span>`;
  if (f.status === 'error') return `<span class="status error" title="${escapeHtml(f.error || '')}">${icon('alert')}Failed</span>`;
  return '<span class="status">Ready</span>';
}
function rowHtml(f) {
  const reveal = f.output ? `<button class="row-btn" data-reveal aria-label="Show converted file in folder" title="Show in folder">${icon('folder')}</button>` : '';
  return `<div class="file-type">${escapeHtml((f.ext || 'file').slice(0, 4).toUpperCase())}</div>
    <div class="file-meta"><div class="file-name" title="${escapeHtml(f.path)}">${escapeHtml(f.name)}</div><div class="file-size">${prettySize(f.size)}</div></div>
    ${statusHtml(f)}${reveal}<button class="row-btn" data-remove aria-label="Remove file" title="Remove">${icon('x')}</button>`;
}
function renderRow(f) {
  let el = rows.get(f.id);
  const fresh = !el;
  if (fresh) {
    el = document.createElement('div');
    el.className = 'file-row enter';
    el.setAttribute('role', 'listitem');
    el.addEventListener('animationend', () => el.classList.remove('enter'), { once: true });
    rows.set(f.id, el);
    $('fileList').appendChild(el);
  }
  el.classList.toggle('unsupported', !isSupported(f));
  el.innerHTML = rowHtml(f);
  el.querySelector('[data-remove]').onclick = () => removeFile(f.id);
  const rv = el.querySelector('[data-reveal]');
  if (rv) rv.onclick = () => window.flowdrop.openFolder(f.output);
}
function removeFile(id) {
  if (busy) return;
  const el = rows.get(id);
  files = files.filter(f => f.id !== id);
  rows.delete(id);
  const done = () => { el && el.remove(); sync(); };
  if (el) { el.classList.remove('enter'); el.classList.add('leave'); setTimeout(done, 190); } else done();
  sync();
}
function clearAll() {
  if (busy) return;
  files = [];
  rows.forEach(el => el.remove());
  rows.clear();
  sync();
}
function sync() {
  const n = files.length;
  document.querySelector('#view-convert').classList.toggle('has-files', n > 0);
  $('listHead').hidden = n === 0;
  $('fileCount').textContent = n;
  updateConvertButton();
}
function updateConvertButton() {
  const n = files.filter(isSupported).length;
  const btn = $('convert');
  btn.disabled = busy || n === 0;
  if (!busy) $('convertText').textContent = n ? `Convert ${n} file${n === 1 ? '' : 's'} to ${FORMAT_NAMES[selectedFormat]}` : 'Convert';
}

async function addPaths(paths) {
  const fresh = [...new Set(paths)].filter(p => p && !files.some(f => f.path === p));
  if (!fresh.length) return;
  const info = await window.flowdrop.fileInfo(fresh);
  info.forEach(i => { const f = { ...i, id: nextId++, status: 'ready' }; files.push(f); renderRow(f); });
  sync();
}
window.flowdrop.onFilesFromShell(addPaths);

/* ── Dropzone ── */
const dz = $('dropzone');
async function browse() { const chosen = await window.flowdrop.chooseFiles(); await addPaths(chosen.map(f => f.path)); }
dz.addEventListener('click', browse);
dz.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); browse(); } });
let dragDepth = 0;
document.addEventListener('dragenter', e => { e.preventDefault(); dragDepth++; dz.classList.add('dragover'); });
document.addEventListener('dragover', e => e.preventDefault());
document.addEventListener('dragleave', e => { e.preventDefault(); dragDepth = Math.max(0, dragDepth - 1); if (!dragDepth) dz.classList.remove('dragover'); });
document.addEventListener('drop', e => {
  e.preventDefault(); dragDepth = 0; dz.classList.remove('dragover');
  if (busy) return;
  showView('convert');
  const paths = [...e.dataTransfer.files].map(f => window.flowdrop.pathForFile(f)).filter(Boolean);
  addPaths(paths);
});
$('clear').addEventListener('click', clearAll);

/* ── Format + quality ── */
const formatBtns = [...document.querySelectorAll('.format')];
function setFormat(fmt, focus) {
  selectedFormat = fmt;
  formatBtns.forEach(b => { const on = b.dataset.format === fmt; b.setAttribute('aria-checked', on); b.tabIndex = on ? 0 : -1; if (on && focus) b.focus(); });
  updateConvertButton();
}
formatBtns.forEach((b, i) => {
  b.addEventListener('click', () => setFormat(b.dataset.format));
  b.addEventListener('keydown', e => {
    const d = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
    if (!d) return;
    e.preventDefault();
    setFormat(formatBtns[(i + d + formatBtns.length) % formatBtns.length].dataset.format, true);
  });
});
$('quality').value = settings.quality;
$('quality').addEventListener('change', e => { settings.quality = e.target.value; $('setQuality').value = e.target.value; persist(); });

/* ── Convert ── */
$('convert').addEventListener('click', async () => {
  const queue = files.filter(isSupported);
  if (!queue.length || busy) return;
  busy = true;
  const btn = $('convert');
  btn.disabled = true;
  btn.style.setProperty('--p', 0);
  const quality = Number($('quality').value);
  let ok = 0, firstOutput = null, firstError = null;
  queue.forEach(f => { f.status = 'ready'; f.output = null; });
  for (let i = 0; i < queue.length; i++) {
    const f = queue[i];
    f.status = 'working'; renderRow(f);
    $('convertText').textContent = `Converting ${i + 1} of ${queue.length}…`;
    try {
      const [r] = await window.flowdrop.convertImage({ paths: [f.path], format: selectedFormat, quality });
      if (r && r.ok) { f.status = 'done'; f.output = r.output; ok++; firstOutput = firstOutput || r.output; }
      else { f.status = 'error'; f.error = r ? r.error : 'Unknown error'; firstError = firstError || f.error; }
    } catch (err) { f.status = 'error'; f.error = err.message; firstError = firstError || f.error; }
    renderRow(f);
    btn.style.setProperty('--p', ((i + 1) / queue.length) * 100);
  }
  busy = false;
  setTimeout(() => btn.style.setProperty('--p', 0), 400);
  updateConvertButton();
  const failed = queue.length - ok;
  if (ok && !failed) toast(`${ok} file${ok === 1 ? '' : 's'} converted to ${FORMAT_NAMES[selectedFormat]}.`);
  else if (ok) toast(`${ok} converted · ${failed} failed: ${firstError}`, 'error');
  else toast(`Conversion failed: ${firstError}`, 'error');
  if (ok && settings.reveal && firstOutput) window.flowdrop.openFolder(firstOutput);
});

/* ── Settings ── */
const themeBtns = [...document.querySelectorAll('#themeSeg [data-theme]')];
function applyTheme(mode) {
  themeBtns.forEach(b => b.setAttribute('aria-checked', b.dataset.theme === mode));
  window.flowdrop.setTheme(mode);
}
themeBtns.forEach(b => b.addEventListener('click', () => { settings.theme = b.dataset.theme; applyTheme(settings.theme); persist(); }));
$('setFormat').value = selectedFormat;
$('setFormat').addEventListener('change', e => { settings.format = e.target.value; setFormat(e.target.value); persist(); });
$('setQuality').value = settings.quality;
$('setQuality').addEventListener('change', e => { settings.quality = e.target.value; $('quality').value = e.target.value; persist(); });
const reveal = $('setReveal');
reveal.setAttribute('aria-checked', settings.reveal);
reveal.addEventListener('click', () => { settings.reveal = !settings.reveal; reveal.setAttribute('aria-checked', settings.reveal); persist(); });

applyTheme(settings.theme);
setFormat(selectedFormat);
sync();
