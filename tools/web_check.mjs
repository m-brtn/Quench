#!/usr/bin/env node
// Runs a web build's page in headless Chrome and checks its scripted run (tools/check.sh step web; docs/app.md,
// "Web runs"). No window on the screen: Chrome starts with --headless=new and a temporary profile.
//
//   node tools/web_check.mjs --page examples/demo/quench-demo.html [--build build/web] [--out DIR]
//        [--golden DIR --compare quench_image_compare] [--timeout SECONDS] [--chrome PATH] [--repo DIR]
//        [--onscreen] [--system-user-dir] [--databases N,N,...] -- APP ARGUMENTS... [-- APP ARGUMENTS...]
//
// The script serves the build directory at / and the repository at /repo/ on 127.0.0.1, so that a scenario is named
// by its URL (--script /repo/examples/demo/scenarios/batch.txt: the page fetches what its virtual file system does
// not have). Paths are relative to the current directory. The defaults of --build and --repo are this engine
// checkout's build/web and the checkout; a game names its own:
//   node <engine>/tools/web_check.mjs --build build/web --repo . --page my_game.html -- \
//        --script /repo/scenarios/smoke.txt
// It adds --offscreen and --user-dir /user to the arguments, prints the page's log (the shell's quenchLog), waits
// for the program's end (quenchExit) and saves the log and the captures of /user into --out. With --golden every
// capture that has a golden image of its name is compared with it. With --onscreen the program runs as on a page a
// person opened (no --offscreen: the canvas fills the page, the frames follow the display, the sound output opens),
// still in headless Chrome; give it a --quit-after.
//
// Several argument lists, separated by `--`, load the page once each, one after another, in the same browser profile
// (the captures and the exit code are the last load's; every load must end with 0). --system-user-dir leaves out
// --user-dir /user, so a load without its own --user-dir uses the default user directory, which the browser keeps
// in IndexedDB. After each load the script waits for the program's stores to IndexedDB to end; --databases gives the
// number of IndexedDB databases the page's origin must have after each load (the persistence check of
// tools/check.sh step web). Without these options a run is one load in a fresh profile, as before.
//
// Exit code: 0 when the program ended with 0, no WebGPU error was logged and every compared capture passed; 1
// otherwise; 2 when the page did not reach the program's end.
//
// Node.js 22 or later (its WebSocket), Google Chrome (QUENCH_CHROME or --chrome names another Chromium).

