import { makeWasi } from './wasi.js';
import * as bulbJs from './mandelbulb.js';

const canvas = document.getElementById('view');
const ctx = canvas.getContext('2d');
const status = document.getElementById('status');
const backendSelect = document.getElementById('backend');
const resolutionSelect = document.getElementById('resolution');
const resetButton = document.getElementById('reset');
const benchButton = document.getElementById('bench');
const results = document.getElementById('results');
const verdict = document.getElementById('verdict');

const sliders = {
    power: document.getElementById('power'),
    iters: document.getElementById('iters'),
    yaw: document.getElementById('yaw'),
    pitch: document.getElementById('pitch'),
    distance: document.getElementById('distance'),
};

const BENCH_RUNS = 5;
const RING_SIZE = 10;
// how much rendering one animation frame may do before the page gets a turn
const BAND_BUDGET_MS = 12;

let wasm;
let memory;
let quiet = false;

function envImports() {
    return {
        host_log(ptr, len) {
            if (quiet) {
                return;
            }
            const bytes = new Uint8Array(memory.buffer, ptr, Number(len));
            console.log(new TextDecoder().decode(bytes));
        },
        host_now_ms() {
            return performance.now();
        },
    };
}

// each backend: configure(...) once per frame, then renderRows over bands, then pixels() to paint
const backends = {
    echo: {
        label: 'Echo',
        available: false,
        ptr: 0,
        configure(...args) {
            this.ptr = wasm.exports.configure(...args);
        },
        renderRows(y0, y1) {
            wasm.exports.render_rows(y0, y1);
        },
        pixels() {
            // memory.grow detaches the old ArrayBuffer, so the view is rebuilt every time
            return new Uint8ClampedArray(memory.buffer, this.ptr, Number(wasm.exports.pixel_count()));
        },
    },
    js: {
        label: 'JavaScript',
        available: true,
        view: null,
        configure(...args) {
            this.view = bulbJs.configure(...args);
        },
        renderRows(y0, y1) {
            bulbJs.renderRows(y0, y1);
        },
        pixels() {
            return this.view;
        },
    },
};

const stats = {};
for (const key of Object.keys(backends)) {
    stats[key] = { last: null, ring: [], best: null };
}

function resetStats() {
    for (const s of Object.values(stats)) {
        s.last = null;
        s.ring = [];
        s.best = null;
    }
}

function record(key, ms) {
    const s = stats[key];
    s.last = ms;
    s.ring.push(ms);
    if (s.ring.length > RING_SIZE) {
        s.ring.shift();
    }
    s.best = s.best === null ? ms : Math.min(s.best, ms);
}

function fmt(ms) {
    return ms === null ? '–' : ms.toFixed(0);
}

function showStats() {
    results.innerHTML = '';
    for (const [key, backend] of Object.entries(backends)) {
        const s = stats[key];
        const avg = s.ring.length ? s.ring.reduce((a, b) => a + b, 0) / s.ring.length : null;
        const row = document.createElement('tr');
        if (key === backendSelect.value) {
            row.className = 'active';
        }
        for (const text of [backend.label, fmt(s.last), fmt(avg), fmt(s.best)]) {
            const cell = document.createElement('td');
            cell.textContent = text;
            row.appendChild(cell);
        }
        results.appendChild(row);
    }
}

function sceneArgs() {
    return [
        canvas.width,
        canvas.height,
        Number(sliders.yaw.value),
        Number(sliders.pitch.value),
        Number(sliders.distance.value),
        Number(sliders.power.value),
        Number(sliders.iters.value),
    ];
}

function applyResolution() {
    const [w, h] = resolutionSelect.value.split('x').map(Number);
    canvas.width = w;
    canvas.height = h;
}

function draw(pixels) {
    ctx.putImageData(new ImageData(pixels, canvas.width, canvas.height), 0, 0);
}

const nextFrame = () => new Promise(requestAnimationFrame);

// bumped by every new frame; a loop that sees a newer one gives up
let generation = 0;

