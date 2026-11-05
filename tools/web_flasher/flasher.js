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
  go: $('go'), pick: $('pick'), status: $('status'), progress: $('progress'),
  console: $('console'), l7Slot: $('l7-slot'),
  log: $('log'), file: $('file'), fileClear: $('file-clear'),
  downloads: $('downloads'), guide: $('guide'), cheatsheet: $('cheatsheet'),
  restore: $('restore'), restoreGo: $('restore-go'), restoreSlot: $('restore-slot'),
  dwPage: $('dw-page'),
};

const LOADER_FILTERS = [{ vendorId: HK.VENDOR_ID, productId: HK.LOADER_PRODUCT_ID }];
const DW_PAGE = 'https://dirtywave.com/pages/resources-downloads';
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let release = null;  // firmware/latest.json
let local = null;    // { name, fw, board } when a .hex from the computer is chosen
let pending = null;  // the source waiting for "Select the loader"
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

function checkBuiltFor(fw, board, what) {
  const built = HK.identifyFirmware(fw);
  if (built !== board) throw new Error(`${what} is built for ${built ? describe(built) : 'something else'}, but this is ${describe(board)}. Nothing was written.`);
}

const releaseSource = {
  get label() { return release ? `L7 ${release.version}` : 'L7'; },
  m8Only: true,
  slot: () => els.l7Slot,
  async firmware(board) {
    const entry = release?.firmware?.[board.key];
    if (!entry) throw new Error(`There's no L7 build for ${describe(board)} in this release.`);
    say(`Downloading ${entry.file}…`);
    const res = await fetch(`firmware/${entry.file}`);
    if (!res.ok) throw new Error(`Couldn't download ${entry.file} (HTTP ${res.status}).`);
    const buf = await res.arrayBuffer();
    if (entry.sha256 && (await sha256(buf)) !== entry.sha256) {
      throw new Error(`${entry.file} didn't download intact. Reload the page and try again.`);
    }
    const fw = HK.parseHex(new TextDecoder().decode(buf));
    checkBuiltFor(fw, board, entry.file);
    return { fw, label: this.label };
  },
};

const localSource = {
  get label() { return local?.name || 'the file'; },
  m8Only: false,
  slot: () => els.l7Slot,
  async firmware(board) {
    checkBuiltFor(local.fw, board, local.name);
    return { fw: local.fw, label: local.name };
  },
};

// Dirtywave's zip is fetched as soon as Restore is clicked, so it downloads
// while the browser's device lists are open.
const dirtywaveSource = {
  label: "the M8's own firmware",
  m8Only: true,
  slot: () => els.restoreSlot,
  download: null,
  start() {
    const url = release?.dirtywave?.zip;
    if (!url) return;
    this.download = fetch(url, { cache: 'no-cache' }).then(async (res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return res.arrayBuffer();
    });
    this.download.catch(() => {}); // reported when it's awaited
  },
  async firmware(board) {
    if (!this.download) this.start();
    if (!this.download) throw new Error("The page doesn't know where Dirtywave's firmware is. Reload it and try again.");
    say("Downloading the M8's firmware from Dirtywave…");
    let buf;
    try {
      buf = await this.download;
    } catch (e) {
      this.download = null;
      throw new Error(`Couldn't download the firmware from Dirtywave (${e.message}).`);
    }
    const entry = ZIP.m8FirmwareHex(ZIP.listZip(buf), board.key);
    if (!entry) throw new Error(`Dirtywave's download has no .hex for the M8 ${board.m8}. Nothing was written.`);
    const name = entry.name.split('/').pop();
    log(`Dirtywave zip: ${(buf.byteLength / 1e6).toFixed(1)} MB, using ${entry.name}`);
    const fw = HK.parseHex(new TextDecoder().decode(await ZIP.extractZip(buf, entry)));
    checkBuiltFor(fw, board, name);
    const version = ZIP.m8FirmwareVersion(name);
    return { fw, label: version ? `the M8 firmware ${version}` : "the M8's own firmware" };
  },
};

const mainSource = () => (local ? localSource : releaseSource);

// ---- Page state --------------------------------------------------------------

function refresh() {
  els.go.textContent = `Install ${mainSource().label}`;
  els.go.disabled = busy || !(release || local);
  els.restoreGo.disabled = busy || !release?.dirtywave?.zip;
  els.pick.disabled = busy;
  els.file.disabled = busy;
  els.fileClear.hidden = !local;
}

function setBusy(on) {
  busy = on;
  window.onbeforeunload = on ? () => true : null;
  refresh();
}

// The status line, progress bar and "Select the loader" follow the button
// that was clicked.
function useSlot(source) {
  const slot = source.slot();
  if (els.console.parentElement !== slot) {
    slot.append(els.console);
    say('');
    els.progress.hidden = true;
    els.pick.hidden = true;
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
    els.downloads.innerHTML = '<li><a href="https://github.com/minusro/L7/releases/latest">The latest release on GitHub</a></li>';
    refresh();
    return;
  }

  const strong = Object.assign(document.createElement('strong'), { textContent: `L7 ${release.version}` });
  const date = Object.assign(document.createElement('span'), { textContent: `Released ${fmtDate(release.published)}` });
  const notes = Object.assign(document.createElement('a'), { href: release.url, textContent: 'Release notes' });
  els.release.replaceChildren(strong, date, notes);

  els.downloads.replaceChildren();
  for (const board of [HK.BOARDS.teensy40, HK.BOARDS.teensyMM]) {
    const f = release.firmware[board.key];
    const li = document.createElement('li');
    const a = Object.assign(document.createElement('a'), { href: `firmware/${f.file}`, download: f.file, textContent: f.file });
    li.append(`M8 ${board.m8}: `, a);
    els.downloads.append(li);
  }
  if (release.docs?.guide) els.guide.href = release.docs.guide;
  if (release.docs?.cheatsheet) els.cheatsheet.href = release.docs.cheatsheet;
  els.dwPage.href = release.dirtywave?.page || DW_PAGE;
  refresh();
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

// Asks for the running M8's serial port and restarts it into the loader.
// false if the person closed the list without choosing.
async function restartIntoLoader() {
  if (!('serial' in navigator)) return false;
  let port;
  try {
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: HK.VENDOR_ID }] });
  } catch (e) {
    if (e.name === 'NotFoundError') return false;
    throw e;
  }
  try {
    await port.open({ baudRate: 134 });
  } catch (e) {
    log(`serial open: ${e.message}`);
    throw new Error("Couldn't open the M8's USB port. Another app is probably using it: quit M8C, Teensy Loader or any serial monitor, then try again.");
  }
  await sleep(200);
  try { await port.close(); } catch { /* it has usually gone by now */ }
  return true;
}

