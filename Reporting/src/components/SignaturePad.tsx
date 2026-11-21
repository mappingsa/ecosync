import { useEffect, useRef, useState } from "react";

/** Finger, pen or mouse signature. Calls onChange with a PNG, or null when cleared. */
export function SignaturePad({ onChange }: { onChange: (png: Blob | null) => void }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const drawing = useRef(false);
  const [empty, setEmpty] = useState(true);

  useEffect(() => {
    const c = canvas.current!;
    const ctx = c.getContext("2d")!;
    ctx.lineWidth = 2.5;
    ctx.lineCap = "round";
    ctx.strokeStyle = "#111";
  }, []);

  const point = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const c = canvas.current!;
    const r = c.getBoundingClientRect();
    return [((e.clientX - r.left) * c.width) / r.width, ((e.clientY - r.top) * c.height) / r.height] as const;
  };

  const down = (e: React.PointerEvent<HTMLCanvasElement>) => {
    drawing.current = true;
    canvas.current!.setPointerCapture(e.pointerId);
    const ctx = canvas.current!.getContext("2d")!;
    const [x, y] = point(e);
    ctx.beginPath();
    ctx.moveTo(x, y);
  };
  const move = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!drawing.current) return;
    const ctx = canvas.current!.getContext("2d")!;
    const [x, y] = point(e);
    ctx.lineTo(x, y);
    ctx.stroke();
  };
  const up = () => {
    if (!drawing.current) return;
    drawing.current = false;
    setEmpty(false);
    canvas.current!.toBlob((b) => onChange(b), "image/png");
  };
  const clear = () => {
    const c = canvas.current!;
    c.getContext("2d")!.clearRect(0, 0, c.width, c.height);
    setEmpty(true);
    onChange(null);
  };

  return (
    <div className="sigpad">
      <canvas ref={canvas} width={600} height={180} onPointerDown={down} onPointerMove={move} onPointerUp={up} onPointerLeave={up} />
      <div className="sigrow">
        <span className="hint">{empty ? "Sign above" : "Signed"}</span>
        <button type="button" className="link" onClick={clear}>Clear</button>
      </div>
    </div>
  );
}
