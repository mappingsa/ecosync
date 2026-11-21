/** Shrink a photo so its long edge is at most maxPx, as JPEG. Small files pass through. */
export async function shrinkImage(file: File, maxPx: number): Promise<Blob> {
  if (!file.type.startsWith("image/")) return file;
  const bitmap = await createImageBitmap(file);
  const scale = Math.min(1, maxPx / Math.max(bitmap.width, bitmap.height));
  if (scale === 1 && file.size < 600_000) return file;
  const canvas = document.createElement("canvas");
  canvas.width = Math.round(bitmap.width * scale);
  canvas.height = Math.round(bitmap.height * scale);
  canvas.getContext("2d")!.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
  return await new Promise<Blob>((ok, fail) =>
    canvas.toBlob((b) => (b ? ok(b) : fail(new Error("Could not process photo"))), "image/jpeg", 0.85),
  );
}

export const jpgName = (name: string) => name.replace(/\.[^.]+$/, "") + ".jpg";
