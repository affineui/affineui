/* sections-editors.jsx
   Color pickers · Curve · Graph · Texture pickers
*/
const { useState: useStateE, useRef: useRefE, useEffect: useEffectE } = React;

/* ─────────── Color helpers ─────────── */
function hsvToRgb(h, s, v) {
  const c = v * s;
  const hh = (h / 60) % 6;
  const x = c * (1 - Math.abs(hh % 2 - 1));
  let r=0, g=0, b=0;
  if (hh < 1) [r,g,b] = [c,x,0];
  else if (hh < 2) [r,g,b] = [x,c,0];
  else if (hh < 3) [r,g,b] = [0,c,x];
  else if (hh < 4) [r,g,b] = [0,x,c];
  else if (hh < 5) [r,g,b] = [x,0,c];
  else [r,g,b] = [c,0,x];
  const m = v - c;
  return [r+m, g+m, b+m];
}
function rgbHex([r,g,b]) {
  const f = n => Math.round(n*255).toString(16).padStart(2, '0');
  return `#${f(r)}${f(g)}${f(b)}`;
}

/* ─────────── Color wheel + triangle (HSV) ─────────── */
function ColorWheel({ size = 220 }) {
  const [h, setH] = useStateE(210);
  const [s, setS] = useStateE(0.7);
  const [v, setV] = useStateE(0.85);
  const wrapRef = useRefE(null);

  const cx = size / 2, cy = size / 2;
  const outerR = size / 2;
  const ringW = 18;
  const innerR = outerR - ringW;
  const triR = innerR - 6;

  // Triangle vertices: V1 = hue tip (rotates with h), V2 = white, V3 = black
  // hue=0 (red) at top; angles in SVG: 0° = right, 90° = down
  const a1 = (h - 90) * Math.PI / 180;
  const a2 = a1 + (2 * Math.PI / 3);
  const a3 = a1 + (4 * Math.PI / 3);
  const V1 = [cx + triR * Math.cos(a1), cy + triR * Math.sin(a1)];  // hue
  const V2 = [cx + triR * Math.cos(a2), cy + triR * Math.sin(a2)];  // white
  const V3 = [cx + triR * Math.cos(a3), cy + triR * Math.sin(a3)];  // black

  // Convert (s, v) → barycentric (a_hue, b_white, c_black) → point inside triangle
  // v = a + b, s = a / (a + b)
  const a = s * v;
  const b = (1 - s) * v;
  const c = 1 - v;
  const cursorX = a * V1[0] + b * V2[0] + c * V3[0];
  const cursorY = a * V1[1] + b * V2[1] + c * V3[1];

  const rgb = hsvToRgb(h, s, v);
  const hex = rgbHex(rgb);
  const hueColor = rgbHex(hsvToRgb(h, 1, 1));

  // Drag handlers
  const distFromCenter = (x, y) => Math.hypot(x - cx, y - cy);
  const handleDown = (e) => {
    const rect = wrapRef.current.getBoundingClientRect();
    const scale = size / rect.width;
    const startX = (e.clientX - rect.left) * scale;
    const startY = (e.clientY - rect.top) * scale;
    const d = distFromCenter(startX, startY);
    const mode = d > innerR ? 'hue' : 'sv';

    const updateHue = (cx0, cy0) => {
      const ang = Math.atan2(cy0 - cy, cx0 - cx);
      let deg = ang * 180 / Math.PI + 90;
      if (deg < 0) deg += 360;
      if (deg >= 360) deg -= 360;
      setH(deg);
    };
    const updateSV = (px, py) => {
      // Barycentric coords of point in triangle V1V2V3
      const denom = (V2[1] - V3[1]) * (V1[0] - V3[0]) + (V3[0] - V2[0]) * (V1[1] - V3[1]);
      let ba = ((V2[1] - V3[1]) * (px - V3[0]) + (V3[0] - V2[0]) * (py - V3[1])) / denom;
      let bb = ((V3[1] - V1[1]) * (px - V3[0]) + (V1[0] - V3[0]) * (py - V3[1])) / denom;
      let bc = 1 - ba - bb;
      // Clamp to triangle
      ba = Math.max(0, ba); bb = Math.max(0, bb); bc = Math.max(0, bc);
      const sum = ba + bb + bc;
      ba /= sum; bb /= sum; bc /= sum;
      const nv = ba + bb;
      const ns = nv > 0 ? ba / nv : 0;
      setS(Math.max(0, Math.min(1, ns)));
      setV(Math.max(0, Math.min(1, nv)));
    };

    if (mode === 'hue') updateHue(startX, startY);
    else updateSV(startX, startY);

    const move = (ev) => {
      const x = (ev.clientX - rect.left) * scale;
      const y = (ev.clientY - rect.top) * scale;
      if (mode === 'hue') updateHue(x, y);
      else updateSV(x, y);
    };
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  };

  // 60-segment hue ring (smooth)
  const segments = 60;
  const hueRing = Array.from({ length: segments }).map((_, i) => {
    const a1 = (i / segments) * 360 - 90 - 3;
    const a2 = ((i + 1) / segments) * 360 - 90 + 3;
    const x1 = cx + outerR * Math.cos(a1 * Math.PI / 180);
    const y1 = cy + outerR * Math.sin(a1 * Math.PI / 180);
    const x2 = cx + outerR * Math.cos(a2 * Math.PI / 180);
    const y2 = cy + outerR * Math.sin(a2 * Math.PI / 180);
    const ix1 = cx + innerR * Math.cos(a1 * Math.PI / 180);
    const iy1 = cy + innerR * Math.sin(a1 * Math.PI / 180);
    const ix2 = cx + innerR * Math.cos(a2 * Math.PI / 180);
    const iy2 = cy + innerR * Math.sin(a2 * Math.PI / 180);
    const color = rgbHex(hsvToRgb((i + 0.5) / segments * 360, 1, 1));
    return <path key={i} d={`M ${x1} ${y1} A ${outerR} ${outerR} 0 0 1 ${x2} ${y2} L ${ix2} ${iy2} A ${innerR} ${innerR} 0 0 0 ${ix1} ${iy1} Z`} fill={color} />;
  });

  // Hue indicator: a notch on the ring at the current hue
  const hueAng = (h - 90) * Math.PI / 180;
  const hueX1 = cx + (outerR + 2) * Math.cos(hueAng);
  const hueY1 = cy + (outerR + 2) * Math.sin(hueAng);
  const hueX2 = cx + (innerR - 2) * Math.cos(hueAng);
  const hueY2 = cy + (innerR - 2) * Math.sin(hueAng);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, width: size + 30 }}>
      <svg ref={wrapRef} viewBox={`0 0 ${size} ${size}`} style={{ width: '100%', display: 'block', cursor: 'crosshair', touchAction: 'none' }} onPointerDown={handleDown}>
        <defs>
          <linearGradient id="cw-white" x1={V1[0]} y1={V1[1]} x2={V2[0]} y2={V2[1]} gradientUnits="userSpaceOnUse">
            <stop offset="0%" stopColor={hueColor} stopOpacity="0" />
            <stop offset="100%" stopColor="#fff" />
          </linearGradient>
          <linearGradient id="cw-black"
            x1={(V1[0] + V2[0]) / 2} y1={(V1[1] + V2[1]) / 2}
            x2={V3[0]} y2={V3[1]} gradientUnits="userSpaceOnUse">
            <stop offset="0%" stopColor="#000" stopOpacity="0" />
            <stop offset="100%" stopColor="#000" />
          </linearGradient>
        </defs>

        {/* Hue ring */}
        {hueRing}

        {/* Hue thumb (notch) */}
        <line x1={hueX1} y1={hueY1} x2={hueX2} y2={hueY2} stroke="#fff" strokeWidth="2.5" />
        <line x1={hueX1} y1={hueY1} x2={hueX2} y2={hueY2} stroke="#000" strokeWidth="1" />

        {/* Triangle */}
        <polygon points={`${V1[0]},${V1[1]} ${V2[0]},${V2[1]} ${V3[0]},${V3[1]}`} fill={hueColor} stroke="rgba(0,0,0,.3)" strokeWidth=".5" />
        <polygon points={`${V1[0]},${V1[1]} ${V2[0]},${V2[1]} ${V3[0]},${V3[1]}`} fill="url(#cw-white)" />
        <polygon points={`${V1[0]},${V1[1]} ${V2[0]},${V2[1]} ${V3[0]},${V3[1]}`} fill="url(#cw-black)" />

        {/* SV cursor */}
        <circle cx={cursorX} cy={cursorY} r="6" fill="none" stroke="#000" strokeWidth="2" />
        <circle cx={cursorX} cy={cursorY} r="6" fill="none" stroke="#fff" strokeWidth="1.25" />
      </svg>

      <div style={{ display: 'grid', gridTemplateColumns: '32px 1fr 1fr 1fr', gap: 4, alignItems: 'center' }}>
        <div style={{ width: 32, height: 32, borderRadius: 4, background: hex, border: '1px solid var(--dcs-line)' }} />
        <input className="dcs-input dcs-num" value={`#${hex.slice(1).toUpperCase()}`} readOnly style={{ textAlign: 'center', padding: 0 }} />
        <input className="dcs-input dcs-num" value={`${Math.round(h)}°`} readOnly style={{ textAlign: 'center', padding: 0 }} />
        <input className="dcs-input dcs-num" value={`${Math.round(s * 100)}/${Math.round(v * 100)}`} readOnly style={{ textAlign: 'center', padding: 0 }} />
      </div>
    </div>
  );
}

