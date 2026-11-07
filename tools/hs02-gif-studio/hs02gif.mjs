// HS-02 GIF studio core: GIF decoding, cropping/scaling to the 160x80 screen, the 2bpp band
// encoder and the firmware patcher. Pure JavaScript, no dependencies: the web constructor
// (index.html) and the command line tool (cli.mjs, used by CI) share this file, and the firmware
// side lives in source/Core/Threads/UI/drawing/color_160x80/draw_gif.cpp (format described there).

export const W = 160;
export const H = 80;
export const ROW_BYTES = W / 4;
export const FRAME_BYTES = ROW_BYTES * H; // 3200
export const DEFAULT_BAND_HEIGHT = 4;
export const KIND = { standby: 0, boot: 1 };

// ---------------------------------------------------------------- GIF decoding
function lzwDecode(minCodeSize, data, pixelCount) {
  const out = new Uint8Array(pixelCount);
  const clear = 1 << minCodeSize;
  const eoi = clear + 1;
  const prefix = new Int16Array(4096);
  const suffix = new Uint8Array(4096);
  const stack = new Uint8Array(4097);
  let codeSize = minCodeSize + 1;
  let codeMask = (1 << codeSize) - 1;
  let next = eoi + 1;
  let old = -1;
  let first = 0;
  let bits = 0;
  let datum = 0;
  let op = 0;
  for (let i = 0; i < clear; i++) {
    prefix[i] = -1;
    suffix[i] = i;
  }
  for (let ip = 0; ip < data.length && op < pixelCount; ) {
    while (bits < codeSize && ip < data.length) {
      datum |= data[ip++] << bits;
      bits += 8;
    }
    if (bits < codeSize) break;
    let code = datum & codeMask;
    datum >>>= codeSize;
    bits -= codeSize;
    if (code === clear) {
      codeSize = minCodeSize + 1;
      codeMask = (1 << codeSize) - 1;
      next = eoi + 1;
      old = -1;
      continue;
    }
    if (code === eoi) break;
    if (old === -1) {
      out[op++] = suffix[code];
      old = code;
      first = suffix[code];
      continue;
    }
    const inCode = code;
    let sp = 0;
    if (code >= next) {
      stack[sp++] = first;
      code = old;
    }
    while (code > clear) {
      stack[sp++] = suffix[code];
      code = prefix[code];
    }
    first = suffix[code];
    stack[sp++] = first;
    if (next < 4096) {
      prefix[next] = old;
      suffix[next] = first;
      next++;
      if ((next & codeMask) === 0 && next < 4096) {
        codeSize++;
        codeMask = (1 << codeSize) - 1;
      }
    }
    old = inCode;
    while (sp > 0 && op < pixelCount) out[op++] = stack[--sp];
  }
  return out;
}

