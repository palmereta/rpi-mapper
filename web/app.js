'use strict';
// Administracion del mapper. El estado es un solo JSON que se manda entero al servidor
// en cada cambio (unos pocos KB); el renderer lo aplica en el cuadro siguiente.

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const COLORES = ['#3399ff', '#ff7319', '#4ce64c', '#e63399', '#e6d933', '#33e6e6', '#b366ff', '#ff4d4d'];

let S = null;            // estado
let INFO = { mode: '1280x720@60' };
let OW = 1280, OH = 720; // resolucion de salida
const deshacer = [];

// ---------- geometria (misma matematica que mapper.c) ----------
function homo(q) {
  const dx1 = q[1][0] - q[2][0], dx2 = q[3][0] - q[2][0], dx3 = q[0][0] - q[1][0] + q[2][0] - q[3][0];
  const dy1 = q[1][1] - q[2][1], dy2 = q[3][1] - q[2][1], dy3 = q[0][1] - q[1][1] + q[2][1] - q[3][1];
  const det = dx1 * dy2 - dx2 * dy1;
  let g = 0, h = 0;
  if (Math.abs(det) > 1e-9 && (Math.abs(dx3) > 1e-9 || Math.abs(dy3) > 1e-9)) {
    g = (dx3 * dy2 - dx2 * dy3) / det; h = (dx1 * dy3 - dx3 * dy1) / det;
  }
  return [q[1][0] - q[0][0] + g * q[1][0], q[3][0] - q[0][0] + h * q[3][0], q[0][0],
          q[1][1] - q[0][1] + g * q[1][1], q[3][1] - q[0][1] + h * q[3][1], q[0][1], g, h, 1];
}
function hmap(m, p) {
  let w = m[6] * p[0] + m[7] * p[1] + m[8];
  if (Math.abs(w) < 1e-6) w = 1e-6;
  return [(m[0] * p[0] + m[1] * p[1] + m[2]) / w, (m[3] * p[0] + m[4] * p[1] + m[5]) / w];
}
function inv3(m) {
  const [a, b, c, d, e, f, g, h, i] = m;
  const A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const det = a * A + b * B + c * C || 1e-12;
  return [A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
          B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
          C / det, -(a * h - b * g) / det, (a * e - b * d) / det];
}
function gp(s, c, r) {
  const G = s.grid;
  if (c < 0) { const a = gp(s, 0, r), b = gp(s, 1, r); return [2 * a[0] - b[0], 2 * a[1] - b[1]]; }
  if (c >= G.cols) { const a = gp(s, G.cols - 1, r), b = gp(s, G.cols - 2, r); return [2 * a[0] - b[0], 2 * a[1] - b[1]]; }
  if (r < 0) { const a = gp(s, c, 0), b = gp(s, c, 1); return [2 * a[0] - b[0], 2 * a[1] - b[1]]; }
  if (r >= G.rows) { const a = gp(s, c, G.rows - 1), b = gp(s, c, G.rows - 2); return [2 * a[0] - b[0], 2 * a[1] - b[1]]; }
  return G.pts[r * G.cols + c];
}
function cr(p0, p1, p2, p3, t) {
  const t2 = t * t, t3 = t2 * t;
  return [0, 1].map((k) => 0.5 * (2 * p1[k] + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
    (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3));
}
function gridEval(s, u, v) {
  const G = s.grid;
  const gx = u * (G.cols - 1), gy = v * (G.rows - 1);
  const c = Math.min(Math.floor(gx), G.cols - 2), r = Math.min(Math.floor(gy), G.rows - 2);
  const fx = gx - c, fy = gy - r;
  if (G.interp === 'bilinear') {
    const a = gp(s, c, r), b = gp(s, c + 1, r), d = gp(s, c, r + 1), e = gp(s, c + 1, r + 1);
    return [0, 1].map((k) => (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (d[k] * (1 - fx) + e[k] * fx) * fy);
  }
  const row = [0, 1, 2, 3].map((k) => cr(gp(s, c - 1, r - 1 + k), gp(s, c, r - 1 + k), gp(s, c + 1, r - 1 + k), gp(s, c + 2, r - 1 + k), fx));
  return cr(row[0], row[1], row[2], row[3], fy);
}
const mapST = (s, m, u, v) => hmap(m, gridEval(s, u, v));

// ---------- estado ----------
function nuevaSuperficie(i, entera) {
  const a = entera ? 0 : 0.25, b = entera ? 1 : 0.75;
  return {
    name: 'Superficie ' + (i + 1), enabled: true, content: 'video', color: hex2rgb(COLORES[i % 8]),
    src: [0, 0, 1, 1], corners: [[a, a], [b, a], [b, b], [a, b]],
    grid: { cols: 2, rows: 2, interp: 'bicubic', pts: [[0, 0], [1, 0], [0, 1], [1, 1]] },
    feather: [0, 0, 0, 0], opacity: 1, brightness: 1, contrast: 1, gamma: 1,
  };
}
function normalizar(st) {
  st.output = Object.assign({ mode: '1280x720' }, st.output);
  st.show = Object.assign({ selected: 0, point: -1, edit: 'corners', overlay: true, pattern: false, blackout: false }, st.show);
  st.media = Object.assign({ playlist: [], loop: true, volume: 80, mute: false, image_secs: 10, audio: 'auto' }, st.media);
  if (!Array.isArray(st.surfaces) || !st.surfaces.length) st.surfaces = [nuevaSuperficie(0, true)];
  st.surfaces = st.surfaces.map((s, i) => {
    const d = nuevaSuperficie(i, true);
    const o = Object.assign(d, s);
    o.grid = Object.assign({}, d.grid, s.grid || {});
    const G = o.grid;
    if (!Array.isArray(G.pts) || G.pts.length !== G.cols * G.rows) G.pts = gridPlana(G.cols, G.rows);
    return o;
  });
  if (st.show.selected >= st.surfaces.length) st.show.selected = 0;
  return st;
}
function gridPlana(cols, rows) {
  const p = [];
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) p.push([c / (cols - 1), r / (rows - 1)]);
  return p;
}
const sel = () => S.surfaces[S.show.selected];
function antes() {
  deshacer.push(JSON.stringify(S));
  if (deshacer.length > 60) deshacer.shift();
}
function undo() {
  if (!deshacer.length) return;
  S = JSON.parse(deshacer.pop());
  enviar(); pintarPanel(); dibujar();
}

// envio: como mucho un PUT en vuelo; si hubo cambios mientras tanto, se manda el ultimo
let enVuelo = false, pendiente = false;
async function enviar() {
  if (enVuelo) { pendiente = true; return; }
  enVuelo = true;
  try {
    const r = await fetch('/api/state', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(S) });
    if (!r.ok) throw new Error(r.status);
  } catch (e) { estado('sin conexión con la Pi', 'mal'); }
  enVuelo = false;
  if (pendiente) { pendiente = false; enviar(); }
}
function cambio() { enviar(); dibujar(); }

