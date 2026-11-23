// Reads .zip files (zipped Shapefiles, KMZ) with the browser's own
// decompression (DecompressionStream "deflate-raw"), so no zip library.

export type Files = Map<string, Uint8Array>;

async function inflate(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([new Uint8Array(data)]).stream().pipeThrough(new DecompressionStream("deflate-raw" as CompressionFormat));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/** Every file in the zip: name (with folders) -> bytes. Folders and Mac "__MACOSX" extras are skipped. */
export async function unzip(buf: Uint8Array): Promise<Files> {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  // the end-of-central-directory record is in the last 64 KB
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("This doesn't look like a zip file.");
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const out: Files = new Map();
  const utf8 = new TextDecoder();
  for (let k = 0; k < count; k++) {
    if (dv.getUint32(p, true) !== 0x02014b50) throw new Error("The zip file is damaged.");
    const method = dv.getUint16(p + 10, true);
    const size = dv.getUint32(p + 20, true);
    const nameLen = dv.getUint16(p + 28, true), extraLen = dv.getUint16(p + 30, true), commentLen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = utf8.decode(buf.subarray(p + 46, p + 46 + nameLen));
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith("/") || name.startsWith("__MACOSX/") || /(^|\/)\._/.test(name)) continue;
    const start = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
    const raw = buf.subarray(start, start + size);
    if (method === 0) out.set(name, raw.slice());
    else if (method === 8) out.set(name, await inflate(raw));
    else throw new Error(`"${name}" in the zip uses a compression this can't read (method ${method}).`);
  }
  return out;
}