/* ─────────── Color picker (embedded large, square+bar) ─────────── */
function ColorPicker({ value, onChange, compact }) {
  const [hsv, setHsv] = useStateE({ h: 210, s: .7, v: .85 });
  const [prevColor] = useStateE('#2f86ee');
  const [mode, setMode] = useStateE('hex');
  const sqRef = useRefE(null);
  const hueRef = useRefE(null);
  const rgb = hsvToRgb(hsv.h, hsv.s, hsv.v);
  const hex = rgbHex(rgb);
  const hueColor = rgbHex(hsvToRgb(hsv.h, 1, 1));
  const recents = ['#4d9fff', '#2f86ee', '#ff7ab8', '#4ed18a', '#f2b14a', '#b48cff', '#4ad5d5', '#ef6b6b'];

  const dragSquare = (e) => {
    const rect = sqRef.current.getBoundingClientRect();
    const update = (cx, cy) => {
      const s = Math.max(0, Math.min(1, (cx - rect.left) / rect.width));
      const v = 1 - Math.max(0, Math.min(1, (cy - rect.top) / rect.height));
      setHsv(h => ({ ...h, s, v }));
    };
    update(e.clientX, e.clientY);
    const move = ev => update(ev.clientX, ev.clientY);
    const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up);
  };
  const dragHue = (e) => {
    const rect = hueRef.current.getBoundingClientRect();
    const update = (cy) => {
      const t = Math.max(0, Math.min(1, (cy - rect.top) / rect.height));
      setHsv(h => ({ ...h, h: t * 360 }));
    };
    update(e.clientY);
    const move = ev => update(ev.clientY);
    const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up);
  };

  return (
    <div style={{ width: compact ? 240 : 320, display: 'flex', flexDirection: 'column', gap: 10 }}>
      {/* Mode tabs */}
      <div style={{ display: 'flex', borderBottom: '1px solid var(--dcs-line)', gap: 2 }}>
        {['hex', 'rgb', 'hsl', 'hsv'].map(m => (
          <button key={m}
            onClick={() => setMode(m)}
            style={{
              background: 'transparent', border: 'none',
              padding: '6px 12px', fontSize: 10, letterSpacing: '.08em',
              fontFamily: 'var(--dcs-font-mono)', textTransform: 'uppercase',
              color: mode === m ? 'var(--dcs-accent)' : 'var(--dcs-text-mute)',
              cursor: 'pointer', borderBottom: mode === m ? '1px solid var(--dcs-accent)' : '1px solid transparent',
              marginBottom: -1,
            }}>{m}</button>
        ))}
        <div style={{ flex: 1 }} />
        <button title="Pick from screen"
          style={{
            background: 'transparent', border: 'none', cursor: 'pointer',
            color: 'var(--dcs-text-mute)', padding: '4px 6px',
          }}><Icon name="eyedropper" /></button>
      </div>

      {/* SV square + vertical hue bar */}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 16px', gap: 8 }}>
        <div ref={sqRef} className="dcs-color-square" style={{ '--hue': hueColor, aspectRatio: '1.2 / 1' }} onPointerDown={dragSquare}>
          <div className="dcs-color-square__cursor" style={{ left: `${hsv.s * 100}%`, top: `${(1 - hsv.v) * 100}%` }} />
        </div>
        <div ref={hueRef}
             onPointerDown={dragHue}
             style={{
               width: 16,
               background: 'linear-gradient(180deg, #f00, #ff0, #0f0, #0ff, #00f, #f0f, #f00)',
               border: '1px solid var(--dcs-line)',
               borderRadius: 'var(--dcs-r-1)',
               position: 'relative',
               cursor: 'ns-resize',
             }}>
          <div style={{
            position: 'absolute', left: -2, right: -2,
            top: `${(hsv.h / 360) * 100}%`,
            transform: 'translateY(-50%)',
            height: 4,
            border: '1px solid #fff',
            background: '#14161c',
            borderRadius: 1,
            pointerEvents: 'none',
          }} />
        </div>
      </div>

      {/* Alpha bar */}
      <div className="dcs-alpha-bar" style={{ '--c': hex }}>
        <div style={{ position: 'absolute', top: -2, bottom: -2, left: '100%', transform: 'translateX(-50%)', width: 4, background: '#fff', border: '1px solid #14161c', borderRadius: 1 }} />
      </div>

      {/* Current / previous comparison */}
      <div style={{
        display: 'grid', gridTemplateColumns: '1fr 1fr', height: 28,
        border: '1px solid var(--dcs-line)', borderRadius: 'var(--dcs-r-1)',
        overflow: 'hidden',
        background: 'repeating-conic-gradient(rgba(255,255,255,.05) 0 90deg, rgba(255,255,255,.1) 90deg 180deg) 0 0 / 12px 12px',
      }}>
        <div style={{ background: prevColor, display: 'flex', alignItems: 'flex-end', padding: '2px 6px', fontFamily: 'var(--dcs-font-mono)', fontSize: 9, color: 'rgba(0,0,0,.6)' }}>previous</div>
        <div style={{ background: hex, display: 'flex', alignItems: 'flex-end', justifyContent: 'flex-end', padding: '2px 6px', fontFamily: 'var(--dcs-font-mono)', fontSize: 9, color: 'rgba(0,0,0,.6)' }}>new</div>
      </div>

      {/* Value fields by mode */}
      {mode === 'hex' && (
        <div className="dcs-row" style={{ gap: 4 }}>
          <span className="dcs-mono" style={{ fontSize: 11, color: 'var(--dcs-text-mute)', minWidth: 28 }}>#</span>
          <input className="dcs-input dcs-mono" value={hex.slice(1).toUpperCase()} readOnly style={{ flex: 1 }} />
          <Button sm icon iconLeft="copy" />
        </div>
      )}
      {mode === 'rgb' && (
        <div className="dcs-row" style={{ gap: 4 }}>
          {['R', 'G', 'B'].map((k, i) => (
            <div key={k} style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: 2 }}>
              <div style={{ fontSize: 9, color: 'var(--dcs-text-mute)', textAlign: 'center', fontFamily: 'var(--dcs-font-mono)' }}>{k}</div>
              <input className="dcs-input dcs-mono" defaultValue={Math.round(rgb[i] * 255)} style={{ textAlign: 'center', padding: 0 }} />
            </div>
          ))}
        </div>
      )}
      {mode === 'hsl' && (
        <div className="dcs-row" style={{ gap: 4 }}>
          {[['H', `${Math.round(hsv.h)}°`], ['S', `${Math.round(hsv.s * 100)}%`], ['L', `${Math.round(hsv.v * 50)}%`]].map(([k, v]) => (
            <div key={k} style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: 2 }}>
              <div style={{ fontSize: 9, color: 'var(--dcs-text-mute)', textAlign: 'center', fontFamily: 'var(--dcs-font-mono)' }}>{k}</div>
              <input className="dcs-input dcs-mono" defaultValue={v} style={{ textAlign: 'center', padding: 0 }} />
            </div>
          ))}
        </div>
      )}
      {mode === 'hsv' && (
        <div className="dcs-row" style={{ gap: 4 }}>
          {[['H', `${Math.round(hsv.h)}°`], ['S', `${Math.round(hsv.s * 100)}%`], ['V', `${Math.round(hsv.v * 100)}%`]].map(([k, v]) => (
            <div key={k} style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: 2 }}>
              <div style={{ fontSize: 9, color: 'var(--dcs-text-mute)', textAlign: 'center', fontFamily: 'var(--dcs-font-mono)' }}>{k}</div>
              <input className="dcs-input dcs-mono" defaultValue={v} style={{ textAlign: 'center', padding: 0 }} />
            </div>
          ))}
        </div>
      )}

      {!compact && (
        <div>
          <div style={{ fontSize: 9, color: 'var(--dcs-text-mute)', letterSpacing: '.08em', textTransform: 'uppercase', fontFamily: 'var(--dcs-font-mono)', marginBottom: 4 }}>Recent</div>
          <div style={{ display: 'flex', gap: 3 }}>
            {recents.map(c => (
              <div key={c} title={c.toUpperCase()} style={{
                flex: 1, height: 18, background: c,
                border: '1px solid var(--dcs-line)', borderRadius: 2,
                cursor: 'pointer',
              }} />
            ))}
          </div>
        </div>
      )}
    </div>
  );
}