// ---------- lienzo ----------
const cv = $('#lienzo');
const ctx = cv.getContext('2d');
let K = 1, OX = 0, OY = 0, DPR = 1;
let shotImg = null, verSalida = false;

function medir() {
  DPR = window.devicePixelRatio || 1;
  const r = cv.getBoundingClientRect();
  cv.width = Math.max(1, r.width * DPR); cv.height = Math.max(1, r.height * DPR);
  K = Math.min(cv.width * 0.86 / OW, cv.height * 0.86 / OH);
  OX = (cv.width - OW * K) / 2; OY = (cv.height - OH * K) / 2;
}
const aLienzo = (p) => [OX + p[0] * OW * K, OY + p[1] * OH * K];
const aNorm = (x, y) => [(x - OX) / (OW * K), (y - OY) / (OH * K)];

function contorno(s, m, n) {
  const pts = [];
  for (let i = 0; i <= n; i++) pts.push(mapST(s, m, i / n, 0));
  for (let i = 0; i <= n; i++) pts.push(mapST(s, m, 1, i / n));
  for (let i = n; i >= 0; i--) pts.push(mapST(s, m, i / n, 1));
  for (let i = n; i >= 0; i--) pts.push(mapST(s, m, 0, i / n));
  return pts;
}
function manijas(s) {
  if (S.show.edit === 'grid') { const m = homo(s.corners); return s.grid.pts.map((p) => hmap(m, p)); }
  return s.corners;
}

