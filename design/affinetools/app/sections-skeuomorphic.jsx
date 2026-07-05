/* sections-skeuomorphic.jsx
   Skeuomorphic step sequencer — patch jacks, backlit buttons, 7-seg LEDs, meters,
   plus draggable patch cables (Reason-style: drag from one jack to another).
*/
const { useState: useStateSK, useEffect: useEffectSK, useRef: useRefSK, useLayoutEffect: useLayoutEffectSK } = React;

/* ─────────── Cable colors ─────────── */
const CABLE_COLORS = [
  '#6fb3ff', // ui blue (brighter, slightly desaturated)
  '#6fdfa8', // ui green
  '#f5c878', // ui amber
  '#f29090', // ui danger
  '#c8a8ff', // ui purple
  '#80e0e0', // ui teal
];

/* ─────────── Patch board context ─────────── */
const PatchContext = React.createContext(null);

function PatchBoard({ children }) {
  const [cables, setCables] = useStateSK([]);
  const [drag, setDrag] = useStateSK(null);
  const [jackPositions, setJackPositions] = useStateSK({});
  const [version, setVersion] = useStateSK(0);
  const boardRef = useRefSK(null);
  const jackRefs = useRefSK({});
  const dragStateRef = useRefSK(null);
  // Mirror cables in a ref so beginDrag always sees the latest array
  // (avoids stale-closure issues when the user clicks rapidly)
  const cablesRef = useRefSK([]);
  useEffectSK(() => { cablesRef.current = cables; }, [cables]);

  const registerJack = (id, el) => {
    jackRefs.current[id] = el;
  };
  const unregisterJack = (id) => {
    delete jackRefs.current[id];
  };

  const measureAll = React.useCallback(() => {
    if (!boardRef.current) return;
    const board = boardRef.current.getBoundingClientRect();
    const next = {};
    Object.entries(jackRefs.current).forEach(([id, el]) => {
      if (!el) return;
      const r = el.getBoundingClientRect();
      next[id] = {
        x: r.left - board.left + r.width / 2,
        y: r.top - board.top + r.height / 2,
      };
    });
    setJackPositions(next);
  }, []);

  useLayoutEffectSK(() => { measureAll(); }, [cables, version, measureAll]);
  useEffectSK(() => {
    const onResize = () => measureAll();
    window.addEventListener('resize', onResize);
    const ro = new ResizeObserver(onResize);
    if (boardRef.current) ro.observe(boardRef.current);
    return () => { window.removeEventListener('resize', onResize); ro.disconnect(); };
  }, [measureAll]);

  const beginDrag = (fromId, e) => {
    const board = boardRef.current.getBoundingClientRect();
    // Read cables from the ref so we always have the latest set,
    // not a stale closure capture from an earlier render.
    const currentCables = cablesRef.current;
    const existingIdx = currentCables.findIndex(c => c.from === fromId || c.to === fromId);
    let anchorId = fromId;
    let color = CABLE_COLORS[currentCables.length % CABLE_COLORS.length];
    if (existingIdx >= 0) {
      const c = currentCables[existingIdx];
      anchorId = c.from === fromId ? c.to : c.from;
      color = c.color;
      const removedFromId = fromId;
      setCables(cs => cs.filter(cc => !(cc.from === c.from && cc.to === c.to && cc.color === c.color)));
    }
    const anchorEl = jackRefs.current[anchorId];
    if (!anchorEl) return;
    const r = anchorEl.getBoundingClientRect();
    const fromPt = { x: r.left - board.left + r.width / 2, y: r.top - board.top + r.height / 2 };

    const initialPt = { x: e.clientX - board.left, y: e.clientY - board.top };
    // Initial pivot = midpoint between anchor and cursor, dropped by current sag
    const initChord = Math.hypot(initialPt.x - fromPt.x, initialPt.y - fromPt.y);
    let initSag = 0;
    if (initChord < CABLE_LENGTH) {
      initSag = Math.sqrt(Math.max(0, CABLE_LENGTH * CABLE_LENGTH - initChord * initChord)) / 2;
    }
    const initBob = {
      x: (fromPt.x + initialPt.x) / 2,
      y: (fromPt.y + initialPt.y) / 2 + initSag,
    };
    dragStateRef.current = {
      fromId: anchorId, fromPt, color,
      targetPt: { ...initialPt },
      bob: { ...initBob },
      vx: 0, vy: 0,
      lastT: performance.now(),
    };
    setDrag({ fromId: anchorId, fromPt, toPt: initialPt, color, bob: initBob });

    let rafId;
    const tick = () => {
      const s = dragStateRef.current;
      if (!s) return;
      const now = performance.now();
      const dt = Math.min(0.05, (now - s.lastT) / 1000);
      s.lastT = now;

      // Pivot = current midpoint between anchor (fromPt) and dragged plug (targetPt).
      // Bob hangs from pivot by `sag` below — pivot moves WITH the plug.
      const chord = Math.hypot(s.targetPt.x - s.fromPt.x, s.targetPt.y - s.fromPt.y);
      let sag = 0;
      if (chord < CABLE_LENGTH) {
        sag = Math.sqrt(Math.max(0, CABLE_LENGTH * CABLE_LENGTH - chord * chord)) / 2;
      }
      const rest = {
        x: (s.fromPt.x + s.targetPt.x) / 2,
        y: (s.fromPt.y + s.targetPt.y) / 2 + sag,
      };

      // Spring-damper on bob toward rest position (underdamped pendulum)
      const k = 30;
      const damp = 1.4;
      const ax = (rest.x - s.bob.x) * k - s.vx * damp;
      const ay = (rest.y - s.bob.y) * k - s.vy * damp;
      s.vx += ax * dt;
      s.vy += ay * dt;
      s.bob.x += s.vx * dt;
      s.bob.y += s.vy * dt;

      setDrag(d => d ? { ...d, toPt: { ...s.targetPt }, bob: { ...s.bob } } : null);
      rafId = requestAnimationFrame(tick);
    };
    rafId = requestAnimationFrame(tick);

    const move = (ev) => {
      const s = dragStateRef.current;
      if (!s) return;
      s.targetPt = { x: ev.clientX - board.left, y: ev.clientY - board.top };
    };
    const up = (ev) => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      cancelAnimationFrame(rafId);
      let hitId = null;
      Object.entries(jackRefs.current).forEach(([id, el]) => {
        if (id === anchorId || !el) return;
        const rr = el.getBoundingClientRect();
        if (ev.clientX >= rr.left && ev.clientX <= rr.right && ev.clientY >= rr.top && ev.clientY <= rr.bottom) {
          hitId = id;
        }
      });
      if (hitId) {
        setCables(cs => {
          const filtered = cs.filter(c => c.from !== anchorId && c.to !== anchorId && c.from !== hitId && c.to !== hitId);
          return [...filtered, { from: anchorId, to: hitId, color }];
        });
      }
      dragStateRef.current = null;
      setDrag(null);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  };

  const removeCable = (idx) => setCables(cs => cs.filter((_, i) => i !== idx));

  const ctx = {
    registerJack, unregisterJack, beginDrag, cables, removeCable,
    isPatched: (id) => cables.some(c => c.from === id || c.to === id) || (drag && drag.fromId === id),
    refreshLayout: () => setVersion(v => v + 1),
  };

  return (
    <PatchContext.Provider value={ctx}>
      <div ref={boardRef} style={{ position: 'relative' }}>
        {children}
        <CableLayer cables={cables} jackPositions={jackPositions} drag={drag} onRemove={removeCable} />
      </div>
    </PatchContext.Provider>
  );
}

