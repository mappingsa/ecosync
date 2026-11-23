// A small, forgiving XML reader for KML and GPX - the same in the browser and
// in tests. Namespace prefixes are dropped (kml:Placemark -> Placemark,
// gx:Track -> Track), which is what reading these formats needs.

export interface XNode {
  name: string;
  attrs: Record<string, string>;
  children: (XNode | string)[];
}

const ENT: Record<string, string> = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };
export const decode = (s: string) =>
  s.replace(/&(#x[0-9a-f]+|#\d+|\w+);/gi, (m, e: string) =>
    e[0] === "#" ? String.fromCodePoint(e[1] === "x" || e[1] === "X" ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10)) : ENT[e] ?? m);
const local = (n: string) => n.slice(n.indexOf(":") + 1);

export function parseXml(text: string): XNode {
  const root: XNode = { name: "#root", attrs: {}, children: [] };
  const stack = [root];
  let i = 0;
  const n = text.length;
  while (i < n) {
    const lt = text.indexOf("<", i);
    if (lt < 0) { pushText(text.slice(i)); break; }
    if (lt > i) pushText(text.slice(i, lt));
    if (text.startsWith("<!--", lt)) { i = end(text.indexOf("-->", lt), 3); continue; }
    if (text.startsWith("<![CDATA[", lt)) {
      const e = text.indexOf("]]>", lt);
      top().children.push(text.slice(lt + 9, e < 0 ? n : e));
      i = end(e, 3);
      continue;
    }
    if (text.startsWith("<?", lt)) { i = end(text.indexOf("?>", lt), 2); continue; }
    if (text.startsWith("<!", lt)) { i = end(text.indexOf(">", lt), 1); continue; }
    const gt = findTagEnd(lt);
    const tag = text.slice(lt + 1, gt);
    i = gt + 1;
    if (tag[0] === "/") {
      const name = local(tag.slice(1).trim());
      // close up to the matching element (forgives a missing close tag)
      for (let k = stack.length - 1; k > 0; k--) if (stack[k].name === name) { stack.length = k; break; }
      continue;
    }
    const self = tag.endsWith("/");
    const body = self ? tag.slice(0, -1) : tag;
    const m = body.match(/^\s*([^\s/>]+)/);
    if (!m) continue;
    const node: XNode = { name: local(m[1]), attrs: {}, children: [] };
    for (const a of body.slice(m[0].length).matchAll(/([^\s=]+)\s*=\s*("([^"]*)"|'([^']*)')/g)) node.attrs[local(a[1])] = decode(a[3] ?? a[4] ?? "");
    top().children.push(node);
    if (!self) stack.push(node);
  }
  return root;

  function top() { return stack[stack.length - 1]; }
  function pushText(s: string) { if (s.trim()) top().children.push(decode(s)); }
  function end(at: number, len: number) { return at < 0 ? n : at + len; }
  function findTagEnd(from: number) {
    let q = "";
    for (let k = from + 1; k < n; k++) {
      const c = text[k];
      if (q) { if (c === q) q = ""; }
      else if (c === '"' || c === "'") q = c;
      else if (c === ">") return k;
    }
    return n;
  }
}

export const kids = (x: XNode, name?: string): XNode[] =>
  x.children.filter((c): c is XNode => typeof c !== "string" && (!name || c.name === name));
export const kid = (x: XNode, name: string) => kids(x, name)[0];

/** Every descendant with this name (not inside another one of the same name). */
export function findAll(x: XNode, name: string, out: XNode[] = []): XNode[] {
  for (const c of kids(x)) {
    if (c.name === name) out.push(c);
    else findAll(c, name, out);
  }
  return out;
}

export function text(x: XNode | undefined): string {
  if (!x) return "";
  return x.children.map((c) => (typeof c === "string" ? c : text(c))).join("").trim();
}
