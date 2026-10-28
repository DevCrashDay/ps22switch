#!/usr/bin/env node
// Frames per second of the browser version of a game in Microsoft Edge, in the same scenes as scripts/speedcheck.py (Windows).
//
//   node scripts/webspeed.mjs --scene hall --scene fight2 --scene arena --look 3 --repeat 2
//
// What it does: it starts its own Edge (a fresh profile in a temp folder, never your own browser), opens the page that
// scripts/serve.py serves on 127.0.0.1, loads the scene's save state through the page's ?state= parameter, presses the
// scene's buttons through ?press=, waits for the warmup, then counts the emulated frames for the measured time. One JSON line
// per run goes to <out>/web-speed.jsonl. It closes only the Edge it started (by process id).
//
// Start the server first (scripts/serve.py build/web-gow2 isos/gow2.iso). The scene states are hard-linked from
// <root>/games/gow2/play into <build>/states by the script (git-ignored build folder, never served beyond 127.0.0.1).
// Standard Node only (version 22 or later: it has WebSocket and fetch).
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, appendFileSync, writeFileSync, linkSync, copyFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = { scene: [], look: [], repeat: 1, seconds: 30, warmup: 10, width: 960, height: 720, port: 8620, debug: 9347, game: 'gow2', scale: '1', pictures: true, flags: [], tag: '' };
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  const next = () => argv[++i];
  if (a === '--scene') opt.scene.push(next());
  else if (a === '--look') opt.look.push(Number(next()));
  else if (a === '--repeat') opt.repeat = Number(next());
  else if (a === '--seconds') opt.seconds = Number(next());
  else if (a === '--warmup') opt.warmup = Number(next());
  else if (a === '--width') opt.width = Number(next());
  else if (a === '--height') opt.height = Number(next());
  else if (a === '--port') opt.port = Number(next());
  else if (a === '--debug-port') opt.debug = Number(next());
  else if (a === '--root') opt.root = resolve(next());
  else if (a === '--build') opt.build = resolve(next());
  else if (a === '--out') opt.out = resolve(next());
  else if (a === '--edge') opt.edge = next();
  else if (a === '--scale') opt.scale = next();
  else if (a === '--no-pictures') opt.pictures = false;
  else if (a === '--gpu-info') opt.gpuInfo = true;
  else if (a === '--edge-flag') opt.flags.push(next());   // an extra Edge switch for an experiment, e.g. --enable-dawn-features=use_dxc
  else if (a === '--tag') opt.tag = next();               // a label kept in the result line
  else { console.error('unknown argument ' + a); process.exit(2); }
}
const repo = resolve(HERE, '..');
opt.root = opt.root || repo;                       // the folder with games/ (the main checkout, where the states are)
opt.build = opt.build || join(repo, 'build', 'web-' + opt.game);
opt.out = opt.out || join(opt.root, 'games', opt.game, 'speed', 'web-compare');
opt.edge = opt.edge || 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
if (!opt.look.length) opt.look = [0];
if (!opt.scene.length && !opt.gpuInfo) { console.error('give --scene hall|fight2|arena|title|film|colossus (repeatable) or --gpu-info'); process.exit(2); }
mkdirSync(opt.out, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The scene table of speedcheck.py: state file, its frame, the button arguments. Python does the work so the two never differ.
function sceneInfo(name) {
  const code = `
import json, sys
from pathlib import Path
sys.path.insert(0, ${JSON.stringify(join(repo, 'scripts'))})
import speedcheck as s
root = Path(${JSON.stringify(opt.root)})
state, frame, moves = s.SCENES[${JSON.stringify(name)}][:3]
warm, secs = s.SCENES[${JSON.stringify(name)}][4:6]
if state is not None and not state.endswith('.state'):
    state = f'gow2_{state}_f{s.FILM_FRAME:06d}.state'; frame = frame or s.FILM_FRAME
toks = []
for t in moves.split():
    toks.append('@' + str(root / 'games' / 'gow2' / 'play' / t[1:]) if t.startswith('@') and not Path(t[1:]).is_absolute() else t)
import play
args = play.parse_moves(' '.join(play.read_moves(' '.join(toks))), frame) if moves else []
print(json.dumps({'state': state, 'frame': frame, 'args': args, 'warmup': warm, 'seconds': secs}))
`;
  const r = spawnSync('python', ['-X', 'utf8', '-c', code], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error('scene table: ' + r.stderr.slice(-400));
  return JSON.parse(r.stdout.trim().split('\n').pop());
}

function powerInfo() {
  const code = `
import json, sys
sys.path.insert(0, ${JSON.stringify(join(repo, 'scripts'))})
import speedcheck as s
print(json.dumps(s.power_state()))
`;
  const r = spawnSync('python', ['-X', 'utf8', '-c', code], { encoding: 'utf8' });
  try { return JSON.parse(r.stdout.trim().split('\n').pop()); } catch (e) { return { error: String(r.stderr).slice(-200) }; }
}

function otherRunners() {
  const r = spawnSync('tasklist', ['/FI', 'IMAGENAME eq ps2run.exe', '/FO', 'CSV', '/NH'], { encoding: 'utf8' });
  return (r.stdout.match(/ps2run\.exe/g) || []).length;
}

// ------------------------------------------------------------------------------------------------ Edge through DevTools
class Page {
  constructor(ws) {
    this.ws = ws; this.id = 0; this.waiting = new Map(); this.logs = [];
    ws.addEventListener('message', (e) => {
      const m = JSON.parse(e.data);
      if (m.id && this.waiting.has(m.id)) { const [ok, bad] = this.waiting.get(m.id); this.waiting.delete(m.id); m.error ? bad(new Error(JSON.stringify(m.error))) : ok(m.result); }
      else if (m.method === 'Runtime.consoleAPICalled') this.logs.push(m.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
      else if (m.method === 'Runtime.exceptionThrown') this.logs.push('EXCEPTION ' + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
    });
  }
  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((ok, bad) => { this.waiting.set(id, [ok, bad]); this.ws.send(JSON.stringify({ id, method, params })); });
  }
  async eval(expr) {
    const r = await this.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error('page: ' + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
    return r.result.value;
  }
}

async function launchEdge(url) {
  const profile = mkdtempSync(join(tmpdir(), 'webspeed-edge-'));
  const args = [`--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', `--remote-debugging-port=${opt.debug}`,
    '--remote-allow-origins=*', ...opt.flags, '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', '--disable-features=msEdgeWelcome,msEdgeSignInFirstRun',
    `--window-size=${opt.width + 16},${opt.height + 120}`, '--window-position=40,20', `--app=${url}`];
  const child = spawn(opt.edge, args, { stdio: 'ignore', detached: false });
  let target = null;
  for (let i = 0; i < 60 && !target; i++) {
    await sleep(500);
    try {
      const list = await (await fetch(`http://127.0.0.1:${opt.debug}/json`)).json();
      target = list.find((t) => t.type === 'page' && t.url.startsWith('http://127.0.0.1')) || null;
    } catch (e) { /* not up yet */ }
  }
  if (!target) { killTree(child.pid); throw new Error('Edge did not come up'); }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, bad) => { ws.addEventListener('open', ok); ws.addEventListener('error', bad); });
  const page = new Page(ws);
  return { child, profile, page, pid: child.pid };
}

function killTree(pid) { if (pid) spawnSync('taskkill', ['/PID', String(pid), '/T', '/F'], { stdio: 'ignore' }); }

async function waitFor(page, expr, what, ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try { if (await page.eval(expr)) return; } catch (e) { /* page still loading */ }
    await sleep(500);
  }
  throw new Error('timeout waiting for ' + what);
}

