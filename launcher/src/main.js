import './style.css';
import { invoke } from '@tauri-apps/api/core';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { open } from '@tauri-apps/plugin-dialog';
import { BUSY_PHASES, PHASE_LABEL, primaryAction } from './action.js';

const PAGES = [
  ['home', 'Home'],
  ['setup', 'Set up'],
  ['settings', 'Settings'],
  ['about', 'About'],
];
const HEIGHTS = [['0', 'Match the window'], ['720', '720p'], ['1080', '1080p'], ['1440', '1440p'], ['2160', '4K']];

const $ = (sel) => document.querySelector(sel);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
let status = null;
let page = 'home';
let drawn = '';

function toast(msg) {
  const el = $('#toast');
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => el.classList.remove('show'), 6000);
}

async function call(cmd, args) {
  try {
    await invoke(cmd, args);
  } catch (e) {
    toast(String(e));
  }
  await refresh();
}

const actions = {
  async pick() {
    const dir = await open({ directory: true, title: 'Folder with your extracted exefs and romfs' });
    if (dir) await call('set_game_dir', { path: dir });
  },
  setup: () => call('start_setup'),
  cancel: () => call('cancel_setup'),
  play: () => call('play'),
  'go-setup': () => go('setup'),
  'open-work': () => call('open_path', { which: 'work' }),
  'open-setup-log': () => call('open_path', { which: 'setup_log' }),
  'open-game-log': () => call('open_path', { which: 'game_log' }),
};

const busy = (s) => BUSY_PHASES.includes(s.phase);

function progress(s) {
  if (!busy(s)) return '';
  const pct = s.progress == null ? null : Math.min(100, Math.round(s.progress * 100));
  return `<div class="task glass">
    <span class="task-msg">${esc(PHASE_LABEL[s.phase])}</span>
    <span class="task-pct">${pct == null ? '' : `${pct}%`}</span>
    <progress max="100" ${pct == null ? '' : `value="${pct}"`}></progress>
    <button class="btn sm" data-act="cancel">Cancel</button>
  </div>`;
}

const views = {
  home(s) {
    const a = primaryAction(s);
    return `<div class="page home">
      <h1 class="wordmark"><span>Forgotten Land</span><small>Recompiled</small></h1>
      <div class="home-actions">
        <button class="btn primary big" data-act="${a.act ?? ''}" ${a.act ? '' : 'disabled'} aria-busy="${!!a.busy}">${esc(a.label)}</button>
      </div>
      ${a.detail ? `<p class="detail glass" role="alert">${esc(a.detail)}</p>` : ''}
      ${progress(s)}
      <p class="fine">Bring your own copy of the game. Nothing here contains or shares game files.</p>
    </div>`;
  },
  setup(s) {
    const step = (n, title, ok, body) => `<section class="card glass">
      <h2><span class="step ${ok ? 'ok' : ''}">${ok ? '&#10003;' : n}</span>${title}</h2>${body}</section>`;
    return `<div class="page">
      <h1>Set up</h1>
      ${step(1, 'Game files', s.game_dir && !s.game_error, `
        <p>${s.game_dir ? `<code>${esc(s.game_dir)}</code>` : 'Pick the folder that holds the <code>exefs</code> and <code>romfs</code> you extracted from your own copy.'}</p>
        ${s.game_error ? `<p class="bad" role="alert">${esc(s.game_error)}</p>` : ''}
        <button class="btn" data-act="pick" ${busy(s) ? 'disabled' : ''}>Select game folder</button>`)}
      ${step(2, 'Build tools', !s.tools_missing.length, s.tools_missing.length
        ? `<p class="bad" role="alert">Missing: ${esc(s.tools_missing.join(', '))}. See the README for the install commands.</p>`
        : '<p>MSYS2 clang, make and the Python environment are installed.</p>')}
      ${step(3, 'Game code', s.built, `
        <p>${s.built ? 'The game code is built for this PC.' : 'Builds the runtime, writes a game data image of about 6 GB next to the romfs folder, then translates the game code from your files and compiles it for this PC. About 10 minutes on a 16-thread CPU.'}</p>
        ${s.phase === 'failed' ? `<p class="bad" role="alert">${esc(s.error)}</p>` : ''}
        ${progress(s)}
        <div class="row">
          <button class="btn ${s.built ? '' : 'primary'}" data-act="setup" ${busy(s) || !s.game_dir || s.game_error || s.tools_missing.length ? 'disabled' : ''}>${s.built ? 'Rebuild' : 'Build'}</button>
          <button class="btn quiet" data-act="open-setup-log">Open setup log</button>
        </div>`)}
    </div>`;
  },
  settings(s) {
    const v = Object.fromEntries(s.settings);
    return `<div class="page">
      <h1>Settings</h1>
      <section class="card glass">
        <label class="field"><span>Fullscreen</span>
          <input type="checkbox" data-key="fullscreen" ${v.fullscreen === '1' ? 'checked' : ''} /></label>
        <label class="field"><span>Render resolution</span>
          <select data-key="render_height">${HEIGHTS.map(([val, name]) => `<option value="${val}" ${(v.render_height || '0') === val ? 'selected' : ''}>${name}</option>`).join('')}</select></label>
        <label class="field"><span>Volume</span>
          <input type="range" min="0" max="1" step="0.05" data-key="volume" value="${esc(v.volume || '1')}" /></label>
      </section>
      <p class="fine">Changes apply the next time the game starts.</p>
    </div>`;
  },
  about(s) {
    return `<div class="page">
      <h1>About</h1>
      <section class="card glass">
        <p>A static recompilation of the game's code into a native program, built on your PC from your own copy. It is not an emulator and it ships no game files, keys or dumping tools.</p>
        <p>Built on letsgo-recomp and LAS Recomp (MIT). Not affiliated with Nintendo or HAL Laboratory.</p>
        <p>Work folder: <code>${esc(s.work)}</code></p>
        <div class="row">
          <button class="btn" data-act="open-work">Open work folder</button>
          <button class="btn quiet" data-act="open-game-log">Open game log</button>
        </div>
      </section>
    </div>`;
  },
};

function draw() {
  if (!status) return;
  const html = views[page](status);
  // status arrives twice a second: rewrite only on change, so focus and open selects survive
  if (html === drawn) return;
  drawn = html;
  $('#view').innerHTML = html;
}

// built once: rebuilding it on every page change would drop the keyboard focus
$('#nav').innerHTML = `<div class="brand">Forgotten Land<small>Recompiled</small></div>${PAGES.map(
  ([id, name]) => `<button class="nav-item" data-page="${id}">${name}</button>`,
).join('')}`;

function go(to) {
  page = to;
  document.body.dataset.page = to;
  for (const b of document.querySelectorAll('.nav-item')) {
    if (b.dataset.page === to) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  drawn = '';
  draw();
}

async function refresh() {
  try {
    status = await invoke('status');
  } catch (e) {
    toast(String(e));
  }
  draw();
}

document.addEventListener('click', (e) => {
  const nav = e.target.closest('.nav-item');
  if (nav) return go(nav.dataset.page);
  const act = e.target.closest('[data-act]')?.dataset.act;
  if (act && actions[act]) actions[act]();
});
document.addEventListener('change', (e) => {
  const key = e.target.dataset?.key;
  if (!key) return;
  const value = e.target.type === 'checkbox' ? (e.target.checked ? '1' : '0') : e.target.value;
  call('set_setting', { key, value });
});
$('#win-min').addEventListener('click', () => getCurrentWindow().minimize());
$('#win-close').addEventListener('click', () => getCurrentWindow().close());

go('home');
refresh();
setInterval(refresh, 500);