function dibujar() {
  if (!S) return;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#0b0c0f'; ctx.fillRect(0, 0, cv.width, cv.height);
  ctx.fillStyle = '#000'; ctx.fillRect(OX, OY, OW * K, OH * K);
  if (verSalida && shotImg) ctx.drawImage(shotImg, OX, OY, OW * K, OH * K);
  ctx.strokeStyle = '#3a404b'; ctx.lineWidth = 1 * DPR; ctx.strokeRect(OX, OY, OW * K, OH * K);

  S.surfaces.forEach((s, i) => {
    const m = homo(s.corners), esSel = i === S.show.selected, col = COLORES[i % 8];
    const poly = contorno(s, m, 24).map(aLienzo);
    ctx.beginPath(); poly.forEach((p, k) => (k ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]))); ctx.closePath();
    if (!verSalida) { ctx.fillStyle = col + (esSel ? '22' : '12'); ctx.fill(); }
    ctx.globalAlpha = s.enabled ? 1 : 0.4;
    ctx.strokeStyle = col; ctx.lineWidth = (esSel ? 2 : 1) * DPR; ctx.stroke();
    if (esSel) {
      ctx.lineWidth = 1 * DPR; ctx.strokeStyle = col + 'aa';
      const G = s.grid, N = 24;
      for (let c = 1; c < G.cols - 1; c++) linea(s, m, (t) => [c / (G.cols - 1), t], N);
      for (let r = 1; r < G.rows - 1; r++) linea(s, m, (t) => [t, r / (G.rows - 1)], N);
    }
    ctx.globalAlpha = 1;
    ctx.fillStyle = col; ctx.font = `${12 * DPR}px system-ui`;
    const c0 = aLienzo(s.corners[0]); ctx.fillText(s.name, c0[0] + 6 * DPR, c0[1] + 16 * DPR);
  });

  const s = sel();
  if (!s) return;
  const big = S.show.edit !== 'grid';
  manijas(s).forEach((p, k) => {
    const [x, y] = aLienzo(p), r = (big ? 7 : 5) * DPR;
    ctx.beginPath(); ctx.arc(x, y, r + 2 * DPR, 0, 7); ctx.fillStyle = '#000'; ctx.fill();
    ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fillStyle = k === S.show.point ? '#ff3b3b' : '#fff'; ctx.fill();
  });
  infoPunto();
}
function linea(s, m, f, n) {
  ctx.beginPath();
  for (let i = 0; i <= n; i++) { const [u, v] = f(i / n); const p = aLienzo(mapST(s, m, u, v)); i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]); }
  ctx.stroke();
}
function infoPunto() {
  const s = sel(), k = S.show.point, el = $('#punto-info');
  const hs = manijas(s);
  if (k < 0 || k >= hs.length) { el.textContent = 'Sin punto seleccionado'; return; }
  const p = hs[k];
  const nombre = S.show.edit === 'grid' ? `Punto ${k + 1} (col ${k % s.grid.cols + 1}, fila ${Math.floor(k / s.grid.cols) + 1})` : ['Arriba izq.', 'Arriba der.', 'Abajo der.', 'Abajo izq.'][k];
  el.textContent = `${nombre}: x ${(p[0] * OW).toFixed(1)} px, y ${(p[1] * OH).toFixed(1)} px`;
}

function dentro(poly, x, y) {
  let d = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const [xi, yi] = poly[i], [xj, yj] = poly[j];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) d = !d;
  }
  return d;
}

let arrastre = null;
cv.addEventListener('pointerdown', (e) => {
  const r = cv.getBoundingClientRect(), x = (e.clientX - r.left) * DPR, y = (e.clientY - r.top) * DPR;
  const s = sel();
  let mejor = -1, dist = 18 * DPR;
  manijas(s).forEach((p, k) => { const q = aLienzo(p), d = Math.hypot(q[0] - x, q[1] - y); if (d < dist) { dist = d; mejor = k; } });
  if (mejor >= 0) {
    antes();
    S.show.point = mejor;
    const q = aLienzo(manijas(s)[mejor]);
    arrastre = { tipo: 'punto', k: mejor, dx: q[0] - x, dy: q[1] - y };
  } else {
    // la superficie de arriba bajo el puntero (la ultima dibujada)
    let hit = -1;
    for (let i = S.surfaces.length - 1; i >= 0; i--) {
      const sf = S.surfaces[i];
      if (dentro(contorno(sf, homo(sf.corners), 12).map(aLienzo), x, y)) { hit = i; break; }
    }
    if (hit < 0) { S.show.point = -1; cambio(); return; }
    if (hit !== S.show.selected) { S.show.selected = hit; S.show.point = -1; pintarPanel(); cambio(); return; }
    antes();
    arrastre = { tipo: 'mover', ini: aNorm(x, y), base: JSON.parse(JSON.stringify(s.corners)) };
  }
  cv.setPointerCapture(e.pointerId);
  cambio();
});
cv.addEventListener('pointermove', (e) => {
  if (!arrastre) return;
  const r = cv.getBoundingClientRect(), x = (e.clientX - r.left) * DPR, y = (e.clientY - r.top) * DPR;
  const s = sel();
  if (arrastre.tipo === 'punto') {
    const n = aNorm(x + arrastre.dx, y + arrastre.dy);
    moverPuntoA(s, arrastre.k, n);
  } else {
    const n = aNorm(x, y), dx = n[0] - arrastre.ini[0], dy = n[1] - arrastre.ini[1];
    s.corners = arrastre.base.map((c) => [c[0] + dx, c[1] + dy]);
  }
  cambio();
});
const soltar = () => { arrastre = null; };
cv.addEventListener('pointerup', soltar);
cv.addEventListener('pointercancel', soltar);

