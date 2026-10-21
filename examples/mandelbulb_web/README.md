# mandelbulb_web

A raymarched 3D Mandelbulb. Echo fills an RGBA buffer, JavaScript paints it onto a canvas, and
a backend picker swaps Echo for the same renderer written in plain JavaScript, so you can put
the two side by side.

This is a `#[target: lib]`: there is no `main`, no `_start`. The page instantiates the
module, calls `_initialize` (wasi-libc's reactor entry), then calls `configure` and
`render_rows`.

## Files

| | |
|---|---|
| `module.eco` | the lib target, named `mandelbulb` |
| `src/vec3.eco` | `Vec3` by value, with declared `+`, `-` and `* float64` operators |
| `src/camera.eco` | an orbit camera around the origin, and a ray per pixel |
| `src/bulb.eco` | the power-N distance estimator, plus an orbit trap for colour |
| `src/shade.eco` | the march, the tetrahedron normal, the lighting |
| `src/render.eco` | `#[export] configure` / `render_rows` / `pixel_count`, `#[wasm: import]` host_log / host_now_ms, `#[wasm: export "memory"]` |
| `web/mandelbulb.js` | the same renderer in plain JavaScript, for comparison |
| `web/wasi.js` | a small `wasi_snapshot_preview1` shim. libc still links `fd_*` even in a reactor |
| `web/main.js` | fetch, instantiate, render in bands, paint, time, benchmark |
| `index.html` | canvas, backend, resolution, power / iterations / camera sliders |

The boundary is scalars and `ptr<T>`. `configure` takes the scene as seven numbers and returns
a pointer into linear memory; JS reads those bytes as `ImageData`. `host_log` takes a pointer
and a length because that is what a `string` already is on the Echo side.

## Why the frame comes in bands

A 2D Mandelbrot pixel is one loop. A Mandelbulb pixel is a march of up to 128 steps, and
every step runs the distance estimator, which is `acos`, `atan2`, `pow`, `sin` and `cos` per
iteration. That is a fraction of a second per frame, not a few milliseconds, so the page does
not ask for the frame in one call. `configure` sets the scene, then `render_rows(y0, y1)` is
called for bands of rows, about 12 ms of work per animation frame. The canvas fills in from
the top, the page stays responsive, and moving a slider starts a new frame rather than
queueing behind the old one. Echo sums its own band times and logs the frame's compute time
through `host_log` when the last row is done.

A reactor never runs static teardown, and `std::env::args()` is empty. Statics still work:
the pixel buffer and the scene are lazy first-use statics on `PixelBuffer` and `Scene`.

## Echo vs JavaScript

`web/mandelbulb.js` is a line-for-line port of the `.eco` files, with the operations in the
same order and a byte clamp that truncates the way `$v as uint8` does (a `Uint8ClampedArray`
store would round). Both backends draw the same picture: on Chrome they agree on every
pixel, `pow`/`acos`/`atan2` included.

One liberty: the port keeps vectors as plain `x, y, z` locals instead of objects. Echo's
`Vec3` is a value that lives in registers; an object per vector operation in JS would be
timing the allocator.

The table times compute only, not `putImageData`, which costs the same either way. A single
render is a noisy number (JIT warm-up on one side, first-use statics and `memory.grow` on the
other), so use **benchmark**: one warm-up plus five full frames per backend, compared by
median. The backend choice sticks in the URL hash (`#js`). If the `.wasm` is not built, the
page still runs the JavaScript side.

Fair warning: JavaScript wins this one. At 320×240 on an M-series Mac in Chrome it was about
160 ms against Echo's 195 ms, and `--optimize whole` moved Echo by only a few percent. The
likely reason is that the hot loop is mostly transcendental math: in the `.wasm` those are
wasi-libc's software `acos`/`atan2`/`pow`/`sin`/`cos`, while JS gets V8's built-ins. That is
an educated guess, not a profile.

## Build and serve

Browsers refuse `fetch` of a `.wasm` from `file://`, so this wants a local server.

```bash
cd examples/mandelbulb_web
echoc build --target-os wasi
python3 -m http.server
```

Then open `http://localhost:8000`. The module lands at `ecobuild/mandelbulb.wasm`.

Check the surface:

```bash
node -e "const m = new WebAssembly.Module(require('fs').readFileSync('ecobuild/mandelbulb.wasm')); console.log(WebAssembly.Module.exports(m), WebAssembly.Module.imports(m))"
```

You should see exports `configure`, `render_rows`, `pixel_count`, `_initialize` and `memory`,
and imports `env.host_log` and `env.host_now_ms`. There is no `_start`.
