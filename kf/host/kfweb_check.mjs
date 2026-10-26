// kfweb_check.mjs -- build/kfweb.wasm (the browser app's engine) driven from a script in node, for
// tools/check_kfweb.sh: the same commands as kf/host/kfweb_host.c, the output written as raw float32 to be
// compared with the native build's byte for byte. GNU AGPL-3.0-or-later.
//
//   node kf/host/kfweb_check.mjs build/kfweb.wasm in.wav script.txt out.f32
import { readFileSync, writeFileSync } from "node:fs";

const [wasmPath, wavPath, scriptPath, outPath] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const k = instance.exports;
const mem = () => k.memory.buffer;

// the WAV's first two channels, 16-bit PCM only (as kfweb_host.c)
const d = readFileSync(wavPath);
let fmt = 0, chans = 0, bits = 0, data = -1, dlen = 0;
for (let o = 12; o + 8 <= d.length;) {
  const cl = d.readUInt32LE(o + 4), id = d.toString("latin1", o, o + 4);
  if (id === "fmt ") {
    fmt = d.readUInt16LE(o + 8); chans = d.readUInt16LE(o + 10); bits = d.readUInt16LE(o + 22);
    if (fmt === 0xfffe) fmt = d.readUInt16LE(o + 32);
  } else if (id === "data") { data = o + 8; dlen = Math.min(cl, d.length - o - 8); }
  o += 8 + cl + (cl & 1);
}
if (fmt !== 1 || bits !== 16 || data < 0) throw new Error("want 16-bit PCM");
let frames = Math.floor(dlen / (2 * chans));
frames = Math.min(frames, k.kw_maxn());
const nch = Math.min(chans, 2);
for (let c = 0; c < nch; c++) {
  const p = new Int16Array(mem(), k.kw_pcm(c), frames);
  for (let i = 0; i < frames; i++) p[i] = d.readInt16LE(data + (i * chans + c) * 2);
}
k.kw_sample(frames, nch > 1 ? 1 : 0);
const lines = [`sample ${frames} ${nch}`];
const outs = [];
for (const raw of readFileSync(scriptPath, "utf8").split("\n")) {
  const w = raw.trim().split(/\s+/);
  if (!w[0] || w[0].startsWith("#")) continue;
  const a = Number(w[1] ?? 0), b = Number(w[2] ?? 0);
  if (w[0] === "analyze") lines.push(`analyze ${k.kw_analyze()}`);
  else if (w[0] === "thin") {
    const n = k.kw_thin(a | 0, b | 0);
    k.kw_set_cuts(n);
    const cuts = Array.from(new Int32Array(mem(), k.kw_cuts(), n));
    lines.push(`thin ${n} of ${k.kw_thin_max(a | 0)}:` + cuts.map((x) => " " + x).join(""));
  } else if (w[0] === "set") k.kw_set(a | 0, Math.fround(b));
  else if (w[0] === "trig") k.kw_trig(a | 0);
  else if (w[0] === "stop") k.kw_stop();
  else if (w[0] === "render") {
    let left = a | 0, pl = 0;
    while (left > 0) {
      const q = Math.min(left, 128);
      pl = k.kw_render(q);
      const l = new Float32Array(mem(), k.kw_out(0), q), r = new Float32Array(mem(), k.kw_out(1), q);
      const il = new Float32Array(2 * q);
      for (let i = 0; i < q; i++) { il[2 * i] = l[i]; il[2 * i + 1] = r[i]; }
      outs.push(Buffer.from(il.buffer));
      left -= q;
    }
    const s = (i) => k.kw_stat(i) >>> 0;
    lines.push(`render ${a | 0}: playing ${pl}, splices ${s(0)}, jumps ${s(1)}, borders ${s(2)}, forced ${s(3)}, ` +
      `keys ${s(4)}; head ${k.kw_head(0)} ${k.kw_head(1)}`);
  } else throw new Error("unknown command " + w[0]);
}
writeFileSync(outPath, Buffer.concat(outs));
console.log(lines.join("\n"));