function moverPuntoA(s, k, n) {
  n = [Math.max(-0.5, Math.min(1.5, n[0])), Math.max(-0.5, Math.min(1.5, n[1]))];
  if (S.show.edit === 'grid') s.grid.pts[k] = hmap(inv3(homo(s.corners)), n);
  else s.corners[k] = n;
}
function empujar(dx, dy) {
  const s = sel(), k = S.show.point;
  const hs = manijas(s);
  if (k < 0 || k >= hs.length) return;
  antes();
  moverPuntoA(s, k, [hs[k][0] + dx / OW, hs[k][1] + dy / OH]);
  cambio();
}

document.addEventListener('keydown', (e) => {
  if (e.target.matches('input[type=text], input[type=number], select')) return;
  const paso = e.shiftKey ? 10 : e.altKey ? 0.2 : ($('#paso-fino').checked ? 0.2 : 1);
  const flechas = { ArrowLeft: [-paso, 0], ArrowRight: [paso, 0], ArrowUp: [0, -paso], ArrowDown: [0, paso] };
  if (flechas[e.key]) { e.preventDefault(); empujar(...flechas[e.key]); return; }
  if (e.key === 'Tab') {
    e.preventDefault();
    const n = manijas(sel()).length;
    S.show.point = ((S.show.point < 0 ? -1 : S.show.point) + (e.shiftKey ? n - 1 : 1)) % n;
    cambio(); return;
  }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); undo(); return; }
  if (e.target.matches('input')) return;
  if (e.key.toLowerCase() === 'b') { apagar(!S.show.blackout); return; }
  const t = { p: 'pattern', o: 'overlay' }[e.key.toLowerCase()];
  if (t) { S.show[t] = !S.show[t]; pintarToggles(); cambio(); }
});

// ---------- panel de superficie ----------
function hex2rgb(h) { return [1, 3, 5].map((i) => parseInt(h.substr(i, 2), 16) / 255); }
function rgb2hex(c) { return '#' + c.map((v) => Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16).padStart(2, '0')).join(''); }

for (let n = 2; n <= 9; n++) { $('#g-cols').add(new Option(n, n)); $('#g-rows').add(new Option(n, n)); }

function pintarToggles() {
  ['pattern', 'overlay', 'blackout'].forEach((k) => $('#t-' + k).classList.toggle('on', !!S.show[k]));
  $('#t-blackout').textContent = S.show.blackout ? 'Encender' : 'Apagar';
  $('#t-shot').classList.toggle('on', verSalida);
  $$('#modo-edit button').forEach((b) => b.classList.toggle('on', b.dataset.v === S.show.edit));
}

function pintarPanel() {
  pintarToggles();
  const L = $('#superficies');
  L.innerHTML = '';
  S.surfaces.forEach((s, i) => {
    const d = document.createElement('div');
    d.className = 'item' + (i === S.show.selected ? ' on' : '') + (s.enabled ? '' : ' apagada');
    d.innerHTML = `<span class="muestra" style="background:${COLORES[i % 8]}"></span><span></span>`;
    d.lastChild.textContent = s.name;
    d.onclick = () => { S.show.selected = i; S.show.point = -1; pintarPanel(); cambio(); };
    L.appendChild(d);
  });
  const s = sel();
  $('#s-nombre').value = s.name;
  $('#s-activa').checked = s.enabled;
  $('#s-contenido').value = s.content;
  $('#s-color').value = rgb2hex(s.color);
  $('#s-color-fila').style.display = s.content === 'color' ? '' : 'none';
  $('#g-cols').value = s.grid.cols; $('#g-rows').value = s.grid.rows; $('#g-interp').value = s.grid.interp;
  for (let k = 0; k < 4; k++) { ponerRango('#src' + k, s.src[k]); ponerRango('#fe' + k, s.feather[k]); }
  ['opacity', 'brightness', 'contrast', 'gamma'].forEach((k) => ponerRango('#a-' + k, s[k]));
  $('#m-volume').value = S.media.volume; $('#m-volume').nextElementSibling.value = S.media.volume;
  $('#m-mute').checked = S.media.mute; $('#m-audio').value = S.media.audio; $('#m-loop').checked = S.media.loop; $('#m-image').value = S.media.image_secs;
  pintarLista();
}
function ponerRango(id, v) { const el = $(id); el.value = v; el.nextElementSibling.value = (+v).toFixed(2); }