/* Catenary with constant total length, lateral sway at the midpoint, smooth bottom.
   Sag is maximum when the endpoints are closest (cable hangs in a deep V) and
   smoothly decreases to zero as the chord approaches the cable length. */
const CABLE_LENGTH = 640;   // total "rope" length in px — cable never stretches
function cablePath(a, b, bob) {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  // If no bob position is supplied, fall back to gravity rest position
  let m = bob;
  if (!m) {
    const chord = Math.hypot(dx, dy);
    let sag = 0;
    if (chord < CABLE_LENGTH) {
      sag = Math.sqrt(Math.max(0, CABLE_LENGTH * CABLE_LENGTH - chord * chord)) / 2;
    }
    m = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 + sag };
  }
  // Smooth cubic through midpoint: control points slightly past the midpoint
  // so the curve passes through (m.x, m.y) without a corner.
  const c1x = a.x + (m.x - a.x) * 0.5;
  const c1y = a.y + (m.y - a.y) * 1.10;
  const c2x = b.x + (m.x - b.x) * 0.5;
  const c2y = b.y + (m.y - b.y) * 1.10;
  return `M ${a.x} ${a.y} C ${c1x} ${c1y}, ${c2x} ${c2y}, ${b.x} ${b.y}`;
}

/* Lighten / darken a hex color */
function shade(hex, amt) {
  const m = hex.match(/^#([0-9a-f]{6})$/i);
  if (!m) return hex;
  const n = parseInt(m[1], 16);
  let r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
  r = Math.max(0, Math.min(255, Math.round(r + 255 * amt)));
  g = Math.max(0, Math.min(255, Math.round(g + 255 * amt)));
  b = Math.max(0, Math.min(255, Math.round(b + 255 * amt)));
  return `#${[r,g,b].map(v => v.toString(16).padStart(2,'0')).join('')}`;
}

function ConnectedEnd({ x, y, color }) {
  // Chrome plug head — black outer rim, brushed-steel center, no cable color
  return (
    <g style={{ pointerEvents: 'none' }}>
      <circle cx={x} cy={y} r="9" fill="#0a0a0a" />
      <circle cx={x} cy={y} r="7.5"
              fill="url(#plug-metal)" />
      <circle cx={x} cy={y} r="3" fill="#1a1a1a" />
      <circle cx={x - 2} cy={y - 2.5} r="2" fill="rgba(255,255,255,.55)" />
    </g>
  );
}

function DragPlug({ x, y, color }) {
  // Free end while dragging — slightly larger, with drop shadow
  return (
    <g style={{ pointerEvents: 'none' }}>
      <ellipse cx={x} cy={y + 2} rx="10" ry="3.5" fill="rgba(0,0,0,.4)" />
      <circle cx={x} cy={y} r="10" fill="#0a0a0a" />
      <circle cx={x} cy={y} r="8.5" fill="url(#plug-metal)" />
      <circle cx={x} cy={y} r="3.5" fill="#1a1a1a" />
      <ellipse cx={x - 2.5} cy={y - 3} rx="2.5" ry="1.6" fill="rgba(255,255,255,.6)" />
    </g>
  );
}

function CableLayer({ cables, jackPositions, drag, onRemove }) {
  const resolved = cables.map((c, i) => {
    const a = jackPositions[c.from];
    const b = jackPositions[c.to];
    if (!a || !b) return null;
    return { ...c, a, b, i };
  }).filter(Boolean);

  return (
    <svg style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 5, overflow: 'visible' }}>
      <defs>
        <radialGradient id="plug-metal" cx="0.4" cy="0.35" r="0.65">
          <stop offset="0%" stopColor="#f0f2f5" />
          <stop offset="45%" stopColor="#9a9ea6" />
          <stop offset="100%" stopColor="#3a3d44" />
        </radialGradient>
        {/* Cable strand gradient: dark edge → light center → dark edge, giving cylindrical look */}
        {[...CABLE_COLORS].map(c => (
          <linearGradient key={c} id={`cable-${c.replace('#','')}`} x1="0" y1="0" x2="0" y2="1" gradientUnits="userSpaceOnUse">
            <stop offset="0%"  stopColor={shade(c, -0.30)} />
            <stop offset="50%" stopColor={shade(c, +0.10)} />
            <stop offset="100%" stopColor={shade(c, -0.30)} />
          </linearGradient>
        ))}
      </defs>

      {/* shadows */}
      {resolved.map(c => (
        <path key={`s${c.i}`} d={cablePath({ x: c.a.x, y: c.a.y + 5 }, { x: c.b.x, y: c.b.y + 5 })}
              fill="none" stroke="rgba(0,0,0,.45)" strokeWidth="7" strokeLinecap="round" />
      ))}
      {drag && (
        <path d={cablePath(drag.fromPt, drag.toPt, drag.bob)} fill="none"
              stroke="rgba(0,0,0,.45)" strokeWidth="7" strokeLinecap="round"
              transform="translate(0,5)" />
      )}

      {/* Plugs first, then cable strokes on top — cable visually enters the plug */}
      {resolved.map(c => (
        <g key={`plugs${c.i}`} style={{ pointerEvents: 'none' }}>
          <ConnectedEnd x={c.a.x} y={c.a.y} color={c.color} />
          <ConnectedEnd x={c.b.x} y={c.b.y} color={c.color} />
        </g>
      ))}

      {/* cable bodies — drawn AFTER plugs, so cable appears to plug into them */}
      {resolved.map(c => {
        const gradId = `cable-${c.color.replace('#','')}`;
        return (
          <g key={c.i} style={{ pointerEvents: 'stroke', cursor: 'pointer' }}
             onClick={(e) => { e.stopPropagation(); onRemove(c.i); }}>
            <path d={cablePath(c.a, c.b)} fill="none" stroke={`url(#${gradId})`} strokeWidth="6" strokeLinecap="round" />
            <path d={cablePath(c.a, c.b)} fill="none" stroke="rgba(255,255,255,.20)" strokeWidth="1.2" strokeLinecap="round" />
          </g>
        );
      })}

      {/* drag preview */}
      {drag && (() => {
        const gradId = `cable-${drag.color.replace('#','')}`;
        const path = cablePath(drag.fromPt, drag.toPt, drag.bob);
        return (
          <g>
            {/* Plug heads first */}
            <ConnectedEnd x={drag.fromPt.x} y={drag.fromPt.y} color={drag.color} />
            <DragPlug x={drag.toPt.x} y={drag.toPt.y} color={drag.color} />
            {/* Cable on top, going INTO the plugs */}
            <path d={path} fill="none" stroke={`url(#${gradId})`} strokeWidth="6" strokeLinecap="round" />
            <path d={path} fill="none" stroke="rgba(255,255,255,.20)" strokeWidth="1.2" strokeLinecap="round" />
          </g>
        );
      })()}
    </svg>
  );
}

