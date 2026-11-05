// The install page: finds the M8, restarts it into its loader over Web Serial
// (opening the port at 134 baud, Teensyduino's reboot request), then writes
// the firmware over WebHID with halfkay.js. firmware/latest.json and the .hex
// files beside it are put there by .github/workflows/web-flasher.yml.

// halfkay.js loads with this script's own ?v=, so a deploy can't pair a new
// page with a cached old module.
const HK = await import('./halfkay.js' + new URL(import.meta.url).search);

const $ = (id) => document.getElementById(id);
const els = {
  release: $('release'), unsupported: $('unsupported'), flasher: $('flasher'),
  go: $('go'), pick: $('pick'), status: $('status'), progress: $('progress'),
  log: $('log'), file: $('file'), fileClear: $('file-clear'), fileDetails: $('file-details'),
  downloads: $('downloads'), guide: $('guide'), cheatsheet: $('cheatsheet'),
};

const LOADER_FILTERS = [{ vendorId: HK.VENDOR_ID, productId: HK.LOADER_PRODUCT_ID }];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let release = null;  // firmware/latest.json
let local = null;    // { name, fw, board } when a .hex from the computer is chosen
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

function label() {
  return local ? local.name : release ? `L7 ${release.version}` : 'L7';
}

function refresh() {
  els.go.textContent = `Install ${label()}`;
  els.go.disabled = busy || !(release || local);
  els.pick.disabled = busy;
  els.file.disabled = busy;
  els.fileClear.hidden = !local;
}

function setBusy(on) {
  busy = on;
  window.onbeforeunload = on ? () => true : null;
  refresh();
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
    els.release.innerHTML = '';
    els.release.append(Object.assign(document.createElement('span'), {
      textContent: "Couldn't look up the latest release. Reload the page to try again.",
    }));
    els.downloads.innerHTML = '<li><a href="https://github.com/minusro/L7/releases/latest">The latest release on GitHub</a></li>';
    refresh();
    return;
  }

  els.release.innerHTML = '';
  const strong = Object.assign(document.createElement('strong'), { textContent: `L7 ${release.version}` });
  const date = Object.assign(document.createElement('span'), { textContent: `Released ${fmtDate(release.published)}` });
  const notes = Object.assign(document.createElement('a'), { href: release.url, textContent: 'Release notes' });
  els.release.append(strong, date, notes);

  els.downloads.innerHTML = '';
  for (const board of [HK.BOARDS.teensy40, HK.BOARDS.teensyMM]) {
    const f = release.firmware[board.key];
    const li = document.createElement('li');
    const a = Object.assign(document.createElement('a'), { href: `firmware/${f.file}`, download: f.file, textContent: f.file });
    li.append(`M8 ${board.m8}: `, a);
    els.downloads.append(li);
  }
  if (release.docs?.guide) els.guide.href = release.docs.guide;
  if (release.docs?.cheatsheet) els.cheatsheet.href = release.docs.cheatsheet;
  refresh();
}

async function sha256(buf) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', buf));
  return [...d].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// The hosted firmware for this board, checked against latest.json.
async function releaseFirmware(board) {
  const entry = release?.firmware?.[board.key];
  if (!entry) throw new Error(`There's no L7 build for the ${board.name} in this release.`);
  say(`Downloading ${entry.file}…`);
  const res = await fetch(`firmware/${entry.file}`);
  if (!res.ok) throw new Error(`Couldn't download ${entry.file} (HTTP ${res.status}).`);
  const buf = await res.arrayBuffer();
  if (entry.sha256 && (await sha256(buf)) !== entry.sha256) {
    throw new Error(`${entry.file} didn't download intact. Reload the page and try again.`);
  }
  const fw = HK.parseHex(new TextDecoder().decode(buf));
  const built = HK.identifyFirmware(fw);
  if (built !== board) throw new Error(`${entry.file} is built for a ${built?.name || 'different board'}, not a ${board.name}.`);
  return fw;
}

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

// "an M8 Model:02 (Teensy MicroMod)", "a Teensy 4.1"
function describe(board) {
  return board.m8 ? `an M8 ${board.m8} (${board.name})` : `a ${board.name}`;
}

async function flash(device) {
  if (!device.opened) await device.open();
  const col = device.collections.find((c) => c.usagePage === HK.LOADER_USAGE_PAGE);
  const board = col ? HK.boardFromUsage(col.usage) : null;
  const outBits = col?.outputReports?.[0]?.items?.reduce((n, i) => n + i.reportSize * i.reportCount, 0);
  log(`loader: usage 0x${col?.usage?.toString(16)}, output report ${outBits / 8} bytes`);
  if (!board) {
    throw new Error("This loader belongs to a board the page doesn't know. It installs on the M8 Model:01 and Model:02 only.");
  }
  if (!local && !board.m8) {
    throw new Error(`That's a ${board.name}, not an M8. L7 runs on the M8 Model:01 and Model:02.`);
  }
  say(`Found ${describe(board)}.`);

  let fw;
  if (local) {
    if (local.board !== board) {
      throw new Error(`${local.name} is for ${describe(local.board)}, but this is ${describe(board)}. Nothing was written.`);
    }
    fw = local.fw;
  } else {
    fw = await releaseFirmware(board);
  }
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
        els.status.textContent = `Installing ${label()}… ${Math.round((100 * n) / total)}%`;
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
  say(`Done. ${label()} is starting on your M8. If the screen stays dark, switch the M8 off and on.`, 'ok');
}

function fail(e) {
  console.error(e);
  if (e?.name === 'NotFoundError') {
    say('No loader chosen. Click "Select the loader" when the M8 is in its loader.');
  } else if (e?.name === 'SecurityError' || e?.name === 'NotAllowedError') {
    say(`The browser blocked access to the M8 (${e.message}). Click the button again.`, 'error');
  } else {
    say(`${e?.message || e}${e instanceof HK.HexError ? '' : " It's safe to try again."}`, 'error');
  }
  els.pick.hidden = false;
}

els.go.addEventListener('click', async () => {
  if (busy) return;
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
    await flash(device);
  } catch (e) {
    fail(e);
  } finally {
    setBusy(false);
  }
});

els.pick.addEventListener('click', async () => {
  if (busy) return;
  setBusy(true);
  try {
    const [device] = await navigator.hid.requestDevice({ filters: LOADER_FILTERS });
    if (!device) {
      say('No loader chosen. If the list was empty, the M8 isn\'t in its loader yet: click Install.');
      return;
    }
    await flash(device);
  } catch (e) {
    fail(e);
  } finally {
    setBusy(false);
  }
});

els.file.addEventListener('change', async () => {
  const file = els.file.files[0];
  if (!file) return;
  try {
    const fw = HK.parseHex(await file.text());
    const board = HK.identifyFirmware(fw);
    if (!board) throw new HK.HexError(`${file.name} isn't firmware for an M8.`);
    local = { name: file.name, fw, board };
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
  els.file.value = '';
  say('');
  refresh();
});

if ('hid' in navigator && window.isSecureContext) {
  els.flasher.hidden = false;
  navigator.hid.addEventListener('connect', (e) => {
    if (e.device.productId === HK.LOADER_PRODUCT_ID) log('loader connected');
  });
} else {
  els.unsupported.hidden = false;
}
refresh();
loadRelease();