/** Decodes a GIF into fully composited RGBA frames: {width, height, frames: [{rgba, delayMs}]}. */
export function decodeGif(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const sig = String.fromCharCode(...b.subarray(0, 6));
  if (sig !== "GIF87a" && sig !== "GIF89a") throw new Error("Это не GIF-файл");
  const width = b[6] | (b[7] << 8);
  const height = b[8] | (b[9] << 8);
  if (!width || !height) throw new Error("Пустой GIF");
  const flags = b[10];
  let pos = 13;
  let globalTable = null;
  if (flags & 0x80) {
    const n = 2 << (flags & 7);
    globalTable = b.subarray(pos, pos + n * 3);
    pos += n * 3;
  }
  const canvas = new Uint8ClampedArray(width * height * 4); // starts transparent
  const frames = [];
  let gce = { disposal: 0, transparent: -1, delay: 0 };
  while (pos < b.length) {
    const block = b[pos++];
    if (block === 0x3b) break; // trailer
    if (block === 0x21) {
      const label = b[pos++];
      if (label === 0xf9 && b[pos] >= 4) {
        const packed = b[pos + 1];
        gce = {
          disposal: (packed >> 2) & 7,
          transparent: packed & 1 ? b[pos + 4] : -1,
          delay: (b[pos + 2] | (b[pos + 3] << 8)) * 10,
        };
      }
      while (pos < b.length && b[pos] !== 0) pos += b[pos] + 1; // skip sub-blocks
      pos++;
      continue;
    }
    if (block !== 0x2c) break; // unknown: stop
    const ix = b[pos] | (b[pos + 1] << 8);
    const iy = b[pos + 2] | (b[pos + 3] << 8);
    const iw = b[pos + 4] | (b[pos + 5] << 8);
    const ih = b[pos + 6] | (b[pos + 7] << 8);
    const iflags = b[pos + 8];
    pos += 9;
    let table = globalTable;
    if (iflags & 0x80) {
      const n = 2 << (iflags & 7);
      table = b.subarray(pos, pos + n * 3);
      pos += n * 3;
    }
    const minCode = b[pos++];
    const chunks = [];
    let total = 0;
    while (pos < b.length && b[pos] !== 0) {
      const len = b[pos];
      chunks.push(b.subarray(pos + 1, pos + 1 + len));
      total += len;
      pos += len + 1;
    }
    pos++;
    const data = new Uint8Array(total);
    let o = 0;
    for (const c of chunks) {
      data.set(c, o);
      o += c.length;
    }
    const indices = lzwDecode(minCode, data, iw * ih);
    // "Restore previous" on the very first frame has nothing to restore: keep it (as Pillow does).
    const before = gce.disposal === 3 && frames.length ? canvas.slice() : null;
    // Row order (interlaced GIFs store rows in 4 passes).
    const rows = [];
    if (iflags & 0x40) {
      for (const [start, step] of [[0, 8], [4, 8], [2, 4], [1, 2]]) for (let y = start; y < ih; y += step) rows.push(y);
    } else {
      for (let y = 0; y < ih; y++) rows.push(y);
    }
    if (table) {
      for (let r = 0; r < ih; r++) {
        const y = iy + rows[r];
        if (y >= height) continue;
        for (let x = 0; x < iw; x++) {
          const cx = ix + x;
          if (cx >= width) continue;
          const ci = indices[r * iw + x];
          if (ci === gce.transparent || ci * 3 + 2 >= table.length) continue;
          const p = (y * width + cx) * 4;
          canvas[p] = table[ci * 3];
          canvas[p + 1] = table[ci * 3 + 1];
          canvas[p + 2] = table[ci * 3 + 2];
          canvas[p + 3] = 255;
        }
      }
    }
    let delay = gce.delay;
    if (delay < 20) delay = 100; // like browsers
    frames.push({ rgba: canvas.slice(), delayMs: delay });
    if (gce.disposal === 2) {
      for (let y = iy; y < Math.min(height, iy + ih); y++) canvas.fill(0, (y * width + ix) * 4, (y * width + Math.min(width, ix + iw)) * 4);
    } else if (gce.disposal === 3 && before) {
      canvas.set(before);
    }
    gce = { disposal: 0, transparent: -1, delay: 0 };
  }
  if (!frames.length) throw new Error("В GIF нет кадров");
  return { width, height, frames };
}

// ---------------------------------------------------------------- crop & scale
/**
 * A 2:1 crop rectangle (in source pixels) for a preset:
 *   "contain": the smallest 2:1 rectangle around the whole image (borders show the background);
 *   "cover":   the largest 2:1 rectangle inside the image (edges are cut off).
 */
export function presetCrop(width, height, mode) {
  const target = W / H;
  const wide = width / height > target;
  let w, h;
  if (mode === "cover") [w, h] = wide ? [height * target, height] : [width, width / target];
  else [w, h] = wide ? [width, width / target] : [height * target, height];
  return { x: (width - w) / 2, y: (height - h) / 2, w, h };
}

/**
 * Renders the crop rectangle of an RGBA source frame onto the 160x80 screen.
 * fit: "stretch" fills 160x80 with the crop; "contain" keeps the crop's aspect with borders.
 * The crop may extend past the image: that area shows the background colour.
 * Returns RGB bytes (160*80*3), already rounded to what the RGB565 panel can show.
 */
