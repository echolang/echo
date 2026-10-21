/*
 * The same renderer as src/*.eco, written in plain JavaScript.
 *
 * A line-for-line port: same camera, same distance estimator, same march, same
 * lighting, same truncating byte clamp, operations in the same order. The two
 * backends draw the same picture (up to the odd last-bit difference between
 * wasi-libc's and V8's pow/acos/atan2), so the timing compares who computed it.
 *
 * The one liberty: vectors are plain x, y, z locals instead of objects. Echo's
 * Vec3 is a value that lives in registers; an object per vector operation in JS
 * would be timing the allocator.
 */

const MAX_STEPS = 128;

let pixels = new Uint8ClampedArray(0);
const scene = {
    width: 1,
    height: 1,
    power: 8.0,
    maxIter: 8,
    eye: [0, 0, 0],
    forward: [0, 0, 0],
    right: [0, 0, 0],
    up: [0, 0, 0],
    tanHalfFov: 0.5,
};

function clampByte(v) {
    if (v < 0.0) {
        return 0;
    }
    if (v > 255.0) {
        return 255;
    }
    // `$v as uint8` truncates; a Uint8ClampedArray store would round
    return v | 0;
}

function normalize3(x, y, z) {
    const inv = 1.0 / Math.sqrt(x * x + y * y + z * z);
    return [x * inv, y * inv, z * inv];
}

function cross3(ax, ay, az, bx, by, bz) {
    return [ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx];
}

// bulb_distance: the sample comes back through these two, not an object
let sampleDistance = 0.0;
let sampleTrap = 0.0;

function bulbDistance(px, py, pz, power, maxIter) {
    let zx = px;
    let zy = py;
    let zz = pz;
    let dr = 1.0;
    let r = Math.sqrt(zx * zx + zy * zy + zz * zz);
    let trap = r * r;
    let i = 0;

    while (i < maxIter && r <= 2.0) {
        const theta = Math.acos(zz / r) * power;
        const phi = Math.atan2(zy, zx) * power;
        let rn = Math.pow(r, power - 1.0);
        dr = rn * power * dr + 1.0;
        rn = rn * r;

        const sinTheta = Math.sin(theta);
        zx = sinTheta * Math.cos(phi) * rn + px;
        zy = Math.sin(phi) * sinTheta * rn + py;
        zz = Math.cos(theta) * rn + pz;

        r = Math.sqrt(zx * zx + zy * zy + zz * zz);
        trap = Math.min(trap, r * r);
        i++;
    }

    sampleDistance = 0.5 * Math.log(r) * r / dr;
    sampleTrap = trap;
}

function surfaceDistance(px, py, pz, kx, ky, kz, e, power, maxIter) {
    bulbDistance(px + kx * e, py + ky * e, pz + kz * e, power, maxIter);
    return sampleDistance;
}

const LIGHT = normalize3(0.6, 0.8, 0.5);