function rango(id, set) {
  const el = $(id);
  el.addEventListener('pointerdown', antes);
  el.addEventListener('keydown', antes);
  el.addEventListener('input', () => { el.nextElementSibling.value = (+el.value).toFixed(2); set(+el.value); cambio(); });
}
for (let k = 0; k < 4; k++) {
  rango('#src' + k, (v) => { sel().src[k] = v; });
  rango('#fe' + k, (v) => { sel().feather[k] = v; });
}
['opacity', 'brightness', 'contrast', 'gamma'].forEach((k) => rango('#a-' + k, (v) => { sel()[k] = v; }));
$('#a-reset').onclick = () => { antes(); Object.assign(sel(), { opacity: 1, brightness: 1, contrast: 1, gamma: 1 }); pintarPanel(); cambio(); };

$('#s-nombre').addEventListener('focus', antes);
$('#s-nombre').addEventListener('input', (e) => { sel().name = e.target.value; pintarPanelLista(); cambio(); });
function pintarPanelLista() { const it = $$('#superficies .item')[S.show.selected]; if (it) it.lastChild.textContent = sel().name; }
$('#s-activa').onchange = (e) => { antes(); sel().enabled = e.target.checked; pintarPanel(); cambio(); };
$('#s-contenido').onchange = (e) => { antes(); sel().content = e.target.value; pintarPanel(); cambio(); };
$('#s-color').addEventListener('focus', antes);
$('#s-color').oninput = (e) => { sel().color = hex2rgb(e.target.value); cambio(); };

function regrillar(cols, rows) {
  const s = sel();
  antes();
  // se conserva la deformacion: los puntos nuevos se toman de la superficie actual
  const pts = [];
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) pts.push(gridEval(s, c / (cols - 1), r / (rows - 1)));
  s.grid.cols = cols; s.grid.rows = rows; s.grid.pts = pts;
  S.show.point = -1;
  pintarPanel(); cambio();
}
$('#g-cols').onchange = (e) => regrillar(+e.target.value, sel().grid.rows);
$('#g-rows').onchange = (e) => regrillar(sel().grid.cols, +e.target.value);
$('#g-interp').onchange = (e) => { antes(); sel().grid.interp = e.target.value; cambio(); };
$('#g-reset').onclick = () => { antes(); const G = sel().grid; G.pts = gridPlana(G.cols, G.rows); cambio(); };
$('#c-reset').onclick = () => { antes(); sel().corners = [[0, 0], [1, 0], [1, 1], [0, 1]]; cambio(); };

$$('#modo-edit button').forEach((b) => (b.onclick = () => { S.show.edit = b.dataset.v; S.show.point = -1; pintarToggles(); cambio(); }));
$('#deshacer').onclick = undo;
$$('.pad button[data-d]').forEach((b) => (b.onclick = () => {
  const [dx, dy] = b.dataset.d.split(',').map(Number), f = $('#paso-fino').checked ? 0.2 : 1;
  empujar(dx * f, dy * f);
}));

$$('[data-tr]').forEach((b) => (b.onclick = () => {
  const s = sel(), [op, arg] = b.dataset.tr.split(':');
  antes();
  const px = s.corners.map((c) => [c[0] * OW, c[1] * OH]);
  const cx = px.reduce((a, p) => a + p[0], 0) / 4, cy = px.reduce((a, p) => a + p[1], 0) / 4;
  let out = px;
  if (op === 'rot') {
    const a = (+arg * Math.PI) / 180, co = Math.cos(a), si = Math.sin(a);
    out = px.map(([x, y]) => [cx + (x - cx) * co - (y - cy) * si, cy + (x - cx) * si + (y - cy) * co]);
  } else if (op === 'esc') {
    out = px.map(([x, y]) => [cx + (x - cx) * +arg, cy + (y - cy) * +arg]);
  } else if (op === 'centrar') {
    out = px.map(([x, y]) => [x - cx + OW / 2, y - cy + OH / 2]);
  } else if (op === 'flip') {
    // espejar el contenido es invertir el recorte: no mueve la geometria
    if (arg === 'h') [s.src[0], s.src[2]] = [s.src[2], s.src[0]];
    else [s.src[1], s.src[3]] = [s.src[3], s.src[1]];
  }
  s.corners = out.map(([x, y]) => [x / OW, y / OH]);
  pintarPanel(); cambio();
}));