export function renderToScreen(src, srcW, srcH, crop, { fit = "contain", smooth = true, bg = [0, 0, 0] } = {}) {
  let dx = 0, dy = 0, dw = W, dh = H;
  if (fit === "contain") {
    const s = Math.min(W / crop.w, H / crop.h);
    dw = Math.max(1, Math.round(crop.w * s));
    dh = Math.max(1, Math.round(crop.h * s));
    dx = Math.floor((W - dw) / 2);
    dy = Math.floor((H - dh) / 2);
  }
  const sx = crop.w / dw, sy = crop.h / dh;
  const out = new Uint8Array(W * H * 3);
  const sample = (x, y) => {
    // RGBA at integer source pixel, transparent outside the image.
    if (x < 0 || y < 0 || x >= srcW || y >= srcH) return null;
    return (y * srcW + x) * 4;
  };
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      let r = bg[0], g = bg[1], b = bg[2];
      if (x >= dx && x < dx + dw && y >= dy && y < dy + dh) {
        let ar = 0, ag = 0, ab = 0, aa = 0, wsum = 0;
        const fx0 = crop.x + (x - dx) * sx, fy0 = crop.y + (y - dy) * sy;
        if (!smooth || (sx <= 1 && sy <= 1)) {
          // nearest (or upscaling: sample the pixel centre)
          const p = sample(Math.floor(fx0 + sx / 2), Math.floor(fy0 + sy / 2));
          if (p !== null) {
            const a = src[p + 3] / 255;
            ar = src[p] * a; ag = src[p + 1] * a; ab = src[p + 2] * a; aa = a; wsum = 1;
          } else wsum = 1;
        } else {
          // area average over the source footprint (box filter with fractional edges)
          const fx1 = fx0 + sx, fy1 = fy0 + sy;
          for (let yy = Math.floor(fy0); yy < Math.ceil(fy1); yy++) {
            const wy = Math.min(fy1, yy + 1) - Math.max(fy0, yy);
            for (let xx = Math.floor(fx0); xx < Math.ceil(fx1); xx++) {
              const wx = Math.min(fx1, xx + 1) - Math.max(fx0, xx);
              const wgt = wx * wy;
              wsum += wgt;
              const p = sample(xx, yy);
              if (p === null) continue;
              const a = (src[p + 3] / 255) * wgt;
              ar += src[p] * a; ag += src[p + 1] * a; ab += src[p + 2] * a; aa += a;
            }
          }
        }
        if (wsum > 0) {
          const cover = aa / wsum;
          // premultiplied average composited over the background
          r = ar / wsum + bg[0] * (1 - cover);
          g = ag / wsum + bg[1] * (1 - cover);
          b = ab / wsum + bg[2] * (1 - cover);
        }
      }
      const o = (y * W + x) * 3;
      out[o] = round5(r);
      out[o + 1] = round6(g);
      out[o + 2] = round5(b);
    }
  }
  return out;
}

const clamp8 = (v) => (v < 0 ? 0 : v > 255 ? 255 : Math.round(v));
const round5 = (v) => { const c = clamp8(v) >> 3; return (c << 3) | (c >> 2); };
const round6 = (v) => { const c = clamp8(v) >> 2; return (c << 2) | (c >> 4); };
export const rgb565 = (r, g, b) => ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
export function rgbFrom565(v) {
  const r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
}

// ---------------------------------------------------------------- colour selection
const dist = (a, b) => 2 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 3 * (a[2] - b[2]) ** 2;

/** Up to k representative colours of [[count, [r,g,b]]]: frequent colours plus distinct details. */
function cluster(counts, k) {
  counts.sort((a, b) => b[0] - a[0]);
  let centres = [counts[0][1]];
  while (centres.length < k) {
    let best = null, bestScore = -1;
    for (const [n, c] of counts) {
      let d = Infinity;
      for (const ce of centres) d = Math.min(d, dist(c, ce));
      const score = n ** 0.3 * d;
      if (score > bestScore) { bestScore = score; best = c; }
    }
    centres.push(best);
  }
  for (let it = 0; it < 4; it++) {
    const sums = centres.map(() => [0, 0, 0, 0]);
    for (const [n, c] of counts) {
      let bi = 0, bd = Infinity;
      for (let j = 0; j < centres.length; j++) {
        const d = dist(c, centres[j]);
        if (d < bd) { bd = d; bi = j; }
      }
      const s = sums[bi];
      s[0] += n; s[1] += n * c[0]; s[2] += n * c[1]; s[3] += n * c[2];
    }
    centres = centres.map((c, j) => (sums[j][0] ? [Math.round(sums[j][1] / sums[j][0]), Math.round(sums[j][2] / sums[j][0]), Math.round(sums[j][3] / sums[j][0])] : c));
  }
  return centres;
}