import { spawn, spawnSync } from 'node:child_process';
import { createReadStream, existsSync, mkdirSync, mkdtempSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { dirname, extname, join, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const options = {
    build: join(root, 'build/web'),
    repo: root,
    page: '',
    out: '',
    golden: '',
    compare: '',
    timeout: 900,
    chrome: process.env.QUENCH_CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
};
let onscreen = false;
let systemUserDir = false;
let databases = [];
const loads = [];
{
    const argv = process.argv.slice(2);
    for (let i = 0; i < argv.length; ++i) {
        if (argv[i] === '--') {
            let current = [];
            for (const arg of argv.slice(i + 1)) {
                if (arg === '--') {
                    loads.push(current);
                    current = [];
                } else {
                    current.push(arg);
                }
            }
            loads.push(current);
            break;
        }
        if (argv[i] === '--onscreen') {
            onscreen = true;
            continue;
        }
        if (argv[i] === '--system-user-dir') {
            systemUserDir = true;
            continue;
        }
        if (argv[i] === '--databases' && i + 1 < argv.length) {
            databases = argv[++i].split(',').map(Number);
            continue;
        }
        const key = argv[i].replace(/^--/, '');
        if (!argv[i].startsWith('--') || !(key in options) || i + 1 >= argv.length) {
            console.error(`web_check: unknown or incomplete option ${argv[i]} (see the top of tools/web_check.mjs)`);
            process.exit(2);
        }
        options[key] = key === 'timeout' ? Number(argv[++i]) : argv[++i];
    }
}
if (loads.length === 0) loads.push([]);
if (databases.length && (databases.length !== loads.length || databases.some((n) => !Number.isInteger(n)))) {
    console.error(`web_check: --databases needs one whole number per load (${loads.length})`);
    process.exit(2);
}
if (!options.page) {
    console.error('web_check: --page PATH (relative to the build directory) is required');
    process.exit(2);
}
const buildDir = resolve(options.build);
const repoDir = resolve(options.repo);
if (!existsSync(join(buildDir, options.page))) {
    console.error(`web_check: ${join(buildDir, options.page)} does not exist (build the web preset first)`);
    process.exit(2);
}
if (!existsSync(options.chrome)) {
    console.error(`web_check: no Chrome at ${options.chrome} (QUENCH_CHROME or --chrome names one)`);
    process.exit(2);
}

// ---------------------------------------------------------------- the server

const mime = {
    '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.txt': 'text/plain',
    '.png': 'image/png', '.json': 'application/json',
};
const server = createServer((request, response) => {
    let path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    let base = buildDir;
    if (path.startsWith('/repo/')) {
        base = repoDir;
        path = path.substring('/repo'.length);
    }
    const file = normalize(join(base, path));
    if (!file.startsWith(base) || !existsSync(file) || !statSync(file).isFile()) {
        response.writeHead(404).end('not found');
        return;
    }
    response.writeHead(200, {
        'Content-Type': mime[extname(file)] || 'application/octet-stream',
        'Content-Length': statSync(file).size,
        'Cache-Control': 'no-store',
    });
    createReadStream(file).pipe(response);
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const origin = `http://127.0.0.1:${server.address().port}`;

// ---------------------------------------------------------------- Chrome

const profile = mkdtempSync(join(tmpdir(), 'quench_web_check_'));
const chrome = spawn(options.chrome, [
    '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run',
    '--no-default-browser-check', '--enable-unsafe-webgpu', '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', '--mute-audio',
    '--window-size=1280,720', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

let finished = false;
function cleanUp() {
    if (finished) return;
    finished = true;
    chrome.kill();
    server.close();
    try {
        rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
    } catch {
        // Chrome may still be writing; the profile is in the temporary directory.
    }
}
function fail(code, message) {
    console.error(`web_check: ${message}`);
    cleanUp();
    process.exit(code);
}
process.on('SIGINT', () => fail(2, 'interrupted'));

const debugPort = await new Promise((done) => {
    let text = '';
    const timer = setTimeout(() => done(0), 30000);
    chrome.stderr.on('data', (chunk) => {
        text += chunk;
        const match = text.match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)\//);
        if (match) {
            clearTimeout(timer);
            done(Number(match[1]));
        }
    });
    chrome.on('exit', () => done(0));
});
if (!debugPort) fail(2, 'Chrome did not start its DevTools port');

const targets = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
const target = targets.find((t) => t.type === 'page');
if (!target) fail(2, 'Chrome has no page');
const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((done, failed) => {
    socket.addEventListener('open', done);
    socket.addEventListener('error', failed);
});
let nextId = 0;
const waiting = new Map();
socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    const entry = waiting.get(message.id);
    if (!entry) return;
    waiting.delete(message.id);
    if (message.error) entry.failed(new Error(message.error.message));
    else entry.done(message.result);
});
function send(method, params = {}) {
    return new Promise((done, failed) => {
        const id = ++nextId;
        waiting.set(id, { done, failed });
        socket.send(JSON.stringify({ id, method, params }));
    });
}
async function evaluate(expression) {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || 'exception');
    return result.result.value;
}

// ---------------------------------------------------------------- the run

const log = [];
let exitCode;
let stopped = '';
let failures = 0;
const deadline = Date.now() + options.timeout * 1000;
let previousPage = 0; // performance.timeOrigin of the last load's document: its state is not the new one's
for (let load = 0; load < loads.length; ++load) {
    const args = [...loads[load], ...(onscreen ? [] : ['--offscreen']),
        ...(systemUserDir ? [] : ['--user-dir', '/user'])];
    const url = `${origin}/${options.page}?${args.map(encodeURIComponent).join('&')}`;
    const name = loads.length > 1 ? ` (load ${load + 1} of ${loads.length})` : '';
    console.log(`web_check: ${options.page} ${args.join(' ')}${name}`);
    await send('Page.navigate', { url });

    exitCode = undefined;
    let seen = 0;
    while (Date.now() < deadline) {
        await new Promise((done) => setTimeout(done, 500));
        let state;
        try {
            state = JSON.parse(await evaluate(`JSON.stringify({
                lines: (window.quenchLog || []).slice(${seen}),
                page: performance.timeOrigin,
                exit: window.quenchExit,
                status: (document.getElementById('status') || {}).textContent || ''})`));
        } catch {
            continue; // the page is loading
        }
        if (state.page === previousPage) continue; // the previous load's document, before the new one is there
        for (const line of state.lines) {
            ++seen;
            log.push(line);
            console.log(line);
        }
        if (state.exit !== undefined && state.exit !== null) {
            exitCode = state.exit;
            break;
        }
        if (/^(Error:|The program stopped|This browser has no WebGPU)/.test(state.status)) {
            stopped = state.status;
            break;
        }
    }
    if (stopped || exitCode === undefined) break;
    previousPage = await evaluate('performance.timeOrigin');
    if (load + 1 < loads.length && exitCode !== 0) {
        console.error(`web_check: FAIL load ${load + 1} ended with exit code ${exitCode}`);
        ++failures;
    }
    if (loads.length > 1 || databases.length) {
        // The program's stores to IndexedDB run after its writes, in the background (src/app/SdlFileSystem.cpp).
        let pending = true;
        while (pending && Date.now() < deadline) {
            pending = await evaluate(`(() => { const s = window.Module && Module.quenchUserFiles;
                return !!(s && (s.busy || s.again)); })()`);
            if (pending) await new Promise((done) => setTimeout(done, 100));
        }
        const files = await evaluate(`JSON.stringify((window.Module && Module.quenchUserFiles) || null)`);
        const names = await evaluate(`indexedDB.databases().then((list) => JSON.stringify(list.map((d) => d.name)))`);
        console.log(`web_check: after load ${load + 1}: user files ${files}, IndexedDB databases ${names}`);
        if (databases.length && JSON.parse(names).length !== databases[load]) {
            console.error(`web_check: FAIL ${databases[load]} IndexedDB databases expected after load ${load + 1}`);
            ++failures;
        }
    }
}

let captures = [];
if (options.out) {
    const out = resolve(options.out);
    rmSync(out, { recursive: true, force: true });
    mkdirSync(out, { recursive: true });
    writeFileSync(join(out, 'log.txt'), log.join('\n') + '\n');
    let names = [];
    try {
        names = JSON.parse(await evaluate(
            `JSON.stringify(Module.FS.readdir('/user').filter((name) => name.endsWith('.png')))`));
    } catch {
        // No user directory: the program did not get that far.
    }
    for (const name of names) {
        const base64 = await evaluate(`(() => {
            const bytes = Module.FS.readFile('/user/' + ${JSON.stringify(name)});
            let text = '';
            for (let i = 0; i < bytes.length; i += 32768)
                text += String.fromCharCode.apply(null, bytes.subarray(i, i + 32768));
            return btoa(text); })()`);
        writeFileSync(join(out, name), Buffer.from(base64, 'base64'));
        captures.push(name);
    }
    console.log(`web_check: log.txt and ${captures.length} captures in ${options.out}`);
}
socket.close();
cleanUp();

if (stopped) {
    console.error(`web_check: the page stopped: ${stopped}`);
    process.exit(2);
}
if (exitCode === undefined) {
    console.error(`web_check: the program did not end within ${options.timeout} s`);
    process.exit(2);
}

const gpuErrors = log.filter((line) => /ERROR: gpu: WebGPU|WebGPU device lost/.test(line));
if (gpuErrors.length) {
    console.error(`web_check: FAIL ${gpuErrors.length} WebGPU errors, the first: ${gpuErrors[0]}`);
    ++failures;
}
if (exitCode !== 0) {
    console.error(`web_check: FAIL the program ended with exit code ${exitCode}`);
    ++failures;
}
if (options.golden) {
    const golden = resolve(options.golden);
    const compare = resolve(options.compare);
    if (!options.out || !existsSync(compare)) {
        console.error('web_check: --golden needs --out and --compare (quench_image_compare of a host build)');
        process.exit(2);
    }
    let compared = 0;
    for (const name of captures) {
        if (!existsSync(join(golden, name))) continue;
        ++compared;
        const result = spawnSync(compare, [join(resolve(options.out), name), join(golden, name)],
            { encoding: 'utf8' });
        const line = (result.stdout || result.stderr || '').split('\n')[0];
        if (result.status !== 0) {
            ++failures;
            console.error(`web_check: FAIL ${name}: ${line}`);
        } else {
            console.log(`web_check: ok   ${name}: ${line}`);
        }
    }
    console.log(`web_check: ${compared} captures compared with ${options.golden}`);
}
console.log(`web_check: ${failures === 0 ? 'passed' : `${failures} failed`}`);
process.exit(failures === 0 ? 0 : 1);