$('#s-nueva').onclick = () => {
  if (S.surfaces.length >= 8) return alert('Máximo 8 superficies.');
  antes(); S.surfaces.push(nuevaSuperficie(S.surfaces.length, false));
  S.show.selected = S.surfaces.length - 1; S.show.point = -1; pintarPanel(); cambio();
};
$('#s-dup').onclick = () => {
  if (S.surfaces.length >= 8) return alert('Máximo 8 superficies.');
  antes();
  const c = JSON.parse(JSON.stringify(sel()));
  c.name += ' (copia)'; c.corners = c.corners.map(([x, y]) => [x + 0.03, y + 0.03]);
  S.surfaces.push(c); S.show.selected = S.surfaces.length - 1; pintarPanel(); cambio();
};
$('#s-borrar').onclick = () => {
  if (S.surfaces.length <= 1) return alert('Tiene que quedar al menos una superficie.');
  if (!confirm(`¿Borrar «${sel().name}»?`)) return;
  antes(); S.surfaces.splice(S.show.selected, 1); S.show.selected = Math.max(0, S.show.selected - 1); S.show.point = -1;
  pintarPanel(); cambio();
};

['pattern', 'overlay'].forEach((k) => ($('#t-' + k).onclick = () => { S.show[k] = !S.show[k]; pintarToggles(); cambio(); }));
// el apagado va por su propia ruta (también lo cambia el botón físico de la Pi)
async function apagar(v) {
  try { S.show.blackout = (await (await fetch('/api/blackout', { method: 'POST', body: JSON.stringify({ value: v }) })).json()).blackout; } catch (e) { /* nada */ }
  pintarToggles();
}
$('#t-blackout').onclick = () => apagar(!S.show.blackout);
async function vigilarApagado() {
  try {
    const b = (await (await fetch('/api/blackout')).json()).blackout;
    if (b !== !!S.show.blackout) { S.show.blackout = b; pintarToggles(); }
  } catch (e) { /* nada */ }
  setTimeout(vigilarApagado, 1500);
}
$('#t-shot').onclick = () => { verSalida = !verSalida; pintarToggles(); if (verSalida) traerCaptura(); else dibujar(); };
async function traerCaptura() {
  if (!verSalida) return;
  try {
    const r = await fetch('/api/shot.png');
    if (r.ok) {
      const img = new Image();
      img.src = URL.createObjectURL(await r.blob());
      await img.decode();
      if (shotImg) URL.revokeObjectURL(shotImg.src);
      shotImg = img; dibujar();
    }
  } catch (e) { /* se reintenta */ }
  setTimeout(traerCaptura, 1500);
}

$$('.tabs button').forEach((b) => (b.onclick = () => {
  $$('.tabs button').forEach((x) => x.classList.toggle('on', x === b));
  $$('.tab').forEach((t) => t.classList.toggle('on', t.id === 'tab-' + b.dataset.tab));
  if (b.dataset.tab === 'cont') traerArchivos();
}));

// ---------- contenido ----------
let ARCHIVOS = [], TRABAJOS = {};
const fmtTam = (b) => (b > 1e9 ? (b / 1e9).toFixed(1) + ' GB' : (b / 1e6).toFixed(1) + ' MB');
const fmtT = (s) => (s == null ? '--:--' : `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`);