/** Best 4 RGB565 colours (sorted) for the pixels of one band, given as RGB bytes. */
function pickPalette(rgb, from, to) {
  const map = new Map();
  for (let i = from; i < to; i += 3) {
    const key = (rgb[i] << 16) | (rgb[i + 1] << 8) | rgb[i + 2];
    map.set(key, (map.get(key) || 0) + 1);
  }
  let colours;
  if (map.size <= 4) {
    colours = [...map.keys()].map((k) => [k >> 16, (k >> 8) & 255, k & 255]);
  } else {
    colours = cluster([...map.entries()].map(([k, n]) => [n, [k >> 16, (k >> 8) & 255, k & 255]]), 4);
  }
  const pal = [...new Set(colours.map((c) => rgb565(c[0], c[1], c[2])))].sort((a, b) => a - b);
  while (pal.length < 4) pal.push(pal[pal.length - 1]);
  return pal;
}

// ---------------------------------------------------------------- packing
export function rleEncode(data) {
  const out = [];
  const literal = [];
  const flush = () => {
    while (literal.length) {
      const chunk = literal.splice(0, 128);
      out.push(chunk.length - 1, ...chunk);
    }
  };
  for (let i = 0; i < data.length; ) {
    let run = 1;
    while (i + run < data.length && data[i + run] === data[i] && run < 129) run++;
    if (run >= 3) {
      flush();
      out.push(126 + run, data[i]);
    } else {
      for (let k = 0; k < run; k++) literal.push(data[i + k]);
    }
    i += run;
  }
  flush();
  return Uint8Array.from(out);
}

/** Mirror of draw_gif.cpp unpack(). */
export function rleApply(src, out, xorOver) {
  let pos = 0, i = 0;
  while (pos < FRAME_BYTES && i < src.length) {
    const c = src[i++];
    let count = c < 128 ? c + 1 : c - 126;
    if (count > FRAME_BYTES - pos) count = FRAME_BYTES - pos;
    if (c < 128) {
      for (let k = 0; k < count && i < src.length; k++, pos++) {
        const v = src[i++];
        out[pos] = xorOver ? out[pos] ^ v : v;
      }
    } else if (i < src.length) {
      const v = src[i++];
      for (let k = 0; k < count; k++, pos++) out[pos] = xorOver ? out[pos] ^ v : v;
    }
  }
}

// ---------------------------------------------------------------- animation encoding
/**
 * Encodes screen frames [{rgb (160*80*3), delayMs}] into one animation blob (see draw_gif.cpp).
 * Returns {bytes, frames, totalMs, colours}.
 */