function shadePixel(x, y, out, at) {
    const s = scene;
    const w = s.width;
    const h = s.height;
    const power = s.power;
    const maxIter = s.maxIter;

    // ray_direction
    const u = ((x + 0.5) / w * 2.0 - 1.0) * (w / h) * s.tanHalfFov;
    const v = (1.0 - (y + 0.5) / h * 2.0) * s.tanHalfFov;
    const [dx, dy, dz] = normalize3(
        s.forward[0] + s.right[0] * u + s.up[0] * v,
        s.forward[1] + s.right[1] * u + s.up[1] * v,
        s.forward[2] + s.right[2] * u + s.up[2] * v
    );
    const [ex, ey, ez] = s.eye;

    const b = ex * dx + ey * dy + ez * dz;
    const c = (ex * ex + ey * ey + ez * ez) - 1.5625;
    const disc = b * b - c;

    let hit = false;
    let t = 0.0;
    let tFar = 0.0;
    let steps = 0;
    let eps = 0.0001;
    let trap = 0.0;

    if (disc > 0.0) {
        const root = Math.sqrt(disc);
        t = Math.max(0.0 - b - root, 0.0);
        tFar = 0.0 - b + root;
    }

    if (disc > 0.0 && tFar > 0.0) {
        const pixel = 2.0 * s.tanHalfFov / h;
        while (steps < MAX_STEPS && t < tFar) {
            bulbDistance(ex + dx * t, ey + dy * t, ez + dz * t, power, maxIter);
            eps = Math.max(t * pixel * 0.5, 0.0001);
            if (sampleDistance < eps) {
                hit = true;
                trap = sampleTrap;
                break;
            }
            t = t + sampleDistance;
            steps++;
        }
    }

    if (!hit) {
        const g = 0.5 + 0.5 * dy;
        out[at] = clampByte(10.0 + 30.0 * g);
        out[at + 1] = clampByte(12.0 + 38.0 * g);
        out[at + 2] = clampByte(22.0 + 58.0 * g);
        return;
    }

    const px = ex + dx * t;
    const py = ey + dy * t;
    const pz = ez + dz * t;

    // surface_normal: four samples on a tetrahedron
    const d0 = surfaceDistance(px, py, pz, 1.0, -1.0, -1.0, eps, power, maxIter);
    const d1 = surfaceDistance(px, py, pz, -1.0, -1.0, 1.0, eps, power, maxIter);
    const d2 = surfaceDistance(px, py, pz, -1.0, 1.0, -1.0, eps, power, maxIter);
    const d3 = surfaceDistance(px, py, pz, 1.0, 1.0, 1.0, eps, power, maxIter);
    const [nx, ny, nz] = normalize3(
        1.0 * d0 + -1.0 * d1 + -1.0 * d2 + 1.0 * d3,
        -1.0 * d0 + -1.0 * d1 + 1.0 * d2 + 1.0 * d3,
        -1.0 * d0 + 1.0 * d1 + -1.0 * d2 + 1.0 * d3
    );

    const [lx, ly, lz] = LIGHT;
    const diffuse = Math.max(nx * lx + ny * ly + nz * lz, 0.0);
    const [hx, hy, hz] = normalize3(lx - dx, ly - dy, lz - dz);
    const specular = Math.pow(Math.max(nx * hx + ny * hy + nz * hz, 0.0), 32.0) * 0.35;
    const occlusion = 1.0 - steps / MAX_STEPS;
    const lit = (0.12 + 0.88 * diffuse) * occlusion;
    const tone = Math.min(Math.max((Math.sqrt(trap) - 0.75) * 2.8, 0.0), 1.0);

    out[at] = clampByte(((0.95 - 0.55 * tone) * lit + specular) * 255.0);
    out[at + 1] = clampByte(((0.55 + 0.25 * tone) * lit + specular) * 255.0);
    out[at + 2] = clampByte(((0.30 + 0.60 * tone) * lit + specular) * 255.0);
}

export function configure(width, height, yawDeg, pitchDeg, distance, power, maxIter) {
    width = Math.max(1, width | 0);
    height = Math.max(1, height | 0);
    maxIter = Math.max(1, maxIter | 0);

    // orbit_camera
    const radians = 0.017453292519943295;
    const yaw = yawDeg * radians;
    const pitch = pitchDeg * radians;
    const eye = [
        Math.cos(pitch) * Math.sin(yaw) * distance,
        Math.sin(pitch) * distance,
        Math.cos(pitch) * Math.cos(yaw) * distance,
    ];
    const forward = normalize3(eye[0] * -1.0, eye[1] * -1.0, eye[2] * -1.0);
    const right = normalize3(...cross3(...forward, 0.0, 1.0, 0.0));
    const up = cross3(...right, ...forward);

    Object.assign(scene, { width, height, power, maxIter, eye, forward, right, up, tanHalfFov: 0.5 });

    const needed = width * height * 4;
    if (pixels.length < needed) {
        pixels = new Uint8ClampedArray(needed);
    }
    return pixels.subarray(0, needed);
}

export function renderRows(yBegin, yEnd) {
    const w = scene.width;
    yEnd = Math.min(yEnd, scene.height);
    for (let y = yBegin; y < yEnd; y++) {
        for (let x = 0; x < w; x++) {
            const at = (y * w + x) * 4;
            shadePixel(x, y, pixels, at);
            pixels[at + 3] = 255;
        }
    }
}