async function traerArchivos() {
  const r = await fetch('/api/media');
  const j = await r.json();
  ARCHIVOS = j.files; TRABAJOS = j.jobs;
  const C = $('#archivos');
  C.innerHTML = '';
  ARCHIVOS.forEach((f) => {
    const d = document.createElement('div');
    d.className = 'archivo';
    const det = f.w ? `${f.w}×${f.h} ${f.codec || ''} ${f.fps ? f.fps + ' fps' : ''} ${f.dur ? fmtT(f.dur) : ''}` : '';
    d.innerHTML = `${f.thumb ? `<img loading="lazy" src="/api/thumb/${encodeURIComponent(f.name)}">` : '<div class="sin"></div>'}
      <div><div class="nombre"></div><div class="det">${det} · ${fmtTam(f.size)}</div></div>
      <div class="acc"><button title="Reproducir solo este">▶</button><button title="Agregar a la lista">+</button><button title="Borrar" class="peligro">✕</button></div>`;
    d.querySelector('.nombre').textContent = f.name;
    const [bPlay, bAdd, bDel] = d.querySelectorAll('button');
    bPlay.onclick = () => { antes(); S.media.playlist = [f.name]; pintarLista(); enviar(); };
    bAdd.onclick = () => { antes(); S.media.playlist.push(f.name); pintarLista(); enviar(); };
    bDel.onclick = async () => {
      if (!confirm(`¿Borrar ${f.name}?`)) return;
      await fetch('/api/media/' + encodeURIComponent(f.name), { method: 'DELETE' });
      S.media.playlist = S.media.playlist.filter((x) => x !== f.name);
      pintarLista(); traerArchivos();
    };
    C.appendChild(d);
  });
  const enCurso = Object.entries(TRABAJOS).filter(([, t]) => t.state === 'run' || t.state === 'queued');
  const errores = Object.entries(TRABAJOS).filter(([, t]) => t.state === 'error');
  const U = $('#subidas');
  U.querySelectorAll('.trabajo').forEach((e) => e.remove());
  [...enCurso, ...errores].forEach(([n, t]) => {
    const d = document.createElement('div');
    d.className = 'trabajo nota';
    d.textContent = t.state === 'error' ? `${n}: error al optimizar (${t.msg})` : `${n}: optimizando para la Pi${t.pct != null ? ` ${t.pct} %` : '…'}`;
    U.appendChild(d);
  });
  if (enCurso.length) setTimeout(traerArchivos, 2000);
}

function pintarLista() {
  const L = $('#lista');
  L.innerHTML = '';
  S.media.playlist.forEach((n, i) => {
    const li = document.createElement('li');
    li.innerHTML = '<span></span><button title="Subir">↑</button><button title="Bajar">↓</button><button title="Ir">▶</button><button title="Quitar">✕</button>';
    li.firstChild.textContent = n;
    const [up, dn, go, rm] = li.querySelectorAll('button');
    up.onclick = () => mover(i, -1);
    dn.onclick = () => mover(i, 1);
    go.onclick = () => fetch('/api/player', { method: 'POST', body: JSON.stringify({ action: 'goto', value: i }) });
    rm.onclick = () => { antes(); S.media.playlist.splice(i, 1); pintarLista(); enviar(); };
    L.appendChild(li);
  });
  if (!S.media.playlist.length) L.innerHTML = '<li class="nota">Vacía: agregá archivos de abajo.</li>';
}
function mover(i, d) {
  const p = S.media.playlist, j = i + d;
  if (j < 0 || j >= p.length) return;
  antes(); [p[i], p[j]] = [p[j], p[i]]; pintarLista(); enviar();
}

$('#archivo').onchange = (e) => { [...e.target.files].forEach(subir); e.target.value = ''; };
function subir(f) {
  const fila = document.createElement('div');
  fila.innerHTML = `<div class="nota"></div><div class="barra-prog"><div></div></div>`;
  fila.firstChild.textContent = `Subiendo ${f.name}…`;
  $('#subidas').appendChild(fila);
  const x = new XMLHttpRequest();
  x.open('PUT', '/api/media?name=' + encodeURIComponent(f.name));
  x.upload.onprogress = (ev) => { if (ev.lengthComputable) fila.lastChild.firstChild.style.width = (ev.loaded / ev.total) * 100 + '%'; };
  x.onload = () => {
    if (x.status !== 200) { fila.firstChild.textContent = `${f.name}: ${JSON.parse(x.responseText || '{}').error || 'error'}`; return; }
    fila.remove(); traerArchivos();
  };
  x.onerror = () => { fila.firstChild.textContent = `${f.name}: se cortó la subida`; };
  x.send(f);
}

$$('[data-pl]').forEach((b) => (b.onclick = () => fetch('/api/player', { method: 'POST', body: JSON.stringify({ action: b.dataset.pl }) })));
rango('#m-volume', (v) => { S.media.volume = v; });
$('#m-volume').addEventListener('input', (e) => { e.target.nextElementSibling.value = e.target.value; });
$('#m-mute').onchange = (e) => { S.media.mute = e.target.checked; enviar(); };
$('#m-audio').onchange = (e) => { S.media.audio = e.target.value; enviar(); };
$('#m-loop').onchange = (e) => { S.media.loop = e.target.checked; enviar(); };
$('#m-image').onchange = (e) => { S.media.image_secs = Math.max(1, +e.target.value || 10); enviar(); };