function SectionColorPicker() {
  const [open, setOpen] = useStateE(false);
  const swatches = [
    '#4d9fff', '#2f86ee', '#1c5fb0', '#0b3a78', '#ef6b6b', '#f2b14a', '#4ed18a',
    '#b48cff', '#ff7ab8', '#4ad5d5', '#e7e9ee', '#aab0bd', '#767c8a', '#14161c',
    '#3c424f', '#2a2e38', '#1f222a', '#181a21', '#00b8d4', '#ff8a3a',
  ];
  return (
    <section className="dw-section" id="color-pickers">
      <div className="dw-section__eyebrow">Editors · 01</div>
      <h2>Color pickers</h2>
      <p className="dw-section__lead">
        Three sizes, one model. The large embedded picker for material editors; a popup variant
        that appears next to swatch buttons; tight chip widgets for inspectors and palette wells.
      </p>

      <Demo frame="app" caption="HSV wheel — outer ring for hue, inner triangle for saturation/value">
        <div className="dcs-row" style={{ gap: 16, alignItems: 'flex-start', flexWrap: 'wrap' }}>
          <Panel title="Material ▸ HSV" icon="palette" pad="sm">
            <ColorWheel size={240} />
          </Panel>
          <Panel title="Same color · different model" icon="droplet">
            <ColorPicker compact />
          </Panel>
        </div>
      </Demo>

      <Demo frame="app" caption="Large embedded — for material / shader panels">
        <div className="dcs-row" style={{ gap: 16, alignItems: 'flex-start' }}>
          <Panel title="Material · Lambert" icon="palette">
            <ColorPicker />
          </Panel>
          <Panel title="Live preview" icon="eye" style={{ minWidth: 200 }}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
              <div style={{ width: '100%', aspectRatio: '1', borderRadius: 6, background: 'radial-gradient(circle at 35% 30%, #b6d6ff, #4d9fff 60%, #1c4080 100%)', boxShadow: 'inset 0 0 24px rgba(0,0,0,.4)' }} />
              <div className="dcs-num" style={{ fontSize: 11, color: 'var(--dcs-text-dim)', lineHeight: 1.7 }}>
                <div>RGB · 077 159 255</div>
                <div>HSV · 210° 70% 100%</div>
                <div>HEX · #4D9FFF</div>
                <div>LIN · 0.072 0.348 1.000</div>
              </div>
            </div>
          </Panel>
        </div>
      </Demo>

      <Demo frame="app" caption="Swatch button → popup">
        <div className="dcs-col" style={{ gap: 10, position: 'relative' }}>
          <div className="dcs-row" style={{ gap: 12 }}>
            <div className="dcs-field" style={{ minWidth: 0 }}>
              <label className="dcs-field__label">Diffuse</label>
              <div className="dcs-swatch" onClick={() => setOpen(o => !o)}>
                <div className="dcs-swatch__chip" style={{ '--c': '#4d9fff' }} />
                <span>#4D9FFF</span>
              </div>
            </div>
            <div className="dcs-field" style={{ minWidth: 0 }}>
              <label className="dcs-field__label">Specular</label>
              <div className="dcs-swatch">
                <div className="dcs-swatch__chip" style={{ '--c': '#23262e' }} />
                <span>#23262E</span>
              </div>
            </div>
            <div className="dcs-field" style={{ minWidth: 0 }}>
              <label className="dcs-field__label">Emission</label>
              <div className="dcs-swatch">
                <div className="dcs-swatch__chip" style={{ '--c': '#000000' }} />
                <span>#000000</span>
              </div>
            </div>
          </div>
          {open && (
            <div style={{ position: 'absolute', top: '100%', left: 0, marginTop: 6, zIndex: 10 }}>
              <Panel raised pad="sm" style={{ boxShadow: 'var(--dcs-shadow-pop)' }}>
                <ColorPicker compact />
              </Panel>
            </div>
          )}
        </div>
      </Demo>

      <Demo frame="app" caption="Palette well — pin colors per project">
        <Panel title="Palette · Project" icon="palette" pad="sm"
               tools={<><Button ghost sm icon iconLeft="plus" /><Button ghost sm icon iconLeft="eyedropper" /></>}>
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(10, 1fr)', gap: 4 }}>
            {swatches.map(c => (
              <div key={c} className="dcs-tooltip" data-tip={c.toUpperCase()} style={{
                aspectRatio: '1', borderRadius: 'var(--dcs-r-1)',
                background: c, cursor: 'pointer',
                border: '1px solid var(--dcs-line)',
                boxShadow: 'var(--dcs-bevel-down)'
              }} />
            ))}
            <div style={{ aspectRatio: '1', borderRadius: 'var(--dcs-r-1)', background: 'var(--dcs-well)', border: '1px dashed var(--dcs-line-strong)', display: 'flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', color: 'var(--dcs-text-mute)' }}>
              <Icon name="plus" size="sm" />
            </div>
          </div>
        </Panel>
      </Demo>
    </section>
  );
}