export function encodeAnimation(frames, { bandHeight = DEFAULT_BAND_HEIGHT } = {}) {
  const bandStarts = [];
  for (let y = 0; y < H; y += bandHeight) bandStarts.push(y);
  if (bandStarts.length > 40) throw new Error("bandHeight must be >= 2");

  // 1. quantize + pack every frame; merge identical consecutive frames
  const q = [];
  for (const f of frames) {
    const pals = [];
    const packed = new Uint8Array(FRAME_BYTES);
    for (let b = 0; b < bandStarts.length; b++) {
      const y0 = bandStarts[b], y1 = b + 1 < bandStarts.length ? bandStarts[b + 1] : H;
      const pal = pickPalette(f.rgb, y0 * W * 3, y1 * W * 3);
      pals.push(pal);
      const palRgb = pal.map(rgbFrom565);
      const cache = new Map();
      for (let y = y0; y < y1; y++) {
        for (let x = 0; x < W; x++) {
          const o = (y * W + x) * 3;
          const key = (f.rgb[o] << 16) | (f.rgb[o + 1] << 8) | f.rgb[o + 2];
          let idx = cache.get(key);
          if (idx === undefined) {
            const c = [f.rgb[o], f.rgb[o + 1], f.rgb[o + 2]];
            let bd = Infinity;
            idx = 0;
            for (let j = 0; j < 4; j++) {
              const d = dist(c, palRgb[j]);
              if (d < bd) { bd = d; idx = j; }
            }
            cache.set(key, idx);
          }
          packed[y * ROW_BYTES + (x >> 2)] |= idx << ((x & 3) * 2);
        }
      }
    }
    const palKey = pals.map((p) => p.join(",")).join(";");
    const delay = Math.max(20, Math.min(65535, Math.round(f.delayMs)));
    const last = q[q.length - 1];
    if (last && last.palKey === palKey && last.delay + delay <= 65535 && last.packed.every((v, i) => v === packed[i])) {
      last.delay += delay;
    } else {
      q.push({ packed, pals, palKey, delay });
    }
  }

  // 2. tables
  const colours = [], colourIds = new Map();
  const rows = [], rowIds = new Map();
  const blobs = [], blobIds = new Map();
  const entries = [];
  const key = q[0].packed;
  q.forEach((f, i) => {
    const row = f.pals.map((p) => {
      const k = p.join(",");
      if (!colourIds.has(k)) { colourIds.set(k, colours.length); colours.push(p); }
      return colourIds.get(k);
    });
    const rk = row.join(",");
    if (!rowIds.has(rk)) { rowIds.set(rk, rows.length); rows.push(row); }
    const whole = rleEncode(f.packed);
    let blob = whole, xor = 0;
    if (i > 0) {
      const x = new Uint8Array(FRAME_BYTES);
      for (let j = 0; j < FRAME_BYTES; j++) x[j] = f.packed[j] ^ key[j];
      const patch = rleEncode(x);
      if (patch.length < whole.length) { blob = patch; xor = 1; }
    }
    const bk = xor + ":" + blob.join(",");
    if (!blobIds.has(bk)) { blobIds.set(bk, blobs.length); blobs.push(blob); }
    entries.push({ blob: blobIds.get(bk), ms: f.delay, row: rowIds.get(rk), xor });
  });

  // 3. serialize
  const indexSize = colours.length <= 256 ? 1 : 2;
  const align4 = (n) => (n + 3) & ~3;
  const headerSize = align4(32 + bandStarts.length);
  const coloursOffset = headerSize;
  const rowsOffset = coloursOffset + colours.length * 8;
  const framesOffset = align4(rowsOffset + rows.length * bandStarts.length * indexSize);
  const dataOffset = framesOffset + entries.length * 12;
  const blobOffsets = [];
  let dataSize = 0;
  for (const b of blobs) { blobOffsets.push(dataSize); dataSize += b.length; }
  const total = align4(dataOffset + dataSize);
  const bytes = new Uint8Array(total);
  const dv = new DataView(bytes.buffer);
  const totalMs = entries.reduce((s, e) => s + e.ms, 0);
  dv.setUint16(0, entries.length, true);
  bytes[2] = bandStarts.length;
  bytes[3] = indexSize;
  dv.setUint32(4, totalMs, true);
  dv.setUint16(8, colours.length, true);
  dv.setUint16(10, rows.length, true);
  dv.setUint32(12, coloursOffset, true);
  dv.setUint32(16, rowsOffset, true);
  dv.setUint32(20, framesOffset, true);
  dv.setUint32(24, dataOffset, true);
  dv.setUint32(28, dataSize, true);
  bandStarts.forEach((y, i) => (bytes[32 + i] = y));
  colours.forEach((p, i) => p.forEach((c, j) => dv.setUint16(coloursOffset + i * 8 + j * 2, c, true)));
  rows.forEach((row, i) => row.forEach((v, j) => {
    const o = rowsOffset + (i * bandStarts.length + j) * indexSize;
    if (indexSize === 1) bytes[o] = v; else dv.setUint16(o, v, true);
  }));
  entries.forEach((e, i) => {
    const o = framesOffset + i * 12;
    dv.setUint32(o, blobOffsets[e.blob], true);
    dv.setUint16(o + 4, e.ms, true);
    dv.setUint16(o + 6, e.row, true);
    bytes[o + 8] = e.xor;
  });
  blobs.forEach((b, i) => bytes.set(b, dataOffset + blobOffsets[i]));
  return { bytes, frames: entries.length, totalMs, colours: colours.length };
}

