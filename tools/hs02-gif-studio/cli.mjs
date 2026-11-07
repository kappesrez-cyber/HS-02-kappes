#!/usr/bin/env node
// Command line version of the web constructor (same encoder: hs02gif.mjs). CI uses it to put
// custom/standby.gif (and custom/boot.gif, if present) into the release firmware.
//
//   node tools/hs02-gif-studio/cli.mjs --firmware HS02A_RU_firmware.bin --standby custom/standby.gif \
//        [--boot custom/boot.gif] [--crop contain|cover] [--speed 1] [--bg #000000] [-o out.bin]
//   node tools/hs02-gif-studio/cli.mjs --standby a.gif --container assets.bin   (raw container only)
//
// Without -o the firmware file is updated in place.
import { readFileSync, writeFileSync } from "node:fs";
import { decodeGif, presetCrop, renderToScreen, encodeToFit, parseFirmware, patchFirmware } from "./hs02gif.mjs";

const args = process.argv.slice(2);
const opt = {};
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (!a.startsWith("-")) { console.error(`unexpected argument ${a}`); process.exit(2); }
  const key = a.replace(/^-+/, "");
  opt[key === "o" ? "out" : key] = args[++i];
}

function screenFrames(path) {
  if (!path) return null;
  const gif = decodeGif(readFileSync(path));
  const crop = presetCrop(gif.width, gif.height, opt.crop || "contain");
  const hex = (opt.bg || "#000000").replace("#", "");
  const bg = [0, 2, 4].map((i) => parseInt(hex.slice(i, i + 2), 16));
  const speed = Number(opt.speed || 1);
  return gif.frames.map((f) => ({
    rgb: renderToScreen(f.rgba, gif.width, gif.height, crop, { fit: "stretch", bg }),
    delayMs: f.delayMs / speed,
  }));
}

const standby = screenFrames(opt.standby);
const boot = screenFrames(opt.boot);
if (!standby && !boot) { console.error("nothing to do: give --standby and/or --boot"); process.exit(2); }

const fw = opt.firmware ? parseFirmware(readFileSync(opt.firmware)) : null;
const max = fw ? fw.maxBytes : Number(opt["max-bytes"] || 29000);
const r = encodeToFit({ standby, boot }, max);
const describe = (name, info, drops) => info && console.log(`  ${name}: ${info.frames} frames, ${(info.totalMs / 1000).toFixed(2)} s loop, ${info.bytes.length} bytes${drops ? ` (every other frame dropped ${drops}x to fit)` : ""}`);
console.log(`GIF container: ${r.container.length} of ${max} bytes`);
describe("standby", r.standby, r.dropped.standby);
describe("boot", r.boot, r.dropped.boot);
if (opt.container) writeFileSync(opt.container, r.container);
if (fw) {
  const out = opt.out || opt.firmware;
  writeFileSync(out, patchFirmware(fw, r.container));
  console.log(`wrote ${out}`);
}