/* ─────────── Curve editor ─────────── */
// Each point: { x, y, hx, hy } where (hx, hy) is the RIGHT-side tangent
// half-vector in normalized 0..1 space. The left handle mirrors as (-hx, -hy)
// so the curve stays smooth across the point.
function CurveEditor({ height = 220, points: initial, showHandles = true }) {
  // Migrate legacy points (with just .h scalar or none) → {x, y, hx, hy}
  const seed = React.useMemo(() => {
    const src = initial || [
      { x: 0,    y: 1 },
      { x: 0.15, y: 0.2 },
      { x: 0.5,  y: 0.6 },
      { x: 0.85, y: 0.1 },
      { x: 1,    y: 0.3 },
    ];
    return src.map((p, i, a) => {
      if (p.hx !== undefined && p.hy !== undefined) return p;
      const prev = a[Math.max(0, i - 1)];
      const next = a[Math.min(a.length - 1, i + 1)];
      const dx = (next.x - prev.x) / 3;
      const dy = (next.y - prev.y) / 3;
      return { x: p.x, y: p.y, hx: dx, hy: dy };
    });
  }, [initial]);

  const [pts, setPts] = useStateE(seed);
  const [active, setActive] = useStateE(2);
  const [size, setSize] = useStateE({ w: 600, h: 260 });
  const wrapRef = useRefE(null);
  const svgRef = useRefE(null);

  useEffectE(() => {
    if (!wrapRef.current) return;
    const ro = new ResizeObserver(entries => {
      const r = entries[0].contentRect;
      setSize({ w: Math.max(100, Math.round(r.width)), h: Math.max(60, Math.round(r.height)) });
    });
    ro.observe(wrapRef.current);
    return () => ro.disconnect();
  }, []);

  const W = size.w, H = size.h;
  const px = (p) => p.x * W;
  const py = (p) => (1 - p.y) * H;
  // Handle pixel positions (right + left, mirrored)
  const hxPx = (p) => p.hx * W;
  const hyPx = (p) => -p.hy * H;   // y is flipped in screen coords

  const dragPoint = (i) => (e) => {
    e.stopPropagation();
    setActive(i);
    const rect = svgRef.current.getBoundingClientRect();
    const move = (ev) => {
      const nx = Math.max(0, Math.min(1, (ev.clientX - rect.left) / rect.width));
      const ny = 1 - Math.max(0, Math.min(1, (ev.clientY - rect.top) / rect.height));
      setPts(p => p.map((q, j) => j === i ? { ...q, x: nx, y: ny } : q));
    };
    const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up);
  };

  // side = +1 for right handle, -1 for left. Either drag updates the
  // right-handle (hx, hy); left-handle is computed as the mirror.
  const dragHandle = (i, side) => (e) => {
    e.stopPropagation();
    setActive(i);
    const rect = svgRef.current.getBoundingClientRect();
    const center = pts[i];
    const move = (ev) => {
      const nx = (ev.clientX - rect.left) / rect.width;
      const ny = 1 - (ev.clientY - rect.top) / rect.height;
      // Offset from point center, in normalized space
      const dx = (nx - center.x) * side;
      const dy = (ny - center.y) * side;
      // Clamp to a sensible reach
      const max = 0.5;
      setPts(p => p.map((q, j) => j === i ? {
        ...q,
        hx: Math.max(-max, Math.min(max, dx)),
        hy: Math.max(-max, Math.min(max, dy)),
      } : q));
    };
    const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up);
  };

  // Build cubic path using each point's tangent as the cubic control offset.
  const path = pts.length < 2 ? '' : pts.reduce((acc, p, i, a) => {
    if (i === 0) return `M ${px(p)} ${py(p)}`;
    const p0 = a[i - 1];
    const p1 = p;
    // cp1 = p0 + p0's right tangent;  cp2 = p1 - p1's right tangent (= p1 + p1.left)
    const cp1x = px(p0) + hxPx(p0);
    const cp1y = py(p0) + hyPx(p0);
    const cp2x = px(p1) - hxPx(p1);
    const cp2y = py(p1) - hyPx(p1);
    return acc + ` C ${cp1x} ${cp1y}, ${cp2x} ${cp2y}, ${px(p1)} ${py(p1)}`;
  }, '');

  const curveColor = '#e7e9ee';
  const tanColor = '#aab0bd';
  const accent = '#4d9fff';

  return (
    <div ref={wrapRef} className="dcs-graph" style={{ height, position: 'relative' }}>
      <div className="dcs-graph__major" />
      <svg ref={svgRef} width={W} height={H} viewBox={`0 0 ${W} ${H}`} style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
        <path d={`${path} L ${W} ${H} L 0 ${H} Z`} fill="var(--dcs-accent)" fillOpacity=".12" />
        <path d={path} fill="none" stroke={curveColor} strokeWidth="1.5" />

        {/* Tangent handles for the active point only — draggable in 2D, mirrored */}
        {showHandles && active !== null && active >= 0 && active < pts.length && (() => {
          const p = pts[active];
          const cx = px(p), cy = py(p);
          const dxp = hxPx(p), dyp = hyPx(p);
          const rx = cx + dxp, ry = cy + dyp;
          const lx = cx - dxp, ly = cy - dyp;
          return (
            <g>
              <line x1={lx} y1={ly} x2={rx} y2={ry}
                    stroke={tanColor} strokeWidth="1" strokeDasharray="3 3" opacity=".9" />
              {/* Right handle */}
              <circle cx={rx} cy={ry} r="9" fill="transparent"
                      style={{ cursor: 'move' }}
                      onPointerDown={dragHandle(active, +1)} />
              <circle cx={rx} cy={ry} r="4"
                      fill={accent} stroke="var(--dcs-bg)" strokeWidth="1.5"
                      style={{ pointerEvents: 'none' }} />
              {/* Left handle (mirrored) */}
              <circle cx={lx} cy={ly} r="9" fill="transparent"
                      style={{ cursor: 'move' }}
                      onPointerDown={dragHandle(active, -1)} />
              <circle cx={lx} cy={ly} r="4"
                      fill={accent} stroke="var(--dcs-bg)" strokeWidth="1.5"
                      style={{ pointerEvents: 'none' }} />
            </g>
          );
        })()}

        {pts.map((p, i) => (
          <g key={i} onPointerDown={dragPoint(i)} style={{ cursor: 'move' }}>
            <circle cx={px(p)} cy={py(p)} r="9" fill="transparent" />
            <circle cx={px(p)} cy={py(p)} r="4.5"
                    fill={i === active ? curveColor : 'var(--dcs-bg)'}
                    stroke={curveColor} strokeWidth="1.5" />
            {i === active && (
              <text x={px(p) + 10} y={py(p) - 8} fontFamily="JetBrains Mono" fontSize="10" fill={curveColor}>
                {p.x.toFixed(2)}, {p.y.toFixed(2)}
              </text>
            )}
          </g>
        ))}

        <text x="6" y="14" fontFamily="JetBrains Mono" fontSize="10" fill="var(--dcs-text-mute)" opacity=".6">1.0</text>
        <text x="6" y={H - 6} fontFamily="JetBrains Mono" fontSize="10" fill="var(--dcs-text-mute)" opacity=".6">0.0</text>
        <text x={W - 26} y={H - 6} fontFamily="JetBrains Mono" fontSize="10" fill="var(--dcs-text-mute)" opacity=".6">1.0</text>
      </svg>
    </div>
  );
}

