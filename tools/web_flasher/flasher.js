// The install page: finds the M8, restarts it into its loader over Web Serial
// (opening the port at 134 baud, Teensyduino's reboot request), then writes
// the firmware over WebHID with halfkay.js. What it writes comes from one of
// three sources: the latest L7 release (firmware/latest.json and the .hex
// files beside it, put there by .github/workflows/web-flasher.yml), a .hex
// from the computer, or Dirtywave's M8Firmware.zip, fetched straight from
// Dirtywave (its address is in latest.json; this site never hosts it).

// The modules load with this script's own ?v=, so a deploy can't pair a new
// page with cached old ones.
const v = new URL(import.meta.url).search;
const HK = await import('./halfkay.js' + v);
const ZIP = await import('./unzip.js' + v);

const $ = (id) => document.getElementById(id);
const els = {
  release: $('release'), unsupported: $('unsupported'), flasher: $('flasher'),
  go: $('go'), pick: $('pick'), l7Actions: $('l7-actions'), restoreActions: $('restore-actions'), status: $('status'), progress: $('progress'),
  console: $('console'), l7Slot: $('l7-slot'),
  log: $('log'), file: $('file'), fileGo: $('file-go'), fileStart: $('file-start'),
  fileActions: $('file-actions'), fileSlot: $('file-slot'),
  downloads: $('downloads'),
  restore: $('restore'), restoreGo: $('restore-go'), restoreSlot: $('restore-slot'),
  dwPage: $('dw-page'), restoreTitle: $('restore-title'),
};

const LOADER_FILTERS = [{ vendorId: HK.VENDOR_ID, productId: HK.LOADER_PRODUCT_ID }];
const DW_PAGE = 'https://dirtywave.com/pages/resources-downloads';
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let release = null;  // firmware/latest.json
let local = null;    // { name, fw, board } when a .hex from the computer is chosen
let awaiting = null; // the source whose M8 is in its loader, waiting for Upload
let busy = false;

function log(msg) {
  const t = new Date().toLocaleTimeString();
  els.log.textContent += `${t}  ${msg}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}

function say(msg, kind = '') {
  els.status.textContent = msg;
  els.status.dataset.kind = kind;
  if (msg) log(msg);
}

// ---- Firmware sources -------------------------------------------------------
// Each: label (before it's fetched), m8Only (refuse non-M8 boards), and
// firmware(board) -> { fw, label } for the board found in the loader.

function describe(board) {
  return board.m8 ? `an M8 ${board.m8} (${board.name})` : `a ${board.name}`;
}

async function sha256(buf) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', buf));
  return [...d].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// A failure with a short code for the install counts
class StepError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

// GoatCounter events, on the page's own site (danielstanciu-l7.goatcounter.com).
// count.js loads after the page and doesn't count on localhost; until it has
// loaded, or if it's blocked, nothing is counted.
function track(path, title = path) {
  try {
    window.goatcounter?.count?.({ path, title: String(title).slice(0, 200), event: true });
  } catch { /* counting is best-effort */ }
}

function failCode(e) {
  if (e instanceof StepError) return e.code;
  if (e instanceof HK.HexError || e instanceof ZIP.ZipError) return 'bad-file';
  if (e?.name === 'NotFoundError') return 'loader-not-chosen';
  if (e?.name === 'SecurityError' || e?.name === 'NotAllowedError') return 'browser-blocked';
  return 'other';
}

let currentModel = null; // the board found in the loader this attempt, for the counts

function checkBuiltFor(fw, board, what) {
  const built = HK.identifyFirmware(fw);
  if (built !== board) throw new StepError('wrong-model', `${what} is built for ${built ? describe(built) : 'something else'}, but this is ${describe(board)}. Nothing was written.`);
}

const releaseSource = {
  get label() { return release ? `L7 ${release.version}` : 'L7'; },
  kind: 'install',
  m8Only: true,
  slot: () => els.l7Slot,
  actions: () => els.l7Actions,
  button: () => els.go,
  async firmware(board) {
    const entry = release?.firmware?.[board.key];
    if (!entry) throw new StepError('no-build', `There's no L7 build for ${describe(board)} in this release.`);
    say(`Downloading ${entry.file}…`);
    const res = await fetch(`firmware/${entry.file}`);
    if (!res.ok) throw new StepError('download', `Couldn't download ${entry.file} (HTTP ${res.status}).`);
    const buf = await res.arrayBuffer();
    if (entry.sha256 && (await sha256(buf)) !== entry.sha256) {
      throw new StepError('download', `${entry.file} didn't download intact. Reload the page and try again.`);
    }
    const fw = HK.parseHex(new TextDecoder().decode(buf));
    checkBuiltFor(fw, board, entry.file);
    return { fw, label: this.label };
  },
};