function Jack({ id, label }) {
  const ctx = React.useContext(PatchContext);
  const ref = useRefSK(null);
  const jackId = id || React.useId();

  useLayoutEffectSK(() => {
    if (ctx) {
      ctx.registerJack(jackId, ref.current);
      ctx.refreshLayout();
      return () => ctx.unregisterJack(jackId);
    }
  }, []);

  const patched = ctx ? ctx.isPatched(jackId) : false;

  const handleDown = (e) => {
    if (!ctx) return;
    e.preventDefault();
    ctx.beginDrag(jackId, e);
  };

  // Hexagon points (flat-top), centered on 16,16, radius ~15
  const hexR = 15;
  const hexPts = [0,1,2,3,4,5].map(i => {
    const ang = Math.PI / 3 * i;
    return [16 + hexR * Math.cos(ang), 16 + hexR * Math.sin(ang)].join(',');
  }).join(' ');

  return (
    <div className={`dcs-jack${patched ? ' dcs-jack--patched' : ''}`}>
      <div ref={ref} className="dcs-jack__socket" onPointerDown={handleDown} style={{ cursor: 'crosshair' }}>
        <svg viewBox="0 0 32 32" aria-hidden="true">
          <defs>
            <linearGradient id="jack-nut" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0%" stopColor="#e8eaee" />
              <stop offset="35%" stopColor="#b8bcc4" />
              <stop offset="65%" stopColor="#7a7e86" />
              <stop offset="100%" stopColor="#3a3d44" />
            </linearGradient>
            <radialGradient id="jack-bevel" cx="0.45" cy="0.35" r="0.7">
              <stop offset="0%" stopColor="#f0f2f5" />
              <stop offset="40%" stopColor="#9a9ea6" />
              <stop offset="100%" stopColor="#2a2d33" />
            </radialGradient>
            <radialGradient id="jack-inner" cx="0.5" cy="0.5" r="0.5">
              <stop offset="0%" stopColor="#3a3d44" />
              <stop offset="100%" stopColor="#181a20" />
            </radialGradient>
            <radialGradient id="jack-hole" cx="0.5" cy="0.4" r="0.5">
              <stop offset="0%" stopColor="#0a0a0a" />
              <stop offset="85%" stopColor="#000" />
              <stop offset="100%" stopColor="#000" />
            </radialGradient>
          </defs>
          {/* Hex nut with brushed metal shading */}
          <polygon points={hexPts} fill="url(#jack-nut)"
                   stroke="rgba(0,0,0,.55)" strokeWidth="0.75"
                   strokeLinejoin="miter" />
          {/* Chrome beveled ring */}
          <circle cx="16" cy="16" r="11" fill="url(#jack-bevel)" />
          {/* Inner dark gray recessed ring */}
          <circle cx="16" cy="16" r="7.5" fill="url(#jack-inner)" />
          {/* Center hole */}
          <circle cx="16" cy="16" r="4" fill="url(#jack-hole)" />
          {/* Subtle inner shadow rim */}
          <circle cx="16" cy="16" r="4" fill="none" stroke="rgba(0,0,0,.6)" strokeWidth="0.5" />
          {/* Highlight glint on top-left of bevel */}
          <path d="M 9 12 a 7 7 0 0 1 5 -3" fill="none" stroke="rgba(255,255,255,.45)" strokeWidth="1" strokeLinecap="round" />
          {/* Patch indicator — orange plug visible in socket */}
          {patched && (
            <>
              <circle cx="16" cy="16" r="3" fill="#f8a23a" />
              <circle cx="15" cy="15" r="1" fill="rgba(255,255,255,.5)" />
            </>
          )}
        </svg>
      </div>
      {label && <div className="dcs-jack__label">{label}</div>}
    </div>
  );
}