function SectionCurve() {
  return (
    <section className="dw-section" id="curve">
      <div className="dw-section__eyebrow">Editors · 02</div>
      <h2>Curve editor</h2>
      <p className="dw-section__lead">
        Drag control points to shape envelopes, color ramps, falloffs, or animation eases. The
        curve fills against the accent at low opacity so the silhouette reads even at small sizes.
      </p>
      <Demo frame="app" caption="Drag any point to reshape the curve">
        <Panel title="Envelope · Filter cutoff" icon="envelope" pad="sm"
               tools={<><ButtonGroup value="bez" onChange={() => {}} options={[
                  { value: 'lin', label: 'Linear' },
                  { value: 'bez', label: 'Bezier' },
                  { value: 'step', label: 'Step' },
               ]} /></>}>
          <CurveEditor />
        </Panel>
      </Demo>

      <Demo frame="app" caption="A second instance — same component, different signal">
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
          <Panel title="Tonemap · Filmic" icon="curve" pad="sm">
            <CurveEditor height={160} points={[
              { x: 0, y: 0 }, { x: 0.18, y: 0.08 }, { x: 0.5, y: 0.45 }, { x: 0.8, y: 0.85 }, { x: 1, y: 0.97 }
            ]} />
          </Panel>
          <Panel title="LFO · Sample &amp; Hold" icon="lfo" pad="sm">
            <CurveEditor height={160} points={[
              { x: 0, y: 0.5 }, { x: 0.2, y: 0.9 }, { x: 0.4, y: 0.15 }, { x: 0.6, y: 0.65 }, { x: 0.8, y: 0.25 }, { x: 1, y: 0.5 }
            ]} />
          </Panel>
        </div>
      </Demo>
    </section>
  );
}