/** Drops every other frame (its time goes to the frame kept before it). */
export function dropEveryOther(frames) {
  const out = [];
  for (let i = 0; i < frames.length; i += 2) {
    const delay = frames[i].delayMs + (i + 1 < frames.length ? frames[i + 1].delayMs : 0);
    out.push({ ...frames[i], delayMs: Math.min(65535, delay) });
  }
  return out;
}

// ---------------------------------------------------------------- container
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** Builds the asset container from animation blobs ({standby, boot}, either may be null). */
export function buildContainer({ standby = null, boot = null }) {
  const parts = [standby, boot];
  let size = 32;
  const offsets = parts.map((p) => {
    if (!p) return 0;
    const o = size;
    size += p.length; // blobs are multiples of 4
    return o;
  });
  const out = new Uint8Array(size);
  const dv = new DataView(out.buffer);
  out.set([..."HS02GIF1"].map((c) => c.charCodeAt(0)), 0);
  dv.setUint32(8, size, true);
  dv.setUint32(16, offsets[0], true);
  dv.setUint32(20, offsets[1], true);
  parts.forEach((p, i) => p && out.set(p, offsets[i]));
  dv.setUint32(12, crc32(out.subarray(16)), true);
  return out;
}

/**
 * Encodes both animations and shrinks them (dropping every other frame of the bigger one) until
 * the container fits `maxBytes`. Inputs: {standby, boot} = screen frame lists or null.
 * Returns {container, standby: info, boot: info, dropped: {standby, boot}}.
 */
export function encodeToFit({ standby = null, boot = null }, maxBytes, opts = {}) {
  let sf = standby, bf = boot;
  const dropped = { standby: 0, boot: 0 };
  for (;;) {
    const s = sf && sf.length ? encodeAnimation(sf, opts) : null;
    const b = bf && bf.length ? encodeAnimation(bf, opts) : null;
    const container = buildContainer({ standby: s && s.bytes, boot: b && b.bytes });
    if (container.length <= maxBytes) return { container, standby: s, boot: b, dropped };
    const sBig = s && (!b || s.bytes.length >= b.bytes.length);
    if (sBig && sf.length > 1) { sf = dropEveryOther(sf); dropped.standby++; }
    else if (b && bf.length > 1) { bf = dropEveryOther(bf); dropped.boot++; }
    else if (s && sf.length > 1) { sf = dropEveryOther(sf); dropped.standby++; }
    else throw new Error(`GIF не помещается: нужно ${container.length} байт, свободно ${maxBytes}. Упростите картинку или уберите вторую GIF.`);
  }
}

// ---------------------------------------------------------------- firmware patching
const HEADER = 16;

/** Reads a FNIRSI firmware file (with its 16-byte bootloader header) and its asset slot. */
export function parseFirmware(fileBytes) {
  const f = fileBytes instanceof Uint8Array ? fileBytes : new Uint8Array(fileBytes);
  if (f.length < HEADER + 1024) throw new Error("Файл слишком маленький для прошивки");
  const tag = String.fromCharCode(f[0], f[1], f[2]);
  const dv = new DataView(f.buffer, f.byteOffset, f.byteLength);
  const declared = dv.getUint32(4, true);
  if (f[3] !== 0 || tag.toLowerCase() !== "bin" || declared !== f.length - HEADER) {
    throw new Error("Нужен файл прошивки HS-02 с заголовком загрузчика (HS02A_RU.bin и т.п. из релизов)");
  }
  const payload = f.subarray(HEADER);
  const needle = [..."HS02SLOT"].map((c) => c.charCodeAt(0));
  let at = -1;
  outer: for (let i = 0; i + 28 <= payload.length; i += 4) {
    for (let k = 0; k < 8; k++) if (payload[i + k] !== needle[k]) continue outer;
    at = i;
    break;
  }
  if (at < 0) throw new Error("В этой прошивке нет места для GIF: нужна прошивка HS-02 Modern UI с поддержкой GIF");
  const pv = new DataView(payload.buffer, payload.byteOffset + at, 24);
  const version = pv.getUint32(8, true);
  const assetsStart = pv.getUint32(12, true);
  const assetsLimit = pv.getUint32(16, true);
  const imageBase = pv.getUint32(20, true);
  const imageSize = assetsStart - imageBase;
  if (version !== 1 || imageSize <= 0 || imageSize > payload.length || assetsLimit <= assetsStart) {
    throw new Error("Неизвестный формат слота GIF в прошивке");
  }
  const existing = readContainer(payload.subarray(imageSize));
  return { tag, header: f.subarray(0, HEADER), image: payload.subarray(0, imageSize), maxBytes: assetsLimit - assetsStart, existing };
}