// ------------------------------------------------------------------------------------------------ one run
async function oneRun(scene, look, runNo) {
  const info = scene === 'adapter' ? { state: null, frame: 0, args: [], warmup: 5, seconds: 5 } : sceneInfo(scene);
  const warmup = opt.warmupGiven ?? opt.warmup, seconds = opt.seconds;
  const params = new URLSearchParams();
  params.set('PS2_SCALE', opt.scale);
  if (info.state) {
    const staged = join(opt.build, 'states', info.state);
    if (!existsSync(staged)) {
      const from = join(opt.root, 'games', opt.game, 'play', info.state);
      if (!existsSync(from)) throw new Error('no state ' + from);
      mkdirSync(join(opt.build, 'states'), { recursive: true });
      try { linkSync(from, staged); } catch (e) { copyFileSync(from, staged); }
    }
    params.set('state', 'states/' + info.state);
  }
  const presses = [];
  for (let i = 0; i < info.args.length; i += 2) {
    if (info.args[i] === '--press') presses.push(info.args[i + 1]);
    else console.error('note: the web page has no --axis, ignored: ' + info.args[i + 1]);
  }
  if (presses.length) params.append('press', presses.join(','));
  const url = `http://127.0.0.1:${opt.port}/?${params}`;
  const power = powerInfo();
  const result = { tag: opt.tag, flags: opt.flags, time: new Date().toISOString(), browser: 'edge', scene, look, run: runNo, scale: opt.scale, power, ps2run_processes: otherRunners(), url_length: url.length, presses: presses.length };
  const edge = await launchEdge(url);
  const page = edge.page;
  try {
    await page.send('Runtime.enable');
    await page.send('Emulation.setDeviceMetricsOverride', { width: opt.width, height: opt.height + 41, deviceScaleFactor: 1, mobile: false });
    await waitFor(page, `(() => { const s = document.getElementById('start'); return !!s && !s.disabled && s.textContent === 'Start'; })()`, 'the Start button', 180000);
    result.gpu = await page.eval(`(async () => { const a = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }); const i = a.info;
      return { vendor: i.vendor, architecture: i.architecture, device: i.device, description: i.description, fallback: !!i.isFallbackAdapter, page: Module.gpuInfo }; })()`);
    if (scene === 'adapter') { return result; }
    await page.eval(`document.getElementById('start').click(); 1`);
    await waitFor(page, `typeof Module._ps2web_frames === 'function' && Module._ps2web_frames() > 3`, 'the first frames', 240000);
    const first = await page.eval(`Module._ps2web_frames()`);
    result.first_frame = first;
    result.state_expected = info.frame;
    result.state_failed = page.logs.some((l) => /cannot load the state/.test(l));
    await page.eval(`Module._ps2web_video_options(${look}); 1`);
    result.canvas = await page.eval(`({ css: [canvas.clientWidth, canvas.clientHeight], pixels: [canvas.width, canvas.height], dpr: devicePixelRatio })`);
    await sleep(warmup * 1000);
    const a = await page.eval(`({ frames: Module._ps2web_frames(), t: performance.now() })`);
    const per = [];
    const onScreen = [];
    const gpuPct = [];
    const t0 = Date.now();
    while (Date.now() - t0 < seconds * 1000) {
      await sleep(1000);
      const text = await page.eval(`statsEl.textContent`);
      const m = /([\d.]+) fps \((\d+) on screen\)\s+GPU (-?\d+)%/.exec(text);
      if (m) { per.push(Number(m[1])); onScreen.push(Number(m[2])); gpuPct.push(Number(m[3])); }
      result.last_stats = text;
    }
    const b = await page.eval(`({ frames: Module._ps2web_frames(), t: performance.now() })`);
    result.frames = b.frames - a.frames;
    result.seconds = (b.t - a.t) / 1000;
    result.fps = result.frames / result.seconds;
    result.fps_per_second = per;
    result.fps_mean_of_seconds = per.length ? per.reduce((x, y) => x + y, 0) / per.length : null;
    result.on_screen_mean = onScreen.length ? onScreen.reduce((x, y) => x + y, 0) / onScreen.length : null;
    result.gpu_percent_mean = gpuPct.length ? gpuPct.reduce((x, y) => x + y, 0) / gpuPct.length : null;
    result.state_reached = a.frames;  // how far the game has come after the warmup
    result.errors = page.logs.filter((l) => /EXCEPTION|cannot|failed|unhandled|missing/i.test(l)).slice(0, 5);
    if (opt.pictures) {
      const shot = await page.send('Page.captureScreenshot', { format: 'jpeg', quality: 40, clip: { x: 0, y: 0, width: opt.width, height: opt.height, scale: 0.3 } });
      const file = join(opt.out, `web-${scene}-look${look}-run${runNo}.jpg`);
      writeFileSync(file, Buffer.from(shot.data, 'base64'));
      result.picture = file;
    }
  } finally {
    try { edge.page.ws.close(); } catch (e) { /* closing anyway */ }
    killTree(edge.pid);
    await sleep(1500);
    try { rmSync(edge.profile, { recursive: true, force: true }); } catch (e) { /* temp folder, left for the system */ }
  }
  return result;
}

const scenes = opt.gpuInfo ? ['adapter'] : opt.scene;
for (const look of opt.look) {
  for (let run = 1; run <= opt.repeat; run++) {
    for (const scene of scenes) {
      let r;
      try { r = await oneRun(scene, look, run); } catch (e) { r = { scene, look, run, error: String(e.message || e) }; }
      appendFileSync(join(opt.out, 'web-speed.jsonl'), JSON.stringify(r) + '\n');
      const brief = r.error ? 'ERROR ' + r.error : (r.fps !== undefined ? `${r.fps.toFixed(1)} fps (mean of seconds ${r.fps_mean_of_seconds?.toFixed(1)}, on screen ${r.on_screen_mean?.toFixed(0)}, GPU ${r.gpu_percent_mean?.toFixed(0)}%)` : JSON.stringify(r.gpu));
      console.log(`web ${scene} look ${look} run ${run}: ${brief}  | mains ${r.power?.mains} mode ${r.power?.mode} plan ${r.power?.plan}`);
    }
  }
}