/* ─────────── Graph editor (node graph) ─────────── */
function NodeGraph() {
  const nodes = [
    { id: 'noise',  x: 30,  y: 30,  title: 'Noise',     icon: 'wave-noise', color: '#b48cff', outs: ['fac'] },
    { id: 'tex',    x: 30,  y: 170, title: 'Texture',   icon: 'image',      color: '#f2b14a', outs: ['rgb'] },
    { id: 'mix',    x: 240, y: 90,  title: 'Mix',       icon: 'array',      color: '#4d9fff', ins: ['a','b','fac'], outs: ['rgb'] },
    { id: 'gamma',  x: 430, y: 70,  title: 'Gamma',     icon: 'bolt',       color: '#4ed18a', ins: ['in'], outs: ['out'] },
    { id: 'output', x: 600, y: 90,  title: 'Output',    icon: 'render',     color: '#4d9fff', ins: ['color'] },
  ];
  // Sockets are positioned via percentages of node body — let's hardcode pixel positions for connections
  const W = 720, H = 280;
  const wires = [
    { fx: 30+120, fy: 30+42,  tx: 240+8,  ty: 90+42 },   // noise.fac -> mix.a
    { fx: 30+120, fy: 170+42, tx: 240+8,  ty: 90+62 },   // tex.rgb   -> mix.b
    { fx: 240+120, fy: 90+42, tx: 430+8,  ty: 70+42 },   // mix.rgb -> gamma.in
    { fx: 430+120, fy: 70+42, tx: 600+8,  ty: 90+42 },   // gamma.out -> output.color
  ];
  return (
    <div className="dcs-graph" style={{ height: 320, position: 'relative', overflow: 'hidden' }}>
      <div className="dcs-graph__major" />
      <svg viewBox={`0 0 ${W} ${H}`} style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }} preserveAspectRatio="xMidYMid meet">
        {wires.map((w, i) => {
          const mx = (w.fx + w.tx) / 2;
          return <path key={i} d={`M ${w.fx} ${w.fy} C ${mx} ${w.fy}, ${mx} ${w.ty}, ${w.tx} ${w.ty}`} fill="none" stroke="var(--dcs-accent)" strokeWidth="1.5" strokeOpacity=".85" />;
        })}
      </svg>
      {nodes.map(n => (
        <div key={n.id} style={{
          position: 'absolute', left: `${(n.x / W) * 100}%`, top: `${(n.y / H) * 100}%`,
          width: 128,
          background: 'var(--dcs-bg)',
          border: '1px solid var(--dcs-line)',
          borderRadius: 'var(--dcs-r-2)',
          overflow: 'hidden',
          fontSize: 11,
          userSelect: 'none',
        }}>
          <div style={{
            height: 22,
            background: n.color,
            color: '#0a1220',
            display: 'flex', alignItems: 'center', gap: 6, padding: '0 8px',
            fontWeight: 600, letterSpacing: '.04em',
            textTransform: 'uppercase', fontSize: 10,
          }}>
            <Icon name={n.icon} size="sm" />
            <span>{n.title}</span>
          </div>
          <div style={{ padding: '6px 0' }}>
            {n.ins?.map((s, i) => (
              <div key={s} style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '2px 8px', color: 'var(--dcs-text-dim)' }}>
                <div style={{ width: 8, height: 8, borderRadius: '50%', background: 'var(--dcs-accent)', marginLeft: -12 }} />
                <span>{s}</span>
              </div>
            ))}
            {n.outs?.map((s) => (
              <div key={s} style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '2px 8px', color: 'var(--dcs-text-dim)', justifyContent: 'flex-end' }}>
                <span>{s}</span>
                <div style={{ width: 8, height: 8, borderRadius: '50%', background: 'var(--dcs-accent)', marginRight: -12 }} />
              </div>
            ))}
          </div>
        </div>
      ))}
    </div>
  );
}