/** Parses an asset container (or returns null); {standby, boot} are animation blobs. */
export function readContainer(bytes) {
  if (bytes.length < 32) return null;
  if (String.fromCharCode(...bytes.subarray(0, 8)) !== "HS02GIF1") return null;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const size = dv.getUint32(8, true);
  if (size > bytes.length || size < 32 || crc32(bytes.subarray(16, size)) !== dv.getUint32(12, true)) return null;
  const pick = (o, end) => (o ? bytes.subarray(o, end) : null);
  const so = dv.getUint32(16, true), bo = dv.getUint32(20, true);
  return {
    container: bytes.subarray(0, size),
    standby: pick(so, bo > so ? bo : size),
    boot: pick(bo, so > bo ? so : size),
  };
}

/** New firmware file = original image + container, with the bootloader header updated. */
export function patchFirmware(fw, container) {
  const payloadLen = fw.image.length + (container ? container.length : 0);
  const out = new Uint8Array(HEADER + payloadLen);
  out.set(fw.header, 0);
  new DataView(out.buffer).setUint32(4, payloadLen, true);
  out.set(fw.image, HEADER);
  if (container) out.set(container, HEADER + fw.image.length);
  return out;
}

// ---------------------------------------------------------------- preview (mirror of draw_gif.cpp)
/** Decodes an animation blob; returns {frames: [{rgb565: Uint16Array(160*80), ms}], totalMs}. */
export function decodeAnimation(anim) {
  const dv = new DataView(anim.buffer, anim.byteOffset, anim.byteLength);
  const frameCount = dv.getUint16(0, true);
  const bands = anim[2], indexSize = anim[3];
  const totalMs = dv.getUint32(4, true);
  const coloursOffset = dv.getUint32(12, true), rowsOffset = dv.getUint32(16, true);
  const framesOffset = dv.getUint32(20, true), dataOffset = dv.getUint32(24, true), dataSize = dv.getUint32(28, true);
  const bandStart = anim.subarray(32, 32 + bands);
  const data = anim.subarray(dataOffset, dataOffset + dataSize);
  const frameAt = (i) => ({ off: dv.getUint32(framesOffset + i * 12, true), ms: dv.getUint16(framesOffset + i * 12 + 4, true), row: dv.getUint16(framesOffset + i * 12 + 6, true), xor: anim[framesOffset + i * 12 + 8] });
  const f0 = frameAt(0);
  const out = [];
  for (let i = 0; i < frameCount; i++) {
    const f = frameAt(i);
    const buf = new Uint8Array(FRAME_BYTES);
    if (f.xor) {
      rleApply(data.subarray(f0.off), buf, false);
      rleApply(data.subarray(f.off), buf, true);
    } else rleApply(data.subarray(f.off), buf, false);
    const px = new Uint16Array(W * H);
    let band = 0;
    for (let y = 0; y < H; y++) {
      while (band + 1 < bands && y >= bandStart[band + 1]) band++;
      const ro = rowsOffset + (f.row * bands + band) * indexSize;
      const ci = indexSize === 1 ? anim[ro] : dv.getUint16(ro, true);
      for (let x = 0; x < W; x++) {
        const idx = (buf[y * ROW_BYTES + (x >> 2)] >> ((x & 3) * 2)) & 3;
        px[y * W + x] = dv.getUint16(coloursOffset + ci * 8 + idx * 2, true);
      }
    }
    out.push({ rgb565: px, ms: f.ms });
  }
  return { frames: out, totalMs };
}