const localSource = {
  get label() { return local?.name || 'the file'; },
  kind: 'file',
  m8Only: false,
  slot: () => els.fileSlot,
  actions: () => els.fileActions,
  button: () => els.fileGo,
  async firmware(board) {
    checkBuiltFor(local.fw, board, local.name);
    return { fw: local.fw, label: local.name };
  },
};

// Dirtywave's zip is fetched as soon as Restore is clicked, so it downloads
// while the browser's device lists are open.
const dirtywaveSource = {
  version: null, // read from the zip's file list when the page loads
  get label() { return this.version ? `the M8 firmware ${this.version}` : "the M8's own firmware"; },
  kind: 'restore',
  m8Only: true,
  slot: () => els.restoreSlot,
  actions: () => els.restoreActions,
  button: () => els.restoreGo,
  download: null,
  prefetch() {
    const url = release?.dirtywave?.zip;
    if (!url) return;
    this.download = fetch(url, { cache: 'no-cache' }).then(async (res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return res.arrayBuffer();
    });
    this.download.catch(() => {}); // reported when it's awaited
  },
  async firmware(board) {
    if (!this.download) this.prefetch();
    if (!this.download) throw new StepError('download', "The page doesn't know where Dirtywave's firmware is. Reload it and try again.");
    say("Downloading the M8's firmware from Dirtywave…");
    let buf;
    try {
      buf = await this.download;
    } catch (e) {
      this.download = null;
      throw new StepError('download', `Couldn't download the firmware from Dirtywave (${e.message}).`);
    }
    const entry = ZIP.m8FirmwareHex(ZIP.listZip(buf), board.key);
    if (!entry) throw new StepError('zip-no-hex', `Dirtywave's download has no .hex for the M8 ${board.m8}. Nothing was written.`);
    const name = entry.name.split('/').pop();
    log(`Dirtywave zip: ${(buf.byteLength / 1e6).toFixed(1)} MB, using ${entry.name}`);
    const fw = HK.parseHex(new TextDecoder().decode(await ZIP.extractZip(buf, entry)));
    checkBuiltFor(fw, board, name);
    this.version = ZIP.m8FirmwareVersion(name) || this.version;
    return { fw, label: this.label };
  },
};


// ---- Page state --------------------------------------------------------------

// While the M8 waits in its loader, the clicked button turns into an outlined
// Cancel and a filled "Upload <firmware>" goes beside it.
// A chosen .hex (in the fold-out) shows a filled "Install <file>" beside an
// outlined Cancel until it's clicked.
function refresh() {
  const goWaits = awaiting === releaseSource;
  els.go.textContent = goWaits ? 'Cancel' : 'Install';   // the version is in the panel's title
  els.go.classList.toggle('secondary', goWaits);
  els.go.disabled = busy || !release;
  els.restoreGo.textContent = awaiting === dirtywaveSource ? 'Cancel' : 'Restore the M8 firmware';
  els.restoreGo.disabled = busy || !release?.dirtywave?.zip;
  els.fileActions.hidden = !local;
  els.fileGo.disabled = busy;
  els.file.disabled = busy;
  els.fileStart.hidden = !local || awaiting === localSource;
  if (local) els.fileStart.textContent = `Install ${local.name}`;
  els.fileStart.disabled = busy;
  els.pick.hidden = !awaiting;
  if (awaiting) els.pick.textContent = `Upload ${awaiting.label}`;
  els.pick.disabled = busy;
}

function awaitLoader(source) {
  awaiting = source;
  source.actions().prepend(els.pick);
  refresh();
}

// Back to how the page started (the M8 stays in its loader until it's
// switched off and on, or something is uploaded)
function cancel() {
  if (awaiting) track(`${awaiting.kind}-cancel`);
  if (awaiting === localSource || (!awaiting && local)) {
    local = null;
    els.file.value = '';
  }
  awaiting = null;
  els.progress.hidden = true;
  say('');
  refresh();
}

function setBusy(on) {
  busy = on;
  window.onbeforeunload = on ? () => true : null;
  refresh();
}