function LitButton({ children, lit, color, sm, onClick }) {
  const cls = [
    'dcs-litbtn',
    lit && 'dcs-litbtn--lit',
    color && `dcs-litbtn--${color}`,
    sm && 'dcs-litbtn--sm',
  ].filter(Boolean).join(' ');
  return <button className={cls} onClick={onClick}>{children}</button>;
}

function Lcd({ value, size = 'md', color, digits }) {
  // 7-segment glyph map. segments: a=top, b=top-r, c=bot-r, d=bot, e=bot-l, f=top-l, g=mid
  const SEG = {
    '0':'abcdef','1':'bc','2':'abged','3':'abgcd','4':'fbgc','5':'afgcd','6':'afgecd',
    '7':'abc','8':'abcdefg','9':'abcfgd','A':'abcefg','B':'cdefg','C':'adef','D':'bcdeg',
    'E':'adefg','F':'aefg','H':'bcefg','I':'ef','J':'bcde','L':'def','O':'abcdef',
    'P':'abefg','R':'efg','S':'afgcd','U':'bcdef','-':'g',' ':'','.':'',
    'K':'efg','o':'cdeg','n':'ceg','r':'eg','t':'defg','u':'cde',
  };
  const str = String(value).toUpperCase();
  const padded = digits ? str.padStart(digits, ' ') : str;

  // Segment polygon coordinates (24×40 viewBox)
  const segPath = {
    a: 'M 5 2 L 19 2 L 17 5 L 7 5 Z',
    b: 'M 19 3 L 21 5 L 21 18 L 19 19 L 17 17 L 17 6 Z',
    c: 'M 19 21 L 21 22 L 21 35 L 19 37 L 17 34 L 17 23 Z',
    d: 'M 5 38 L 19 38 L 17 35 L 7 35 Z',
    e: 'M 5 21 L 7 23 L 7 34 L 5 37 L 3 35 L 3 22 Z',
    f: 'M 5 3 L 7 6 L 7 17 L 5 19 L 3 18 L 3 5 Z',
    g: 'M 5 20 L 7 18 L 17 18 L 19 20 L 17 22 L 7 22 Z',
  };

  return (
    <div className={`dcs-lcd dcs-lcd--${size}${color ? ` dcs-lcd--${color}` : ''}`}>
      {padded.split('').map((ch, i) => {
        if (ch === '.') return <span key={i} className="dcs-lcd__dot dcs-lcd__dot--on" />;
        const on = SEG[ch] || '';
        return (
          <div key={i} className="dcs-lcd__digit">
            <svg viewBox="0 0 24 40" preserveAspectRatio="xMidYMid meet">
              {Object.entries(segPath).map(([k, d]) => (
                <path key={k} d={d}
                      className={on.includes(k) ? 'dcs-lcd__seg-on' : 'dcs-lcd__seg-off'} />
              ))}
            </svg>
          </div>
        );
      })}
    </div>
  );
}

