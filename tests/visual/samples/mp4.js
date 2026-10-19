// Writes H.264 chunks from a WebCodecs VideoEncoder into an MP4 file: one
// video track, one chunk, moov before mdat so playback starts right away.
// Expects the encoder's "avc" output (length-prefixed NAL units) and its
// avcC description, and chunks in presentation order (no B-frames).
"use strict";

const Mp4 = (() => {
  const ascii = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
  const u8 = (...values) => Uint8Array.from(values);
  const u16 = (v) => u8((v >> 8) & 255, v & 255);
  const u32 = (v) => u8((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
  const zeros = (n) => new Uint8Array(n);

  function concat(parts) {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let offset = 0;
    for (const p of parts) {
      out.set(p, offset);
      offset += p.length;
    }
    return out;
  }

  const box = (type, ...parts) => {
    const body = concat(parts);
    return concat([u32(body.length + 8), ascii(type), body]);
  };
  const fullBox = (type, version, flags, ...parts) =>
    box(type, u8(version, (flags >> 16) & 255, (flags >> 8) & 255, flags & 255), ...parts);
  const matrix = () => concat([0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000].map(u32));

  function moov({ width, height, timescale, delta, sizes, keys, description, offset }) {
    const count = sizes.length;
    const mediaDuration = count * delta;
    const movieDuration = Math.round((mediaDuration / timescale) * 1000);
    const avc1 = box("avc1",
      zeros(6), u16(1),                  // reserved, data reference index
      zeros(16),                         // pre-defined and reserved
      u16(width), u16(height),
      u32(0x00480000), u32(0x00480000),  // 72 dpi
      zeros(4), u16(1),                  // reserved, frame count
      zeros(32),                         // compressor name
      u16(0x0018), u16(0xffff),          // depth, pre-defined
      box("avcC", description),
      box("colr", ascii("nclx"), u16(1), u16(1), u16(1), u8(0)),  // BT.709, limited range
      box("pasp", u32(1), u32(1)));
    const stbl = box("stbl",
      fullBox("stsd", 0, 0, u32(1), avc1),
      fullBox("stts", 0, 0, u32(1), u32(count), u32(delta)),
      fullBox("stss", 0, 0, u32(keys.length), ...keys.map((k) => u32(k + 1))),
      fullBox("stsc", 0, 0, u32(1), u32(1), u32(count), u32(1)),
      fullBox("stsz", 0, 0, u32(0), u32(count), ...sizes.map(u32)),
      fullBox("stco", 0, 0, u32(1), u32(offset)));
    const minf = box("minf",
      fullBox("vmhd", 0, 1, zeros(8)),
      box("dinf", fullBox("dref", 0, 0, u32(1), fullBox("url ", 0, 1))),
      stbl);
    const mdia = box("mdia",
      fullBox("mdhd", 0, 0, zeros(8), u32(timescale), u32(mediaDuration), u16(0x55c4), zeros(2)),
      fullBox("hdlr", 0, 0, zeros(4), ascii("vide"), zeros(12), ascii("TEKITO video\0")),
      minf);
    const trak = box("trak",
      fullBox("tkhd", 0, 3, zeros(8), u32(1), zeros(4), u32(movieDuration), zeros(8),
        zeros(4), zeros(4), matrix(), u32(width << 16), u32(height << 16)),
      mdia);
    return box("moov",
      fullBox("mvhd", 0, 0, zeros(8), u32(1000), u32(movieDuration), u32(0x00010000), u16(0x0100),
        zeros(10), matrix(), zeros(24), u32(2)),
      trak);
  }

  // chunks: [{data: Uint8Array, key: boolean, timestamp: microseconds}]
  function write({ chunks, description, width, height, fps }) {
    for (let i = 1; i < chunks.length; i++) {
      if (chunks[i].timestamp <= chunks[i - 1].timestamp) throw new Error("chunks out of order");
    }
    const timescale = fps * 512;
    const info = {
      width, height, timescale, delta: 512, description,
      sizes: chunks.map((c) => c.data.length),
      keys: chunks.flatMap((c, i) => (c.key ? [i] : [])),
    };
    const ftyp = box("ftyp", ascii("isom"), u32(512), ascii("isomiso2avc1mp41"));
    const moovSize = moov({ ...info, offset: 0 }).length;
    const header = moov({ ...info, offset: ftyp.length + moovSize + 8 });
    const payload = chunks.map((c) => c.data);
    const mdatSize = payload.reduce((n, p) => n + p.length, 8);
    return new Blob([ftyp, header, u32(mdatSize), ascii("mdat"), ...payload], { type: "video/mp4" });
  }

  return { write };
})();