function SectionGraph() {
  return (
    <section className="dw-section" id="graph">
      <div className="dw-section__eyebrow">Editors · 03</div>
      <h2>Node graph</h2>
      <p className="dw-section__lead">
        For shader networks, compositors, animation rigs, audio routing. Colored title bars sort by
        kind without an icon legend; accent-color wires keep signal flow legible against the grid.
      </p>
      <Demo frame="app">
        <Panel title="Shader network · jane_skin.mat" icon="graph" pad={0}
               tools={<>
                 <Button ghost sm icon iconLeft="zoom-in" />
                 <Button ghost sm icon iconLeft="zoom-out" />
                 <Button ghost sm icon iconLeft="fit" />
                 <Button ghost sm iconLeft="plus">Add node</Button>
               </>}>
          <NodeGraph />
        </Panel>
      </Demo>
    </section>
  );
}

/* ─────────── Texture / image picker ─────────── */
function SectionTexture() {
  const [picked, setPicked] = useStateE(2);
  // Generate procedural tile thumbnails so we don't need image assets
  const tiles = [
    { name: 'Concrete_01', kind: 'PBR', res: '4K', gradient: 'linear-gradient(135deg, #7c8492, #4a5161)' },
    { name: 'WoodPlank_F',  kind: 'PBR', res: '2K', gradient: 'repeating-linear-gradient(90deg, #6e4a2a, #6e4a2a 18px, #4e3520 18px, #4e3520 24px)' },
    { name: 'Sky_Studio',   kind: 'HDRI', res: '8K', gradient: 'linear-gradient(180deg, #ffb84d, #f2b14a 40%, #4d9fff 90%)' },
    { name: 'Metal_Brushed',kind: 'PBR', res: '4K', gradient: 'repeating-linear-gradient(135deg, #cfd4dc, #cfd4dc 1px, #aab0bd 1px, #aab0bd 2px)' },
    { name: 'Noise_Perlin', kind: 'PROC', res: '∞',  gradient: 'radial-gradient(circle at 30% 40%, #555d6e, #20232b)' },
    { name: 'Grass_Hi',     kind: 'PBR', res: '2K', gradient: 'linear-gradient(180deg, #4ed18a, #2bb872 60%, #1a6e44)' },
    { name: 'RustyMetal_3', kind: 'PBR', res: '4K', gradient: 'radial-gradient(circle at 60% 40%, #f2b14a, #8a4a1a)' },
    { name: 'Water_Sub',    kind: 'PBR', res: '2K', gradient: 'linear-gradient(180deg, #4ad5d5, #1c5fb0)' },
    { name: 'Marble_Stat',  kind: 'PBR', res: '4K', gradient: 'linear-gradient(45deg, #e7e9ee, #cfd4dc 50%, #aab0bd)' },
    { name: 'Voronoi_Cell', kind: 'PROC', res: '∞', gradient: 'repeating-radial-gradient(circle at 30% 40%, #b48cff, #b48cff 8px, #6f4eea 8px, #6f4eea 16px)' },
    { name: 'Brick_OldRed', kind: 'PBR', res: '2K', gradient: 'repeating-linear-gradient(0deg, #8a3a3a, #8a3a3a 12px, #602727 12px, #602727 18px)' },
    { name: 'Asphalt_Wet',  kind: 'PBR', res: '4K', gradient: 'radial-gradient(ellipse at 50% 60%, #20232b, #14161c)' },
  ];
  return (
    <section className="dw-section" id="textures">
      <div className="dw-section__eyebrow">Editors · 04</div>
      <h2>Texture &amp; image pickers</h2>
      <p className="dw-section__lead">
        A grid browser with kind/resolution badges, a sidebar of categories, and an inspector for
        the active asset. Tiles use a square aspect with a 1px line keyline.
      </p>
      <Demo frame="app" inset>
        <Panel title="Asset browser · /work/intro/textures" icon="texture" pad={0}
               tools={<><Button ghost sm icon iconLeft="search" /><Button ghost sm icon iconLeft="grid" pressed /><Button ghost sm icon iconLeft="menu" /></>}>
          <div style={{ display: 'flex', minHeight: 360 }}>
            <div style={{ width: 160, borderRight: '1px solid var(--dcs-line)', background: 'var(--dcs-surface-1)' }}>
              <div className="dcs-tree" style={{ padding: '6px 0' }}>
                {[
                  { id: 'all', name: 'All assets', icon: 'folder-open', count: 248 },
                  { id: 'pbr', name: 'PBR Surfaces', icon: 'texture', count: 184 },
                  { id: 'hdri', name: 'HDRI', icon: 'globe', count: 12 },
                  { id: 'proc', name: 'Procedural', icon: 'wave-noise', count: 24 },
                  { id: 'fav', name: 'Favorites', icon: 'star', count: 18 },
                  { id: 'recent', name: 'Recent', icon: 'timeline', count: 10 },
                ].map((c, i) => (
                  <div key={c.id} className="dcs-tree__row" aria-selected={i === 0} style={{ '--depth': 0, paddingLeft: 14 }}>
                    <span style={{ width: 14 }} />
                    <Icon name={c.icon} className="dcs-tree__icon" />
                    <span className="dcs-tree__label">{c.name}</span>
                    <span className="dcs-tree__meta">{c.count}</span>
                  </div>
                ))}
              </div>
            </div>
            <div style={{ flex: 1, padding: 12, display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(96px, 1fr))', gap: 8, alignContent: 'start' }}>
              {tiles.map((t, i) => (
                <div key={t.name} onClick={() => setPicked(i)} style={{
                  cursor: 'pointer', display: 'flex', flexDirection: 'column',
                  border: i === picked ? '1px solid var(--dcs-accent)' : '1px solid transparent',
                  background: i === picked ? 'var(--dcs-accent-dim)' : 'transparent',
                  padding: 4, borderRadius: 4,
                }}>
                  <div style={{
                    width: '100%', aspectRatio: '1',
                    background: t.gradient,
                    border: '1px solid var(--dcs-line)',
                    borderRadius: 2,
                    position: 'relative',
                  }}>
                    <span className="dcs-badge" style={{
                      position: 'absolute', top: 4, left: 4,
                      fontSize: 9, height: 14, padding: '0 4px',
                      background: 'rgba(0,0,0,.5)', borderColor: 'transparent',
                      color: t.kind === 'HDRI' ? 'var(--dcs-warn)' : t.kind === 'PROC' ? 'var(--dcs-purple)' : 'var(--dcs-accent)',
                    }}>{t.kind}</span>
                    <span className="dcs-badge" style={{
                      position: 'absolute', top: 4, right: 4,
                      fontSize: 9, height: 14, padding: '0 4px',
                      background: 'rgba(0,0,0,.5)', borderColor: 'transparent',
                      color: 'var(--dcs-text-dim)',
                    }}>{t.res}</span>
                  </div>
                  <div style={{
                    fontSize: 10, color: i === picked ? 'var(--dcs-text)' : 'var(--dcs-text-dim)',
                    marginTop: 4, textAlign: 'center', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                    fontFamily: 'var(--dcs-font-mono)',
                  }}>{t.name}</div>
                </div>
              ))}
            </div>
            <div style={{ width: 220, borderLeft: '1px solid var(--dcs-line)', background: 'var(--dcs-surface-1)', padding: 12 }}>
              <div style={{
                width: '100%', aspectRatio: '1',
                background: tiles[picked].gradient,
                border: '1px solid var(--dcs-line)', borderRadius: 4, marginBottom: 10,
              }} />
              <div style={{ fontSize: 13, color: 'var(--dcs-text)', marginBottom: 4 }}>{tiles[picked].name}</div>
              <div className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text-mute)', marginBottom: 12 }}>
                <div>{tiles[picked].kind} · {tiles[picked].res} · linear sRGB</div>
                <div>8.4 MB · 4096 × 4096</div>
                <div>last used 2 days ago</div>
              </div>
              <div className="dcs-col" style={{ gap: 6 }}>
                <Button sm primary iconLeft="check" style={{ width: '100%' }}>Use texture</Button>
                <Button sm ghost iconLeft="export" style={{ width: '100%' }}>Reveal in OS</Button>
                <Button sm ghost iconLeft="trash" style={{ width: '100%' }}>Remove</Button>
              </div>
            </div>
          </div>
        </Panel>
      </Demo>
    </section>
  );
}

Object.assign(window, { SectionColorPicker, SectionCurve, SectionGraph, SectionTexture, CurveEditor, NodeGraph, ColorPicker });