let tocandoPos = false;
$('#pos').addEventListener('pointerdown', () => (tocandoPos = true));
$('#pos').addEventListener('change', (e) => {
  tocandoPos = false;
  fetch('/api/player', { method: 'POST', body: JSON.stringify({ action: 'seek', value: +e.target.value }) });
});
async function traerReproductor() {
  if ($('#tab-cont').classList.contains('on')) {
    try {
      const p = await (await fetch('/api/player')).json();
      $('#ahora').textContent = p.filename ? `${p.pause ? '⏸' : '▶'} ${p.filename} · ${fmtT(p['time-pos'])} / ${fmtT(p.duration)}` : 'Nada en reproducción';
      if (!tocandoPos && p.duration) { $('#pos').max = p.duration; $('#pos').value = p['time-pos'] || 0; }
      $$('#lista li').forEach((li, i) => li.classList.toggle('actual', i === p['playlist-pos']));
    } catch (e) { /* nada */ }
  }
  setTimeout(traerReproductor, 1000);
}

// ---------- sistema ----------
async function traerInfo() {
  try {
    const i = await (await fetch('/api/info')).json();
    INFO = i;
    const m = /^(\d+)x(\d+)/.exec(i.mode || '');
    if (m && (+m[1] !== OW || +m[2] !== OH)) { OW = +m[1]; OH = +m[2]; medir(); dibujar(); }
    estado(i.renderer ? `${i.mode} · ${i.temp} °C` : 'renderer detenido', i.renderer ? 'ok' : 'mal');
    const sel = $('#o-mode');
    const modos = i.modes || [];
    if (sel.options.length !== modos.length) {
      sel.innerHTML = '';
      modos.forEach((x) => sel.add(new Option(x, x.split('@')[0] === x ? x : x)));
    }
    const pedido = S.output.mode;
    const op = [...sel.options].find((o) => o.value === pedido || o.value.split('@')[0] === pedido);
    if (op && document.activeElement !== sel) sel.value = op.value;
    const filas = [
      ['Salida', `${i.mode || '?'} ${i.connected ? '(conectado)' : '(sin proyector)'}`],
      ['Contenido', i.video && i.video[0] ? `${i.video[0]}×${i.video[1]} · decodificación ${i.hwdec}` : '—'],
      ['Audio', i.audio || '—'],
      ['Temperatura', `${i.temp} °C` + (i.throttled && i.throttled !== '0x0' ? ` · limitación ${i.throttled}` : '')],
      ['Carga', (i.load || []).join(' ')],
      ['Memoria', `${i.mem_used_mb} / ${i.mem_total_mb} MB`],
      ['Disco libre', `${i.disk_free_gb} GB`],
      ['Encendida hace', fmtT(i.uptime_s).replace(/:(\d+)$/, ' min')],
      ['Equipo', i.hostname],
    ];
    $('#info').innerHTML = filas.map(([a, b]) => `<tr><td>${a}</td><td>${b}</td></tr>`).join('');
  } catch (e) { estado('sin conexión con la Pi', 'mal'); }
  setTimeout(traerInfo, 3000);
}
function estado(t, c) { const e = $('#estado'); e.textContent = t; e.className = 'pill ' + (c || ''); }

$('#o-aplicar').onclick = () => {
  const v = $('#o-mode').value;
  if (!v) return;
  S.output.mode = v.replace(/@60$/, '');
  enviar();
  estado('reiniciando el renderer…');
};
$('#sys-restart').onclick = () => fetch('/api/system', { method: 'POST', body: JSON.stringify({ action: 'restart' }) });
$('#sys-reboot').onclick = () => confirm('¿Reiniciar la Raspberry?') && fetch('/api/system', { method: 'POST', body: JSON.stringify({ action: 'reboot' }) });
$('#sys-off').onclick = () => confirm('¿Apagar la Raspberry? Para volver a prenderla hay que desenchufarla y enchufarla.') &&
  fetch('/api/system', { method: 'POST', body: JSON.stringify({ action: 'poweroff' }) });
$('#exportar').onclick = () => {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(S, null, 1)], { type: 'application/json' }));
  a.download = `mapeo-${new Date().toISOString().slice(0, 16).replace(/[:T]/g, '-')}.json`;
  a.click();
};
$('#importar').onchange = async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  try {
    const j = JSON.parse(await f.text());
    if (!Array.isArray(j.surfaces)) throw new Error('no es un mapeo');
    antes(); S = normalizar(j); pintarPanel(); cambio();
  } catch (err) { alert('No se pudo importar: ' + err.message); }
  e.target.value = '';
};

// ---------- arranque ----------
async function iniciar() {
  S = normalizar(await (await fetch('/api/state')).json());
  medir(); pintarPanel(); dibujar();
  enviar();
  traerInfo(); traerReproductor(); vigilarApagado();
}
new ResizeObserver(() => { medir(); dibujar(); }).observe($('#lienzo-caja'));
iniciar();