async function startFrame() {
    const mine = ++generation;
    const key = backendSelect.value;
    const backend = backends[key];
    const height = canvas.height;

    backend.configure(...sceneArgs());

    let y = 0;
    let rows = 4;
    let compute = 0;

    while (y < height) {
        const budgetStart = performance.now();
        while (y < height && performance.now() - budgetStart < BAND_BUDGET_MS) {
            const started = performance.now();
            backend.renderRows(y, y + rows);
            const ms = performance.now() - started;
            compute += ms;
            y = Math.min(y + rows, height);
            // aim each band at a quarter of the budget, whatever the scene costs
            rows = Math.max(1, Math.min(64, Math.round(rows * (BAND_BUDGET_MS / 4) / Math.max(ms, 0.1))));
        }

        draw(backend.pixels());
        status.textContent = `rendering… ${Math.round((100 * y) / height)}%`;
        await nextFrame();
        if (mine !== generation) {
            return;
        }
    }

    record(key, compute);
    status.textContent = `rendered in ${compute.toFixed(0)} ms by ${backend.label}`;
    showStats();
}

function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    const mid = sorted.length >> 1;
    return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

function fullFrame(key) {
    const backend = backends[key];
    backend.configure(...sceneArgs());
    const started = performance.now();
    backend.renderRows(0, canvas.height);
    return performance.now() - started;
}

async function benchmark() {
    // stop any frame in flight, it would interleave with the timed ones
    generation++;
    const controls = [backendSelect, resolutionSelect, resetButton, benchButton, ...Object.values(sliders)];
    for (const c of controls) {
        c.disabled = true;
    }
    quiet = true;
    verdict.textContent = '';

    const medians = {};
    try {
        for (const [key, backend] of Object.entries(backends)) {
            if (!backend.available) {
                continue;
            }
            status.textContent = `benchmarking ${backend.label}…`;
            await nextFrame();

            // the first frame pays for JIT warm-up, first-use statics and memory.grow
            fullFrame(key);
            const samples = [];
            for (let i = 0; i < BENCH_RUNS; i++) {
                const ms = fullFrame(key);
                samples.push(ms);
                record(key, ms);
                status.textContent = `benchmarking ${backend.label}… ${i + 1}/${BENCH_RUNS}`;
                await nextFrame();
            }
            medians[key] = median(samples);
            showStats();
        }
    } finally {
        quiet = false;
        for (const c of controls) {
            c.disabled = false;
        }
        backendSelect.querySelector('option[value="echo"]').disabled = !backends.echo.available;
    }

    const shown = backends[backendSelect.value];
    fullFrame(backendSelect.value);
    draw(shown.pixels());

    const parts = Object.entries(medians).map(
        ([key, ms]) => `${backends[key].label} median ${ms.toFixed(0)} ms`
    );
    if (medians.echo !== undefined && medians.js !== undefined) {
        const ratio = medians.js / medians.echo;
        parts.push(ratio >= 1
            ? `Echo is ${ratio.toFixed(2)}× faster`
            : `Echo is ${(1 / ratio).toFixed(2)}× slower`);
    }
    verdict.textContent = `${canvas.width}×${canvas.height}, ${BENCH_RUNS} runs each: ${parts.join(', ')}`;
    status.textContent = 'benchmark done';
}

const defaults = {};
for (const [name, input] of Object.entries(sliders)) {
    defaults[name] = input.value;
    input.addEventListener('input', startFrame);
}

resetButton.addEventListener('click', () => {
    for (const [name, input] of Object.entries(sliders)) {
        input.value = defaults[name];
        document.querySelector(`output[for="${input.id}"]`).textContent = input.value;
    }
    startFrame();
});

resolutionSelect.addEventListener('change', () => {
    applyResolution();
    // a different pixel count is a different benchmark
    resetStats();
    verdict.textContent = '';
    startFrame();
});

backendSelect.addEventListener('change', () => {
    history.replaceState(null, '', '#' + backendSelect.value);
    startFrame();
});

benchButton.addEventListener('click', benchmark);

async function loadEcho() {
    const response = await fetch('ecobuild/mandelbulb.wasm');
    if (!response.ok) {
        throw new Error('fetch mandelbulb.wasm failed: ' + response.status);
    }
    const { instance } = await WebAssembly.instantiate(await response.arrayBuffer(), {
        env: envImports(),
        wasi_snapshot_preview1: makeWasi(() => memory),
    });
    wasm = instance;
    memory = instance.exports.memory;
    instance.exports._initialize();
}

let loadError = null;
try {
    await loadEcho();
    backends.echo.available = true;
} catch (error) {
    loadError = error;
    console.error(error);
    backendSelect.querySelector('option[value="echo"]').disabled = true;
}

const wanted = location.hash.slice(1);
backendSelect.value = backends[wanted]?.available ? wanted : (backends.echo.available ? 'echo' : 'js');
applyResolution();
showStats();
await startFrame();

if (loadError) {
    status.textContent = 'Echo module not loaded (run `echoc build --target-os wasi`), showing JavaScript only';
}