// The status line and progress bar follow the button that was clicked.
function useSlot(source) {
  const slot = source.slot();
  if (els.console.parentElement !== slot) {
    slot.append(els.console);
    say('');
    els.progress.hidden = true;
    awaiting = null;
  }
}

function fmtDate(iso) {
  try {
    return new Date(iso).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });
  } catch {
    return '';
  }
}

async function loadRelease() {
  try {
    const res = await fetch('firmware/latest.json', { cache: 'no-cache' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    release = await res.json();
  } catch (e) {
    log(`latest.json: ${e.message}`);
    els.release.replaceChildren(Object.assign(document.createElement('span'), {
      textContent: "Couldn't look up the latest release. Reload the page to try again.",
    }));
    els.downloads.innerHTML = '<li><a href="https://github.com/minusro/L7/releases/latest" target="_blank" rel="noopener">The latest release on GitHub</a></li>';
    refresh();
    return;
  }

  const strong = Object.assign(document.createElement('strong'), { textContent: `L7 ${release.version}` });
  const date = Object.assign(document.createElement('span'), { textContent: `Released ${fmtDate(release.published)}` });
  const notes = Object.assign(document.createElement('a'), { href: release.url, target: '_blank', rel: 'noopener', textContent: 'Release notes' });
  els.release.replaceChildren(strong, date, notes);

  els.downloads.replaceChildren();
  for (const board of [HK.BOARDS.teensy40, HK.BOARDS.teensyMM]) {
    const f = release.firmware[board.key];
    const li = document.createElement('li');
    const a = Object.assign(document.createElement('a'), { href: `firmware/${f.file}`, download: f.file, textContent: f.file });
    a.addEventListener('click', () => track(`download-${board.key}`, f.file));
    li.append(`M8 ${board.m8}: `, a);
    els.downloads.append(li);
  }
  els.dwPage.href = release.dirtywave?.page || DW_PAGE;
  refresh();
  loadDirtywaveVersion();
}

// The version of Dirtywave's current firmware, for the Restore title: the
// .hex names in the zip's file list say it, and the file list is at the end
// of the zip, so only the last 64 KB are fetched (a plain "bytes=N-" Range,
// which needs no CORS preflight; the zip's host refuses preflights).
async function loadDirtywaveVersion() {
  const url = release?.dirtywave?.zip;
  if (!url) return;
  try {
    const head = await fetch(url, { method: 'HEAD', cache: 'no-cache' });
    const size = Number(head.headers.get('content-length'));
    if (!head.ok || !size) throw new Error(`HEAD ${head.status}, size ${size}`);
    const start = Math.max(0, size - 65536);
    const res = await fetch(url, { headers: { Range: `bytes=${start}-` }, cache: 'no-cache' });
    if (res.status !== 206 && res.status !== 200) throw new Error(`HTTP ${res.status}`);
    const entries = ZIP.listZip(await res.arrayBuffer(), res.status === 206 ? start : 0);
    const hex = ZIP.m8FirmwareHex(entries, 'model01') || ZIP.m8FirmwareHex(entries, 'model02');
    const version = hex && ZIP.m8FirmwareVersion(hex.name);
    if (!version) throw new Error('no version in the file names');
    dirtywaveSource.version = version;
    els.restoreTitle.textContent = `Go back to the M8's own firmware (${version})`;
    log(`Dirtywave firmware: ${version}`);
    refresh();
  } catch (e) {
    log(`Dirtywave firmware version: ${e.message}`);
  }
}

// ---- The M8 ------------------------------------------------------------------

async function grantedLoader() {
  const devices = await navigator.hid.getDevices();
  return devices.find((d) => d.vendorId === HK.VENDOR_ID && d.productId === HK.LOADER_PRODUCT_ID) || null;
}

async function waitForGrantedLoader(ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const d = await grantedLoader();
    if (d) return d;
    await sleep(250);
  }
  return null;
}

// The running M8's serial port: one the browser already allows this page
// (from an earlier install) when there's exactly one, so no list is needed;
// otherwise the browser's list. null if the person closed the list.
async function m8Port() {
  if (!('serial' in navigator)) return null;
  const known = (await navigator.serial.getPorts()).filter((p) => p.getInfo().usbVendorId === HK.VENDOR_ID);
  if (known.length === 1) return known[0];
  say('Choose your M8 in the pop-up. The page restarts it into its loader.');
  try {
    return await navigator.serial.requestPort({ filters: [{ usbVendorId: HK.VENDOR_ID }] });
  } catch (e) {
    if (e.name === 'NotFoundError') return null;
    throw e;
  }
}

// Opening the port at 134 baud asks Teensyduino's USB code to reboot into
// the loader.
async function restartIntoLoader(port) {
  try {
    await port.open({ baudRate: 134 });
  } catch (e) {
    log(`serial open: ${e.message}`);
    throw new StepError('port-busy', "Couldn't open the M8's USB port. Another app is probably using it: quit M8C, Teensy Loader or any serial monitor, then try again.");
  }
  await sleep(200);
  try { await port.close(); } catch { /* it has usually gone by now */ }
}

// The loader, once the M8 has restarted into it: one the browser already
// allows, or, while the click that started this still counts as one (Chrome
// gives it about 5 s), the browser's list straight away, which shows the
// loader as soon as it appears. null if neither: then the Upload button asks.
// Waiting for an allowed loader costs time from that click, so the page only
// waits long when this browser has allowed the loader before.
const LOADER_ALLOWED = 'l7-flasher.loader-allowed';
function rememberLoaderAllowed() {
  try { localStorage.setItem(LOADER_ALLOWED, '1'); } catch { /* storage off: just waits less */ }
}
function loaderAllowedBefore() {
  try { return localStorage.getItem(LOADER_ALLOWED) === '1'; } catch { return false; }
}

async function chooseLoader() {
  const [device] = await navigator.hid.requestDevice({ filters: LOADER_FILTERS });
  if (device) rememberLoaderAllowed();
  return device || null;
}

async function findLoader() {
  const known = await waitForGrantedLoader(loaderAllowedBefore() ? 3000 : 300);
  if (known) return known;
  if (!navigator.userActivation?.isActive) return null;
  say("Choose the M8's loader in the pop-up. It appears a moment after the M8 restarts.");
  try {
    return await chooseLoader();
  } catch (e) {
    log(`loader list: ${e.name}: ${e.message}`);
    return null;
  }
}

async function flash(device, source) {
  if (!device.opened) await device.open();
  const col = device.collections.find((c) => c.usagePage === HK.LOADER_USAGE_PAGE);
  const board = col ? HK.boardFromUsage(col.usage) : null;
  const outBits = col?.outputReports?.[0]?.items?.reduce((n, i) => n + i.reportSize * i.reportCount, 0);
  log(`loader: usage 0x${col?.usage?.toString(16)}, output report ${outBits / 8} bytes`);
  if (!board) {
    throw new StepError('unknown-board', "This loader belongs to a board the page doesn't know. It installs on the M8 Model:01 and Model:02 only.");
  }
  if (source.m8Only && !board.m8) {
    throw new StepError('not-m8', `That's a ${board.name}, not an M8. This installs on the M8 Model:01 and Model:02.`);
  }
  currentModel = board.m8 ? board.key : board.id;
  say(`Found ${describe(board)}.`);

  const { fw, label } = await source.firmware(board);
  if (fw.size > board.codeSize) throw new StepError('too-big', `The firmware is too big for a ${board.name}.`);

  const abort = new AbortController();
  const onGone = (e) => { if (e.device === device) abort.abort(); };
  navigator.hid.addEventListener('disconnect', onGone);
  const send = (report) => device.sendReport(0, report);
  try {
    els.progress.hidden = false;
    els.progress.value = 0;
    say("Erasing the M8's flash. This takes a few seconds…");
    const started = Date.now();
    let blocks;
    try {
      blocks = await HK.upload(fw, send, {
        signal: abort.signal,
        onProgress: (n, total) => {
          els.progress.value = n / total;
          const pct = Math.round((100 * n) / total);
          els.status.textContent = pct ? `Installing ${label}… ${pct}%` : `Installing ${label}…`;
          els.status.dataset.kind = '';
        },
      });
    } catch (e) {
      throw new StepError(abort.signal.aborted ? 'disconnected' : 'write', e.message);
    }
    log(`wrote ${blocks} blocks in ${((Date.now() - started) / 1000).toFixed(1)} s`);
    try {
      await HK.boot(send);
    } catch (e) {
      log(`restart: ${e.message} (the M8 usually restarts anyway)`);
    }
  } finally {
    navigator.hid.removeEventListener('disconnect', onGone);
    try { await device.close(); } catch { /* gone into the new firmware */ }
  }
  awaiting = null;
  track(`${source.kind}-done-${currentModel}`, `${label} on ${describe(board)}`);
  say(`Done. ${label[0].toUpperCase()}${label.slice(1)} is starting on your M8. If the screen stays dark, switch the M8 off and on.`, 'ok');
}

function fail(e, source, inLoader = true) {
  console.error(e);
  track(`${source.kind}-fail-${failCode(e)}${currentModel ? `-${currentModel}` : ''}`, e?.message || e);
  if (inLoader) awaitLoader(source);
  if (e?.name === 'NotFoundError') {
    say(`No loader chosen. Click "Upload ${source.label}" when the M8 is in its loader.`);
  } else if (e?.name === 'SecurityError' || e?.name === 'NotAllowedError') {
    say(`The browser blocked access to the M8 (${e.message}). Click the button again.`, 'error');
  } else {
    const fileProblem = e instanceof HK.HexError || e instanceof ZIP.ZipError || /Nothing was written/.test(e?.message);
    say(`${e?.message || e}${fileProblem ? '' : " It's safe to try again."}`, 'error');
  }
}

// One click: restart the M8 into its loader and install. The browser shows
// a list for each device the page hasn't been allowed before (the M8, then
// its loader), so a first install shows two and later ones none; if the
// second list can't open by itself, the Upload button opens it.
async function start(source) {
  if (busy) return;
  useSlot(source);
  awaiting = null;
  currentModel = null;
  setBusy(true);
  track(`${source.kind}-start`, `Clicked: ${source.label}`);
  let inLoader = false;
  try {
    let device = await grantedLoader();
    if (!device) {
      const port = await m8Port();
      if (port) {
        await restartIntoLoader(port);
        inLoader = true;
        say('Restarting the M8 into its loader…');
      }
      device = port ? await findLoader() : null;
      if (!device) {
        awaitLoader(source);
        track(port ? `${source.kind}-upload-button` : `${source.kind}-no-m8-chosen`);
        say(port
          ? `The M8 is in its loader. Click "Upload ${source.label}" and choose the loader in the pop-up.`
          : `No M8 chosen. If it's already in its loader, click "Upload ${source.label}".`);
        return;
      }
    }
    inLoader = true;
    await flash(device, source);
  } catch (e) {
    fail(e, source, inLoader);
  } finally {
    setBusy(false);
  }
}

els.go.addEventListener('click', () => {
  if (busy) return;
  if (awaiting === releaseSource) cancel();
  else start(releaseSource);
});

els.restoreGo.addEventListener('click', () => {
  if (busy) return;
  if (awaiting === dirtywaveSource) return cancel();
  dirtywaveSource.prefetch();
  start(dirtywaveSource);
});

els.pick.addEventListener('click', async () => {
  if (busy) return;
  const source = awaiting || releaseSource;
  currentModel = null;
  setBusy(true);
  try {
    const device = await chooseLoader();
    if (!device) {
      say('No loader chosen. If the pop-up was empty, the M8 isn\'t in its loader: click Cancel, then Install again.');
      return;
    }
    await flash(device, source);
  } catch (e) {
    fail(e, source);
  } finally {
    setBusy(false);
  }
});

els.file.addEventListener('change', async () => {
  const file = els.file.files[0];
  if (!file) return;
  useSlot(localSource);
  try {
    const fw = HK.parseHex(await file.text());
    const board = HK.identifyFirmware(fw);
    if (!board) throw new HK.HexError(`${file.name} isn't firmware for an M8.`);
    local = { name: file.name, fw, board };
    say(`${file.name} is built for ${describe(board)}.`);
  } catch (e) {
    local = null;
    els.file.value = '';
    say(e.message, 'error');
  }
  refresh();
});

// Cancel: forget the file (and stop waiting for its loader)
els.fileGo.addEventListener('click', () => {
  if (!busy) cancel();
});

els.fileStart.addEventListener('click', () => start(localSource));

if ('hid' in navigator && window.isSecureContext) {
  els.flasher.hidden = false;
  els.restore.hidden = false;
  navigator.hid.addEventListener('connect', (e) => {
    if (e.device.productId === HK.LOADER_PRODUCT_ID) log('loader connected');
  });
} else {
  els.unsupported.hidden = false;
  // count.js may not have loaded yet
  window.addEventListener('load', () => setTimeout(() => track('unsupported-browser', 'Browser without WebHID'), 1000));
}
refresh();
loadRelease();