async function flash(device, source) {
  if (!device.opened) await device.open();
  const col = device.collections.find((c) => c.usagePage === HK.LOADER_USAGE_PAGE);
  const board = col ? HK.boardFromUsage(col.usage) : null;
  const outBits = col?.outputReports?.[0]?.items?.reduce((n, i) => n + i.reportSize * i.reportCount, 0);
  log(`loader: usage 0x${col?.usage?.toString(16)}, output report ${outBits / 8} bytes`);
  if (!board) {
    throw new Error("This loader belongs to a board the page doesn't know. It installs on the M8 Model:01 and Model:02 only.");
  }
  if (source.m8Only && !board.m8) {
    throw new Error(`That's a ${board.name}, not an M8. This installs on the M8 Model:01 and Model:02.`);
  }
  say(`Found ${describe(board)}.`);

  const { fw, label } = await source.firmware(board);
  if (fw.size > board.codeSize) throw new Error(`The firmware is too big for a ${board.name}.`);

  const abort = new AbortController();
  const onGone = (e) => { if (e.device === device) abort.abort(); };
  navigator.hid.addEventListener('disconnect', onGone);
  const send = (report) => device.sendReport(0, report);
  try {
    els.progress.hidden = false;
    els.progress.value = 0;
    say("Erasing the M8's flash. This takes a few seconds…");
    const started = Date.now();
    const blocks = await HK.upload(fw, send, {
      signal: abort.signal,
      onProgress: (n, total) => {
        els.progress.value = n / total;
        els.status.textContent = `Installing ${label}… ${Math.round((100 * n) / total)}%`;
        els.status.dataset.kind = '';
      },
    });
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
  els.pick.hidden = true;
  pending = null;
  say(`Done. ${label[0].toUpperCase()}${label.slice(1)} is starting on your M8. If the screen stays dark, switch the M8 off and on.`, 'ok');
}

function fail(e) {
  console.error(e);
  if (e?.name === 'NotFoundError') {
    say('No loader chosen. Click "Select the loader" when the M8 is in its loader.');
  } else if (e?.name === 'SecurityError' || e?.name === 'NotAllowedError') {
    say(`The browser blocked access to the M8 (${e.message}). Click the button again.`, 'error');
  } else {
    const fileProblem = e instanceof HK.HexError || e instanceof ZIP.ZipError || /Nothing was written/.test(e?.message);
    say(`${e?.message || e}${fileProblem ? '' : " It's safe to try again."}`, 'error');
  }
  els.pick.hidden = false;
}

async function start(source) {
  if (busy) return;
  useSlot(source);
  pending = source;
  setBusy(true);
  try {
    let device = await grantedLoader();
    if (!device) {
      say('Choose your M8 in the list. The page restarts it into its loader.');
      const restarted = await restartIntoLoader();
      if (restarted) {
        say('Restarting the M8 into its loader…');
        device = await waitForGrantedLoader(4000);
      }
      if (!device) {
        els.pick.hidden = false;
        say(restarted
          ? 'The M8 is in its loader. Click "Select the loader" and choose it in the list.'
          : 'No M8 chosen. If it\'s already in its loader, click "Select the loader".');
        return;
      }
    }
    await flash(device, source);
  } catch (e) {
    fail(e);
  } finally {
    setBusy(false);
  }
}

els.go.addEventListener('click', () => start(mainSource()));

els.restoreGo.addEventListener('click', () => {
  if (busy) return;
  dirtywaveSource.start();
  start(dirtywaveSource);
});

els.pick.addEventListener('click', async () => {
  if (busy) return;
  const source = pending || mainSource();
  setBusy(true);
  try {
    const [device] = await navigator.hid.requestDevice({ filters: LOADER_FILTERS });
    if (!device) {
      say('No loader chosen. If the list was empty, the M8 isn\'t in its loader yet: start again from the Install button.');
      return;
    }
    await flash(device, source);
  } catch (e) {
    fail(e);
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
    pending = null;
    say(`Ready to install ${file.name}, built for ${describe(board)}.`);
  } catch (e) {
    local = null;
    els.file.value = '';
    say(e.message, 'error');
  }
  refresh();
});

els.fileClear.addEventListener('click', () => {
  local = null;
  pending = null;
  els.file.value = '';
  say('');
  refresh();
});

if ('hid' in navigator && window.isSecureContext) {
  els.flasher.hidden = false;
  els.restore.hidden = false;
  navigator.hid.addEventListener('connect', (e) => {
    if (e.device.productId === HK.LOADER_PRODUCT_ID) log('loader connected');
  });
} else {
  els.unsupported.hidden = false;
}
refresh();
loadRelease();