function LedMeter({ value, segments = 14, vertical }) {
  const lit = Math.round(value * segments);
  const segs = Array.from({ length: segments }, (_, i) => {
    const t = i / (segments - 1);
    const isLit = i < lit;
    let zone = 'ok';
    if (t > 0.85) zone = 'peak';
    else if (t > 0.65) zone = 'warn';
    const cls = isLit
      ? `dcs-meter__seg dcs-meter__seg--lit-${zone}`
      : `dcs-meter__seg dcs-meter__seg--${zone}`;
    return <div key={i} className={cls} />;
  });
  return <div className={`dcs-meter${vertical ? ' dcs-meter--v' : ''}`} style={{ width: vertical ? 14 : '100%', height: vertical ? 100 : 14 }}>{segs}</div>;
}

function SectionSkeuomorphic() {
  const [playing, setPlaying] = useStateSK(false);
  const [steps, setSteps] = useStateSK([true, false, true, true, false, true, false, true]);
  const [activeStep, setActiveStep] = useStateSK(0);
  const [numSteps, setNumSteps] = useStateSK(8);
  const [meter, setMeter] = useStateSK(0.5);

  useEffectSK(() => {
    if (!playing) return;
    const id = setInterval(() => setActiveStep(s => (s + 1) % numSteps), 200);
    return () => clearInterval(id);
  }, [playing, numSteps]);

  useEffectSK(() => {
    const id = setInterval(() => setMeter(m => Math.max(0.1, Math.min(0.95, m + (Math.random() - 0.5) * 0.4))), 90);
    return () => clearInterval(id);
  }, []);

  const toggleStep = (i) => setSteps(s => s.map((v, j) => j === i ? !v : v));

  return (
    <section className="dw-section" id="skeuomorphic">
      <div className="dw-section__eyebrow">Showpiece · 03</div>
      <h2>Skeuomorphic hardware</h2>
      <p className="dw-section__lead">
        For when the design needs to feel like real gear — modular synths, drum machines, guitar
        pedals, mixing consoles. Patch jacks with chrome rings, backlit plastic buttons that glow,
        red 7-segment LED displays, LED bargraph meters. <strong style={{ color: 'var(--dw-text)' }}>Drag from one
        jack to another to patch a cable</strong> — Reason-style. Click an existing cable to remove it.
      </p>

      <div className="dw-demo dw-demo--app" style={{ padding: 28, marginBottom: 32 }}>
        <div className="dcs" data-dcs-demo>
          <PatchBoard>
            <div className="dcs-hw dcs-hw--red" style={{ padding: '36px 24px 24px', position: 'relative', display: 'flex', flexDirection: 'column', gap: 14 }}>
              <span className="dcs-hw__screw dcs-hw__screw--tl" />
              <span className="dcs-hw__screw dcs-hw__screw--tr" />
              <span className="dcs-hw__screw dcs-hw__screw--bl" />
              <span className="dcs-hw__screw dcs-hw__screw--br" />

              <div className="dcs-hw__label" style={{ position: 'absolute', top: 12, left: 0, right: 0, textAlign: 'center', fontSize: 13, letterSpacing: '.22em' }}>
                8 STEP SEQUENCER
              </div>

              <div style={{ display: 'flex', alignItems: 'flex-end', gap: 16 }}>
                <div style={{ display: 'flex', gap: 12 }}>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
                    <LitButton lit={!playing} onClick={() => setPlaying(false)}>■</LitButton>
                    <Jack id="stop-in" label="STOP" />
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
                    <LitButton lit={playing} color="green" onClick={() => setPlaying(true)}>▶</LitButton>
                    <Jack id="start-in" label="START" />
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
                    <LitButton onClick={() => setActiveStep(s => (s + 1) % numSteps)}>▶|</LitButton>
                    <div style={{ height: 30, display: 'flex', alignItems: 'center' }}>
                      <span className="dcs-jack__label">STEP</span>
                    </div>
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
                    <Jack id="play-trig" label="PLAY TRIG" />
                    <Jack id="reset-in" label="RESET" />
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
                    <LitButton color="amber">EXT</LitButton>
                    <Jack id="ext-clk" label="EXT CLK" />
                  </div>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                  <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                    <div className="dcs-step">
                      <div className="dcs-step__btn" onClick={() => setNumSteps(n => Math.min(16, n + 1))}>
                        <Icon name="chevron-up" />
                      </div>
                      <div className="dcs-step__btn" onClick={() => setNumSteps(n => Math.max(1, n - 1))}>
                        <Icon name="chevron-down" />
                      </div>
                    </div>
                    <Lcd value={numSteps} size="md" />
                  </div>
                  <div className="dcs-jack__label">NUM OF STEPS</div>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
                  <Jack id="cv-offset" />
                  <div className="dcs-jack__label">CV OFFSET</div>
                </div>

                <div style={{ display: 'flex', gap: 12 }}>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                    <Knob value={0.45} onChange={() => {}} size={50} />
                    <div className="dcs-jack__label" style={{ marginTop: -4 }}>RATE</div>
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                    <Knob value={0.2} onChange={() => {}} size={50} />
                    <div className="dcs-jack__label" style={{ marginTop: -4 }}>GLIDE</div>
                  </div>
                </div>
              </div>

              <div style={{ display: 'flex', alignItems: 'flex-start', gap: 16, marginTop: 4 }}>
                <div style={{ flex: 1 }}>
                  <div className="dcs-jack__label" style={{ marginBottom: 6 }}>STEP ON / OFF</div>
                  <div style={{ display: 'grid', gridTemplateColumns: 'repeat(8, 1fr)', gap: 6 }}>
                    {steps.slice(0, 8).map((on, i) => (
                      <LitButton key={i} lit={on} color="cyan" onClick={() => toggleStep(i)}>
                        <span style={{ opacity: i === activeStep && playing ? .4 : 1 }}>{i + 1}</span>
                      </LitButton>
                    ))}
                  </div>
                  <div style={{ display: 'grid', gridTemplateColumns: 'repeat(8, 1fr)', gap: 6, marginTop: 10 }}>
                    {steps.slice(0, 8).map((_, i) => (
                      <div key={i} style={{ display: 'flex', justifyContent: 'center' }}>
                        <div style={{ width: 22, position: 'relative', height: 60 }}>
                          <div style={{ position: 'absolute', left: '50%', top: 0, bottom: 0, width: 2, background: 'rgba(0,0,0,.4)', transform: 'translateX(-50%)' }} />
                          <div style={{ position: 'absolute', left: '50%', top: `${20 + (i * 7) % 30}%`, transform: 'translateX(-50%)', width: 12, height: 5, background: 'linear-gradient(180deg, #d8dade, #8a8d94)', border: '1px solid #14161c', borderRadius: 2, cursor: 'ns-resize' }} />
                        </div>
                      </div>
                    ))}
                  </div>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                  <Knob value={0.6} onChange={() => {}} size={50} />
                  <div className="dcs-jack__label">GATE TIME</div>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                  <Jack id="gate-out" />
                  <div className="dcs-jack__label">GATE OUT</div>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                  <Jack id="cv-out" />
                  <div className="dcs-jack__label">CV OUT</div>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
                  <LedMeter value={meter} segments={14} vertical />
                  <div className="dcs-jack__label">LEVEL</div>
                </div>
              </div>
            </div>

            {/* Second module — destination */}
            <div className="dcs-hw dcs-hw--brushed" style={{ marginTop: 18, padding: '24px 20px', position: 'relative', display: 'flex', alignItems: 'center', gap: 20 }}>
              <span className="dcs-hw__screw dcs-hw__screw--tl" />
              <span className="dcs-hw__screw dcs-hw__screw--tr" />
              <span className="dcs-hw__screw dcs-hw__screw--bl" />
              <span className="dcs-hw__screw dcs-hw__screw--br" />
              <div className="dcs-hw__label" style={{ letterSpacing: '.18em' }}>VCO ▸ INPUTS</div>
              <div style={{ display: 'flex', gap: 14, marginLeft: 'auto' }}>
                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}><Jack id="vco-1v" /><div className="dcs-jack__label">1V/OCT</div></div>
                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}><Jack id="vco-gate" /><div className="dcs-jack__label">GATE IN</div></div>
                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}><Jack id="vco-fm" /><div className="dcs-jack__label">FM</div></div>
                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}><Jack id="vco-sync" /><div className="dcs-jack__label">SYNC</div></div>
              </div>
            </div>
          </PatchBoard>
        </div>
        <div className="dw-demo__caption">
          <span>// Drag a jack onto another to patch · click a cable to disconnect</span>
        </div>
      </div>

      {/* Component reference grid */}
      <h3 style={{ marginTop: 40, marginBottom: 12 }}>Components</h3>
      <div className="dw-demo dw-demo--app" style={{ padding: 28, marginBottom: 32 }}>
        <div className="dcs" data-dcs-demo>
          <PatchBoard>
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(180px, 1fr))', gap: 28, color: 'var(--dcs-text)' }}>
              <div>
                <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 12 }}>Patch jacks</div>
                <div style={{ display: 'flex', gap: 16, alignItems: 'flex-end' }}>
                  <Jack id="ref-in"  label="IN" />
                  <Jack id="ref-out" label="OUT" />
                  <Jack id="ref-thru" label="THRU" />
                  <Jack id="ref-aux" label="AUX" />
                </div>
              </div>

              <div>
                <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 12 }}>Backlit buttons</div>
                <div style={{ display: 'flex', gap: 8 }}>
                  <LitButton lit>1</LitButton>
                  <LitButton lit color="cyan">2</LitButton>
                  <LitButton lit color="amber">3</LitButton>
                  <LitButton lit color="green">4</LitButton>
                  <LitButton>5</LitButton>
                </div>
              </div>

              <div>
                <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 12 }}>7-segment LEDs</div>
                <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                  <Lcd value="888" size="md" />
                  <Lcd value="42" size="md" color="amber" />
                  <Lcd value="OK" size="md" color="green" />
                </div>
              </div>

              <div>
                <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 12 }}>LED meters</div>
                <div style={{ display: 'flex', gap: 12, alignItems: 'flex-end' }}>
                  <div style={{ display: 'flex', flexDirection: 'column', gap: 4, alignItems: 'center' }}>
                    <LedMeter value={meter} segments={14} vertical />
                    <div className="dcs-jack__label">L</div>
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', gap: 4, alignItems: 'center' }}>
                    <LedMeter value={Math.max(0.1, meter - 0.1)} segments={14} vertical />
                    <div className="dcs-jack__label">R</div>
                  </div>
                </div>
              </div>

              <div>
                <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 12 }}>Modular row</div>
                <div style={{ display: 'flex', gap: 12, alignItems: 'flex-end' }}>
                  <Jack id="ref-vco" label="VCO" />
                  <Jack id="ref-vcf" label="VCF" />
                  <Jack id="ref-vca" label="VCA" />
                  <Jack id="ref-env" label="ENV" />
                </div>
              </div>
            </div>
          </PatchBoard>
        </div>
        <div className="dw-demo__caption">
          <span>// Drag jacks to patch · click a connected jack to grab its plug</span>
        </div>
      </div>
    </section>
  );
}

Object.assign(window, { SectionSkeuomorphic, Jack, LitButton, Lcd, LedMeter, PatchBoard });
