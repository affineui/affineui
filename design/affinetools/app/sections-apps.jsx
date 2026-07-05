/* sections-apps.jsx
   The two showpieces: Sample DCC tool · Sample synthesizer
*/
const { useState: useStateA, useEffect: useEffectA, useRef: useRefA } = React;

/* ─────────────────────────────────────────────────────────────
   Sample DCC tool — "decius modeler"
   A scaled-down composite of every DCC pattern in the framework.
   ───────────────────────────────────────────────────────────── */
function SampleDCC() {
  const [tool, setTool] = useStateA('select');
  const [shading, setShading] = useStateA('shaded');
  const [snap, setSnap] = useStateA(true);
  const [pos, setPos] = useStateA({ x: 0.000, y: 1.250, z: -0.300 });
  const [rot, setRot] = useStateA({ x: 0, y: 22, z: 0 });
  const [scale, setScale] = useStateA(0.4);
  const [rough, setRough] = useStateA(0.42);
  const [metallic, setMetallic] = useStateA(0.08);
  const [wire, setWire] = useStateA(false);
  const [smooth, setSmooth] = useStateA(true);
  const [shadow, setShadow] = useStateA(true);
  const [inView, setInView] = useStateA(true);
  const [frame, setFrame] = useStateA(48);
  const [playing, setPlaying] = useStateA(false);
  const [expanded, setExpanded] = useStateA(new Set(['scene', 'env', 'chars', 'jane']));
  const [sel, setSel] = useStateA('cube');

  useEffectA(() => {
    if (!playing) return;
    const id = setInterval(() => setFrame(f => (f >= 240 ? 1 : f + 1)), 1000 / 24);
    return () => clearInterval(id);
  }, [playing]);

  const toggle = (id) => setExpanded(prev => { const n = new Set(prev); n.has(id) ? n.delete(id) : n.add(id); return n; });

  return (
    <div className="dcs" data-dcs-density="compact" style={{ background: 'var(--dcs-bg-app)', height: 660, display: 'flex', flexDirection: 'column' }}>
      {/* Top menu bar */}
      <div style={{
        height: 28, display: 'flex', alignItems: 'center', gap: 0,
        background: 'linear-gradient(180deg, #3c424f, #2a2e38)',
        borderBottom: '1px solid var(--dcs-line)',
        padding: '0 8px',
        fontSize: 11,
      }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6, paddingRight: 12, marginRight: 8, borderRight: '1px solid var(--dcs-line)' }}>
          <Icon name="decius" style={{ color: 'var(--dcs-accent)' }} />
          <span style={{ fontWeight: 600, letterSpacing: '.02em' }}>modeler</span>
        </div>
        {['File', 'Edit', 'View', 'Mesh', 'Animation', 'Render', 'Window', 'Help'].map(m => (
          <button key={m} style={{
            background: 'none', border: 'none', color: 'var(--dcs-text-dim)',
            padding: '4px 10px', cursor: 'pointer', fontSize: 11, height: '100%',
          }}
            onMouseEnter={e => e.currentTarget.style.background = 'var(--dcs-surface-2)'}
            onMouseLeave={e => e.currentTarget.style.background = 'none'}>
            {m}
          </button>
        ))}
        <div style={{ flex: 1 }} />
        <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text-mute)' }}>Scene_Intro_v014.dcs</span>
        <span className="dcs-badge dcs-badge--accent" style={{ marginLeft: 10 }}>modified</span>
      </div>

      {/* Tool ribbon */}
      <Toolbar>
        <ButtonGroup value={tool} onChange={setTool} options={[
          { value: 'select', icon: 'select' },
          { value: 'move',   icon: 'move' },
          { value: 'rotate', icon: 'rotate' },
          { value: 'scale',  icon: 'scale-corners' },
        ]} />
        <ToolbarSep />
        <Button ghost sm icon iconLeft="cube" />
        <Button ghost sm icon iconLeft="sphere" />
        <Button ghost sm icon iconLeft="cylinder" />
        <Button ghost sm icon iconLeft="plane" />
        <Button ghost sm icon iconLeft="light" />
        <Button ghost sm icon iconLeft="camera" />
        <ToolbarSep />
        <Button ghost sm icon iconLeft="extrude" />
        <Button ghost sm icon iconLeft="subdivide" />
        <Button ghost sm icon iconLeft="mirror" />
        <Button ghost sm icon iconLeft="array" />
        <ToolbarSep />
        <Button ghost sm icon iconLeft="snap-grid" pressed={snap} onClick={() => setSnap(!snap)} />
        <Button ghost sm icon iconLeft="magnet" />
        <Button ghost sm icon iconLeft="pivot" />
        <ToolbarSep />
        <ButtonGroup value={shading} onChange={setShading} options={[
          { value: 'wire',    icon: 'view-wire' },
          { value: 'shaded',  icon: 'view-solid' },
          { value: 'tex',     icon: 'view-tex' },
          { value: 'render',  icon: 'view-render' },
        ]} />
        <div style={{ flex: 1 }} />
        <Button ghost sm icon iconLeft="undo" />
        <Button ghost sm icon iconLeft="redo" />
        <ToolbarSep />
        <Button primary sm iconLeft="render">Render</Button>
      </Toolbar>

      {/* Main split */}
      <div style={{ flex: 1, display: 'flex', minHeight: 0 }}>
        {/* Outliner */}
        <div style={{ width: 220, borderRight: '1px solid var(--dcs-line)', background: 'var(--dcs-bg)', display: 'flex', flexDirection: 'column' }}>
          <div className="dcs-panel__header"><div className="dcs-panel__title"><Icon name="layers" /><span>Outliner</span></div>
            <div className="dcs-panel__tools"><Button ghost sm icon iconLeft="search" /><Button ghost sm icon iconLeft="more-h" /></div>
          </div>
          <div style={{ flex: 1, overflow: 'auto' }}>
            <Tree
              expanded={expanded}
              onExpand={toggle}
              selected={sel}
              onSelect={setSel}
              nodes={[{
                id: 'scene', label: 'Scene_Intro_v014', icon: 'globe', meta: '37',
                children: [
                  { id: 'env', label: 'Environment', icon: 'folder-open', children: [
                    { id: 'hdri', label: 'Sky_4k.hdr', icon: 'image' },
                    { id: 'sun',  label: 'Sun.001',   icon: 'light' },
                    { id: 'grnd', label: 'Ground',    icon: 'plane' },
                  ]},
                  { id: 'chars', label: 'Characters', icon: 'folder-open', children: [
                    { id: 'jane', label: 'Jane', icon: 'folder-open', children: [
                      { id: 'cube', label: 'jane_body', icon: 'mesh', meta: '64k' },
                      { id: 'rig',  label: 'jane_rig',  icon: 'bone' },
                      { id: 'mat',  label: 'jane_skin', icon: 'palette' },
                    ]},
                    { id: 'eric', label: 'Eric',     icon: 'folder' },
                  ]},
                  { id: 'cams', label: 'Cameras', icon: 'folder', meta: '3' },
                  { id: 'fx',   label: 'FX',      icon: 'folder', meta: '7' },
                ]
              }]}
            />
          </div>
        </div>

        {/* Viewport */}
        <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minWidth: 0 }}>
          <div style={{
            flex: 1, minHeight: 0, position: 'relative',
            background: 'radial-gradient(ellipse at 50% 38%, #3a4054, #161922 80%)',
            overflow: 'hidden',
          }}>
            {/* Grid floor */}
            <svg viewBox="-200 -120 400 240" preserveAspectRatio="xMidYMid slice" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
              <defs>
                <pattern id="dcc-grid" width="20" height="20" patternUnits="userSpaceOnUse" patternTransform="skewX(0) scale(1.5, 0.5)">
                  <path d="M 20 0 L 0 0 0 20" fill="none" stroke="rgba(255,255,255,.07)" strokeWidth=".3" />
                </pattern>
              </defs>
              <rect x="-200" y="0" width="400" height="120" fill="url(#dcc-grid)" />
              <line x1="-200" y1="0" x2="200" y2="0" stroke="rgba(255,255,255,.15)" strokeWidth=".5" />
              {/* Object: a faceted form */}
              <g transform={`translate(${pos.x * 12} ${-(pos.y * 12)}) rotate(${rot.y})`}>
                <g style={{ filter: 'drop-shadow(0 4px 12px rgba(0,0,0,.5))' }}>
                  <path d="M-22-22 L22-22 L28-14 L28 18 L-16 22 L-22 12 Z" fill="rgba(77,159,255,.22)" stroke="#4d9fff" strokeWidth=".4" />
                  <path d="M-22-22 L28-14 L28 18 L-22 12 Z" fill="rgba(77,159,255,.10)" stroke="rgba(77,159,255,.35)" strokeWidth=".3" />
                  <path d="M-22-22 L22-22 L28-14" fill="rgba(255,255,255,.07)" stroke="rgba(255,255,255,.25)" strokeWidth=".25" />
                  {/* Gizmo */}
                  <line x1="0" y1="0" x2="30" y2="0" stroke="#ef6b6b" strokeWidth=".5" />
                  <line x1="0" y1="0" x2="0" y2="-26" stroke="#4ed18a" strokeWidth=".5" />
                  <line x1="0" y1="0" x2="-12" y2="6" stroke="#4d9fff" strokeWidth=".5" />
                  <circle cx="0" cy="0" r="1.5" fill="#fff" />
                </g>
              </g>
              {/* Light helper */}
              <g transform="translate(-100,-60)" stroke="rgba(255,184,77,.5)" strokeWidth=".3" fill="none">
                <circle cx="0" cy="0" r="6" />
                <line x1="-10" y1="0" x2="10" y2="0" />
                <line x1="0" y1="-10" x2="0" y2="10" />
                <line x1="-7" y1="-7" x2="7" y2="7" />
                <line x1="-7" y1="7" x2="7" y2="-7" />
              </g>
            </svg>

            {/* Overlay HUD */}
            <div style={{ position: 'absolute', top: 8, left: 12, display: 'flex', gap: 4 }}>
              <span className="dcs-badge dcs-badge--accent">PERSPECTIVE</span>
              <span className="dcs-badge">FRAME {String(frame).padStart(3, '0')}</span>
              <span className="dcs-badge dcs-badge--ok dcs-badge--dot">SOLVED</span>
            </div>
            <div style={{ position: 'absolute', top: 8, right: 12, display: 'flex', gap: 4 }}>
              <Button ghost sm icon iconLeft="zoom-in" />
              <Button ghost sm icon iconLeft="zoom-out" />
              <Button ghost sm icon iconLeft="fit" />
              <Button ghost sm icon iconLeft="fullscreen" />
            </div>
            <div style={{
              position: 'absolute', bottom: 8, right: 12,
              fontFamily: 'var(--dcs-font-mono)', fontSize: 10, color: 'var(--dcs-text-mute)',
              textAlign: 'right', lineHeight: 1.5
            }}>
              <div>{sel === 'cube' ? 'jane_body · 64,201 tri · 32,108 verts' : sel}</div>
              <div style={{ color: 'var(--dcs-accent)' }}>60 fps · 4.1 ms · 312 MB</div>
            </div>
            {/* Axis gizmo — top-right of viewport */}
            <svg viewBox="-36 -36 72 72" style={{ position: 'absolute', bottom: 12, left: 12, width: 72, height: 72, opacity: .95 }}>
              <defs>
                <marker id="dcc-arr-x" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
                  <path d="M0 0 L10 5 L0 10 Z" fill="#ef6b6b" />
                </marker>
                <marker id="dcc-arr-y" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
                  <path d="M0 0 L10 5 L0 10 Z" fill="#4ed18a" />
                </marker>
                <marker id="dcc-arr-z" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
                  <path d="M0 0 L10 5 L0 10 Z" fill="#4d9fff" />
                </marker>
              </defs>
              <circle cx="0" cy="0" r="32" fill="rgba(0,0,0,.3)" stroke="rgba(255,255,255,.06)" strokeWidth=".5" />
              <line x1="0" y1="0" x2="24" y2="0" stroke="#ef6b6b" strokeWidth="2" markerEnd="url(#dcc-arr-x)" />
              <line x1="0" y1="0" x2="0" y2="-24" stroke="#4ed18a" strokeWidth="2" markerEnd="url(#dcc-arr-y)" />
              <line x1="0" y1="0" x2="-17" y2="14" stroke="#4d9fff" strokeWidth="2" markerEnd="url(#dcc-arr-z)" />
              <text x="30" y="3" fill="#ef6b6b" fontSize="11" fontFamily="JetBrains Mono" fontWeight="600">X</text>
              <text x="-3" y="-28" fill="#4ed18a" fontSize="11" fontFamily="JetBrains Mono" fontWeight="600">Y</text>
              <text x="-28" y="22" fill="#4d9fff" fontSize="11" fontFamily="JetBrains Mono" fontWeight="600">Z</text>
              <circle cx="0" cy="0" r="2.5" fill="#fff" />
            </svg>
          </div>

          {/* Timeline */}
          <div style={{
            height: 80, borderTop: '1px solid var(--dcs-line)', background: 'var(--dcs-bg)',
            padding: '6px 10px', display: 'flex', flexDirection: 'column', gap: 6,
          }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <Button ghost sm icon iconLeft="skip-back" onClick={() => setFrame(1)} />
              <Button ghost sm icon iconLeft="rewind" onClick={() => setFrame(f => Math.max(1, f - 24))} />
              <Button ghost sm icon={playing ? 'pause' : 'play'} iconLeft={playing ? 'pause' : 'play'} pressed={playing} onClick={() => setPlaying(p => !p)} />
              <Button ghost sm icon iconLeft="fast-forward" onClick={() => setFrame(f => Math.min(240, f + 24))} />
              <Button ghost sm icon iconLeft="skip-fwd" onClick={() => setFrame(240)} />
              <Combo value={frame} onChange={setFrame} min={1} max={240} step={1} format={v => `F ${String(v).padStart(3,'0')}`} width={100} />
              <span style={{ flex: 1 }} />
              <Button ghost sm icon iconLeft="record" />
              <Button ghost sm icon iconLeft="key" />
            </div>
            <div style={{ flex: 1, position: 'relative', background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3, overflow: 'hidden' }}>
              {/* Frame ticks */}
              {Array.from({ length: 25 }).map((_, i) => (
                <div key={i} style={{
                  position: 'absolute', left: `${(i / 24) * 100}%`, top: 0, bottom: 0, width: 1,
                  background: i % 4 === 0 ? 'rgba(255,255,255,.12)' : 'rgba(255,255,255,.04)',
                }} />
              ))}
              {/* Keyframes */}
              {[12, 24, 48, 64, 96, 128, 160, 192, 224].map(f => (
                <div key={f} style={{
                  position: 'absolute', left: `${(f / 240) * 100}%`, top: '50%',
                  width: 8, height: 8, marginLeft: -4, marginTop: -4,
                  background: 'var(--dcs-accent)', transform: 'rotate(45deg)',
                  boxShadow: '0 0 6px rgba(77,159,255,.5)',
                }} />
              ))}
              {/* Playhead */}
              <div style={{
                position: 'absolute', left: `${(frame / 240) * 100}%`, top: -4, bottom: -4,
                width: 2, background: 'var(--dcs-accent-hi)',
                boxShadow: '0 0 6px rgba(111,179,255,.6)',
              }} />
              <div style={{
                position: 'absolute', left: `${(frame / 240) * 100}%`, top: 0,
                marginLeft: -8, fontSize: 9, fontFamily: 'var(--dcs-font-mono)',
                color: 'var(--dcs-accent)', background: 'var(--dcs-bg)', padding: '0 4px',
              }}>{String(frame).padStart(3, '0')}</div>
            </div>
          </div>
        </div>

        {/* Inspector */}
        <div style={{ width: 240, borderLeft: '1px solid var(--dcs-line)', background: 'var(--dcs-bg)', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
          <div style={{
            display: 'flex', alignItems: 'center', gap: 6,
            height: 24, padding: '0 10px',
            background: 'var(--dcs-surface-1)',
            borderBottom: '1px solid var(--dcs-line)',
            fontSize: 11, fontWeight: 500, color: 'var(--dcs-text-dim)',
          }}>
            <Icon name="cog" size="sm" />
            <span style={{ flex: 1 }}>Inspector</span>
            <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-accent)' }}>jane_body</span>
            <Button ghost sm icon iconLeft="pin" />
            <Button ghost sm icon iconLeft="more-h" />
          </div>
          <div style={{ flex: 1, overflow: 'auto' }}>
            <Foldouts>
              <Foldout title="Transform" icon="move" tools={<Button ghost sm icon iconLeft="key" />}>
                <div className="dcs-props">
                  {[['X', '#ef6b6b', pos.x, 'x'], ['Y', '#4ed18a', pos.y, 'y'], ['Z', '#4d9fff', pos.z, 'z']].map(([k, c, v, key]) => (
                    <div key={k} className="dcs-field">
                      <span className="dcs-field__label" style={{ flex: '0 0 14px', color: c, fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums', fontSize: 11 }}>{k}</span>
                      <Combo value={v} onChange={nv => setPos(p => ({ ...p, [key]: nv }))} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} />
                    </div>
                  ))}
                </div>
              </Foldout>
              <Foldout title="Rotation" icon="rotate">
                <div className="dcs-props">
                  {[['X', '#ef6b6b', rot.x, 'x'], ['Y', '#4ed18a', rot.y, 'y'], ['Z', '#4d9fff', rot.z, 'z']].map(([k, c, v, key]) => (
                    <div key={k} className="dcs-field">
                      <span className="dcs-field__label" style={{ flex: '0 0 14px', color: c, fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums', fontSize: 11 }}>{k}</span>
                      <Combo value={v} onChange={nv => setRot(r => ({ ...r, [key]: nv }))} min={-180} max={180} step={1} format={v => `${v.toFixed(0)}°`} />
                    </div>
                  ))}
                </div>
              </Foldout>
              <Foldout title="Display" icon="eye">
                <div className="dcs-props">
                  <div className="dcs-field"><span className="dcs-field__label">Wireframe</span><Switch checked={wire} onChange={setWire} /></div>
                  <div className="dcs-field"><span className="dcs-field__label">Smooth shade</span><Switch checked={smooth} onChange={setSmooth} /></div>
                  <div className="dcs-field"><span className="dcs-field__label">Cast shadow</span><Switch checked={shadow} onChange={setShadow} /></div>
                  <div className="dcs-field"><span className="dcs-field__label">In viewport</span><Switch checked={inView} onChange={setInView} /></div>
                </div>
              </Foldout>
              <Foldout title="Material" icon="palette" meta="Lambert.001">
                <div className="dcs-props">
                  <div className="dcs-field">
                    <span className="dcs-field__label">Albedo</span>
                    <div className="dcs-swatch"><div className="dcs-swatch__chip" style={{ '--c': '#4d9fff' }} /><span>#4D9FFF</span></div>
                  </div>
                  <div className="dcs-field"><span className="dcs-field__label">Rough</span><Slider value={rough} onChange={setRough} /></div>
                  <div className="dcs-field"><span className="dcs-field__label">Metal</span><Slider value={metallic} onChange={setMetallic} /></div>
                </div>
              </Foldout>
              <Foldout title="Subdivision" icon="subdivide" defaultOpen={false} meta={`L${Math.round(scale * 6)}`}>
                <div className="dcs-props">
                  <div className="dcs-field"><span className="dcs-field__label">Level</span><Slider value={scale} onChange={setScale} /></div>
                  <div className="dcs-field"><span className="dcs-field__label">Triangles</span>
                    <span className="dcs-num" style={{ color: 'var(--dcs-accent)', fontSize: 11 }}>{Math.round(64201 * Math.pow(4, scale * 2)).toLocaleString()}</span>
                  </div>
                </div>
              </Foldout>
              <Foldout title="Modifiers" icon="bolt" defaultOpen={false} meta="3" />
              <Foldout title="Constraints" icon="link" defaultOpen={false} />
            </Foldouts>
          </div>
        </div>
      </div>

      {/* Status bar */}
      <div style={{
        height: 22, display: 'flex', alignItems: 'center', gap: 12,
        background: 'var(--dcs-surface-1)',
        borderTop: '1px solid var(--dcs-line)',
        padding: '0 12px', fontSize: 10, fontFamily: 'var(--dcs-font-mono)',
        color: 'var(--dcs-text-mute)',
      }}>
        <span className="dcs-badge dcs-badge--ok dcs-badge--dot" style={{ fontSize: 9, height: 14 }}>READY</span>
        <span>vertices 32,108 · faces 64,201 · selected 1</span>
        <span style={{ flex: 1 }} />
        <span>GPU · RTX 4090 · 312 / 24576 MB</span>
        <span style={{ color: 'var(--dcs-accent)' }}>60.0 fps</span>
      </div>
    </div>
  );
}

/* ─────────────────────────────────────────────────────────────
   Sample synth — "decius synthesis"
   Big knob array, fader strip, envelope+lfo, FX rack.
   ───────────────────────────────────────────────────────────── */
function SampleSynth() {
  const [k, setK] = useStateA({
    osc1: 0.7, osc2: 0.3, oscMix: 0.5, detune: 0.45,
    cutoff: 0.62, reso: 0.3, drive: 0.18, env: 0.55,
    attack: 0.04, decay: 0.4, sustain: 0.65, release: 0.45,
    lfoRate: 0.35, lfoAmt: 0.4,
    reverb: 0.3, delay: 0.2, chorus: 0.15,
    master: 0.78,
  });
  const upd = (key, v) => setK(o => ({ ...o, [key]: v }));
  const [osc1Wave, setOsc1Wave] = useStateA('saw');
  const [osc2Wave, setOsc2Wave] = useStateA('square');
  const [filterMode, setFilterMode] = useStateA('lp');
  const [armed, setArmed] = useStateA(false);
  const [bpm, setBpm] = useStateA(120);

  // Animated meter
  const [meter, setMeter] = useStateA(0.4);
  useEffectA(() => {
    const id = setInterval(() => setMeter(m => Math.max(0, Math.min(1, m + (Math.random() - 0.5) * 0.4))), 80);
    return () => clearInterval(id);
  }, []);

  const Section = ({ title, icon, color, children, span, hw }) => (
    <div className={hw ? `dcs-hw dcs-hw--${hw}` : ''} style={{
      background: hw ? undefined : 'var(--dcs-bg)',
      border: hw ? undefined : '1px solid var(--dcs-line)',
      borderRadius: hw ? undefined : 'var(--dcs-r-2)',
      boxShadow: hw ? undefined : 'var(--dcs-shadow-2)',
      gridColumn: span ? `span ${span}` : undefined,
      display: 'flex', flexDirection: 'column',
      position: 'relative',
    }}>
      {hw && <>
        <span className="dcs-hw__screw dcs-hw__screw--tl" />
        <span className="dcs-hw__screw dcs-hw__screw--tr" />
        <span className="dcs-hw__screw dcs-hw__screw--bl" />
        <span className="dcs-hw__screw dcs-hw__screw--br" />
      </>}
      {title && (
        <div className={hw ? 'dcs-hw__label' : ''} style={{
          position: 'absolute', top: hw ? 10 : 8, left: hw ? 22 : 12,
          display: 'flex', alignItems: 'center', gap: 6,
          color: hw ? undefined : (color || 'var(--dcs-accent)'),
          fontSize: hw ? undefined : 9,
          fontWeight: hw ? undefined : 600,
          textTransform: 'uppercase',
          letterSpacing: hw ? undefined : '.12em',
          opacity: hw ? undefined : .7,
          pointerEvents: 'none',
        }}>
          {icon && !hw && <Icon name={icon} size="sm" />}
          <span>{title}</span>
        </div>
      )}
      <div style={{ flex: 1, padding: hw ? '26px 18px 18px' : '22px 14px 14px', display: 'flex', flexDirection: 'column' }}>{children}</div>
    </div>
  );

  return (
    <div className="dcs" data-dcs-style="3d" style={{ background: 'var(--dcs-bg-app)', padding: 12 }}>
      {/* Brand header */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 12,
        padding: '6px 14px 12px', marginBottom: 12,
        borderBottom: '1px solid var(--dcs-line)',
      }}>
        <div style={{
          width: 36, height: 36, borderRadius: 8,
          background: 'linear-gradient(135deg, var(--dcs-accent), var(--dcs-accent-lo))',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          color: '#0a1220', boxShadow: 'inset 0 1px 0 rgba(255,255,255,.3), 0 4px 12px rgba(77,159,255,.3)',
        }}>
          <Icon name="wave-saw" />
        </div>
        <div>
          <div style={{ fontSize: 16, fontWeight: 600, letterSpacing: '-.01em' }}>decius synthesis</div>
          <div className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text-mute)' }}>polyphonic dual-osc · subtractive · v0.4</div>
        </div>
        <span style={{ flex: 1 }} />
        <div className="dcs-row" style={{ gap: 8 }}>
          <Combo value={bpm} onChange={setBpm} min={20} max={300} step={1} format={v => `${v} BPM`} width={100} />
          <Button ghost sm icon iconLeft="record" pressed={armed} onClick={() => setArmed(!armed)} />
          <Button ghost sm icon iconLeft="play" />
          <Button ghost sm icon iconLeft="stop" />
          <div className="dcs-divider--v" />
          {/* Master meter */}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2, alignItems: 'flex-end' }}>
            <div style={{ display: 'flex', gap: 1, height: 10, alignItems: 'flex-end' }}>
              {Array.from({ length: 24 }).map((_, i) => {
                const t = i / 23;
                const active = t < meter;
                const color = t > 0.9 ? '#ef6b6b' : t > 0.75 ? '#f2b14a' : '#4ed18a';
                return <div key={i} style={{ width: 3, height: 10, background: active ? color : 'rgba(255,255,255,.06)', borderRadius: 1 }} />;
              })}
            </div>
            <span className="dcs-mono" style={{ fontSize: 9, color: 'var(--dcs-text-mute)' }}>{Math.round((meter - 0.75) * 60)} dB</span>
          </div>
        </div>
      </div>

      <div style={{
        display: 'grid',
        gridTemplateColumns: '1.4fr 1fr 1.2fr',
        gridAutoRows: 'minmax(0, auto)',
        gap: 10,
      }}>
        {/* Oscillators */}
        <Section title="Oscillators" icon="wave-saw" span={2} hw="brushed">
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 18 }}>
            {[
              { name: 'OSC 1', wave: osc1Wave, setWave: setOsc1Wave, level: k.osc1, levelKey: 'osc1' },
              { name: 'OSC 2', wave: osc2Wave, setWave: setOsc2Wave, level: k.osc2, levelKey: 'osc2' },
            ].map(o => (
              <div key={o.name} style={{ padding: 10, background: 'transparent', borderRadius: 6, border: '1px solid rgba(255,255,255,.45)' }}>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 10 }}>
                  <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-accent)', letterSpacing: '.08em' }}>{o.name}</span>
                  <ButtonGroup value={o.wave} onChange={o.setWave} options={[
                    { value: 'sine',   icon: 'wave-sine' },
                    { value: 'tri',    icon: 'wave-tri' },
                    { value: 'saw',    icon: 'wave-saw' },
                    { value: 'square', icon: 'wave-square' },
                    { value: 'noise',  icon: 'wave-noise' },
                  ]} />
                </div>
                <div style={{ display: 'flex', justifyContent: 'space-around', alignItems: 'flex-end', paddingBottom: 14 }}>
                  <Knob value={o.level} onChange={v => upd(o.levelKey, v)} size={48} label="LEVEL" format={v => `${(v*100).toFixed(0)}`} />
                  <Knob value={k.detune} onChange={v => upd('detune', v)} size={48} label="DETUNE" bipolar format={v => `${((v-0.5)*100).toFixed(0)}ct`} />
                  <Knob value={k.oscMix} onChange={v => upd('oscMix', v)} size={48} label="OCTAVE" bipolar format={v => `${Math.round((v-0.5)*4)}`} />
                </div>
              </div>
            ))}
          </div>
        </Section>

        {/* Filter */}
        <Section title="Filter" icon="filter-lp" color="#b48cff" hw="brushed">
          <ButtonGroup value={filterMode} onChange={setFilterMode} options={[
            { value: 'lp', icon: 'filter-lp', label: 'LP' },
            { value: 'hp', icon: 'filter-hp', label: 'HP' },
            { value: 'bp', icon: 'filter-bp', label: 'BP' },
            { value: 'nt', icon: 'filter-notch', label: 'Nt' },
          ]} />
          <div style={{ display: 'flex', justifyContent: 'space-around', alignItems: 'flex-end', marginTop: 18, paddingBottom: 14 }}>
            <Knob value={k.cutoff} onChange={v => upd('cutoff', v)} size={56} label="CUTOFF" format={v => `${Math.round(v * 22050)}`} />
            <Knob value={k.reso} onChange={v => upd('reso', v)} size={56} label="RESO" format={v => `${(v*100).toFixed(0)}%`} />
            <Knob value={k.drive} onChange={v => upd('drive', v)} size={56} label="DRIVE" format={v => `${(v*24).toFixed(1)}`} />
          </div>
        </Section>

        {/* Envelope */}
        <Section title="Amp envelope" icon="envelope" color="#4ed18a" hw="brushed">
          <div style={{ height: 78, marginBottom: 8 }}>
            <svg viewBox="0 0 200 80" preserveAspectRatio="none" style={{ width: '100%', height: '100%', background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3 }}>
              {(() => {
                const ax = k.attack * 60;
                const dx = k.decay * 60;
                const sy = (1 - k.sustain) * 60 + 10;
                const rx = k.release * 60;
                return <>
                  <path d={`M 5 70 L ${5+ax} 10 L ${5+ax+dx} ${sy} L ${130} ${sy} L ${130+rx} 70`}
                        fill="rgba(78,209,138,.18)" stroke="#4ed18a" strokeWidth="1.25" strokeLinejoin="round" />
                  <line x1="5" y1="70" x2="195" y2="70" stroke="rgba(255,255,255,.1)" strokeWidth=".5" />
                  <circle cx={5+ax} cy="10" r="2.5" fill="#4ed18a" />
                  <circle cx={5+ax+dx} cy={sy} r="2.5" fill="#4ed18a" />
                  <circle cx={130} cy={sy} r="2.5" fill="#4ed18a" />
                  <circle cx={130+rx} cy="70" r="2.5" fill="#4ed18a" />
                </>;
              })()}
            </svg>
          </div>
          <div style={{ display: 'flex', justifyContent: 'space-around', alignItems: 'flex-end', paddingBottom: 14 }}>
            <Knob value={k.attack} onChange={v => upd('attack', v)} size={40} label="A" format={v => `${(v*5000).toFixed(0)}ms`} />
            <Knob value={k.decay} onChange={v => upd('decay', v)} size={40} label="D" format={v => `${(v*5000).toFixed(0)}ms`} />
            <Knob value={k.sustain} onChange={v => upd('sustain', v)} size={40} label="S" format={v => `${(v*100).toFixed(0)}%`} />
            <Knob value={k.release} onChange={v => upd('release', v)} size={40} label="R" format={v => `${(v*5000).toFixed(0)}ms`} />
          </div>
        </Section>

        {/* LFO */}
        <Section title="LFO 1" icon="lfo" color="#ff7ab8" hw="brushed">
          <div style={{ marginBottom: 6 }}>
            <ButtonGroup value="sine" onChange={() => {}} options={[
              { value: 'sine', icon: 'wave-sine' },
              { value: 'tri', icon: 'wave-tri' },
              { value: 'sq', icon: 'wave-square' },
              { value: 'sh', icon: 'wave-noise' },
            ]} />
          </div>
          <div style={{ height: 50, marginBottom: 6 }}>
            <svg viewBox="0 0 200 50" preserveAspectRatio="none" style={{ width: '100%', height: '100%', background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3 }}>
              {(() => {
                const freq = 1 + k.lfoRate * 8;
                const amp = k.lfoAmt * 18;
                let d = `M 0 25 `;
                for (let x = 0; x <= 200; x += 2) {
                  d += `L ${x} ${25 - Math.sin((x / 200) * freq * Math.PI * 2) * amp} `;
                }
                return <path d={d} fill="none" stroke="#ff7ab8" strokeWidth="1.25" />;
              })()}
            </svg>
          </div>
          <div style={{ display: 'flex', justifyContent: 'space-around', alignItems: 'flex-end', paddingBottom: 14 }}>
            <Knob value={k.lfoRate} onChange={v => upd('lfoRate', v)} size={44} label="RATE" format={v => `${(1+v*8).toFixed(2)}Hz`} />
            <Knob value={k.lfoAmt} onChange={v => upd('lfoAmt', v)} size={44} label="AMT" format={v => `${(v*100).toFixed(0)}%`} />
            <div style={{ display: 'flex', flexDirection: 'column', gap: 4, alignItems: 'center' }}>
              <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text-mute)' }}>DEST</span>
              <select className="dcs-select" defaultValue="cut" style={{ width: 70, fontSize: 10 }}>
                <option value="cut">Cutoff</option>
                <option value="pit">Pitch</option>
                <option value="amp">Amp</option>
              </select>
            </div>
          </div>
        </Section>

        {/* FX rack */}
        <Section title="FX" icon="bolt" color="#4ad5d5" span={2} hw="brushed">
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 14 }}>
            {[
              { name: 'CHORUS', v: k.chorus, key: 'chorus', color: '#b48cff' },
              { name: 'DELAY', v: k.delay, key: 'delay', color: '#4ad5d5' },
              { name: 'REVERB', v: k.reverb, key: 'reverb', color: '#4ed18a' },
            ].map(fx => (
              <div key={fx.name} style={{ padding: 10, background: 'transparent', borderRadius: 6, border: '1px solid rgba(255,255,255,.45)', display: 'flex', flexDirection: 'column', gap: 8 }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                  <Switch checked={fx.v > 0.01} onChange={() => {}} />
                  <span className="dcs-mono" style={{ fontSize: 10, color: fx.color, letterSpacing: '.08em' }}>{fx.name}</span>
                </div>
                <div style={{ display: 'flex', justifyContent: 'space-around', paddingBottom: 14, paddingTop: 4 }}>
                  <Knob value={fx.v} onChange={v => upd(fx.key, v)} size={44} label="MIX" format={v => `${(v*100).toFixed(0)}`} />
                  <Knob value={0.5} onChange={() => {}} size={44} label={fx.name === 'DELAY' ? 'TIME' : fx.name === 'CHORUS' ? 'RATE' : 'SIZE'} format={() => '0.50'} />
                </div>
              </div>
            ))}
          </div>
        </Section>

        {/* Master */}
        <Section title="Master" icon="volume" color="#4d9fff" hw="lacquer">
          <div style={{ display: 'flex', gap: 14, alignItems: 'center', justifyContent: 'center', height: '100%' }}>
            <Fader value={k.master} onChange={v => upd('master', v)} height={120} />
            <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
              <span className="dcs-mono" style={{ fontSize: 11, color: 'var(--dcs-accent)' }}>{((k.master - 0.75) * 60).toFixed(1)} dB</span>
              <span className="dcs-mono" style={{ fontSize: 9, color: 'var(--dcs-text-mute)' }}>MASTER OUT</span>
              <hr style={{ width: '100%', border: 'none', borderTop: '1px solid var(--dcs-line)', margin: '6px 0' }} />
              <span className="dcs-mono" style={{ fontSize: 9, color: 'var(--dcs-text-mute)' }}>VOICES</span>
              <span className="dcs-mono" style={{ fontSize: 11, color: 'var(--dcs-text)' }}>4 / 16</span>
              <span className="dcs-mono" style={{ fontSize: 9, color: 'var(--dcs-text-mute)' }}>CPU</span>
              <span className="dcs-mono" style={{ fontSize: 11, color: 'var(--dcs-ok)' }}>12 %</span>
            </div>
          </div>
        </Section>

        {/* Keyboard strip */}
        <div style={{ gridColumn: 'span 3', background: 'var(--dcs-bg)', border: '1px solid var(--dcs-line)', borderRadius: 'var(--dcs-r-2)', padding: 10 }}>
          <div style={{ display: 'flex', gap: 0, height: 70, position: 'relative' }}>
            {(() => {
              const names = ['C','D','E','F','G','A','B'];
              const blackAfter = [true, true, false, true, true, true, false]; // C# D# (none) F# G# A# (none)
              const startOctave = 2;
              const totalOctaves = 4;
              const keys = [];
              for (let o = 0; o < totalOctaves; o++) {
                for (let n = 0; n < 7; n++) {
                  keys.push({ name: names[n], octave: startOctave + o, hasBlack: blackAfter[n] });
                }
              }
              keys.push({ name: 'C', octave: startOctave + totalOctaves, hasBlack: false });
              const highlight = Math.floor(keys.length / 3); // pick a middle-ish key

              return keys.map((k, i) => (
                <div key={i} style={{
                  flex: 1, background: i === highlight
                    ? 'linear-gradient(180deg, #b8d6ff, #6fa8ff)'
                    : 'linear-gradient(180deg, #f8f9fb, #e3e6ec)',
                  border: '1px solid #14161c', borderRight: i === keys.length - 1 ? '1px solid #14161c' : 'none',
                  borderRadius: '0 0 3px 3px',
                  position: 'relative', cursor: 'pointer',
                }}>
                  {k.hasBlack && (
                    <div style={{
                      position: 'absolute', right: '-30%', top: 0,
                      width: '60%', height: '62%',
                      background: 'linear-gradient(180deg, #2a2e38, #14161c)',
                      border: '1px solid #14161c', borderRadius: '0 0 2px 2px',
                      zIndex: 2, cursor: 'pointer',
                    }} />
                  )}
                  {k.name === 'C' && (
                    <div style={{
                      position: 'absolute', bottom: 4, left: 0, right: 0,
                      textAlign: 'center', fontSize: 9, fontFamily: 'var(--dcs-font-mono)',
                      color: i === highlight ? '#1a3a6e' : '#767c8a',
                      fontWeight: 600, pointerEvents: 'none',
                    }}>C{k.octave}</div>
                  )}
                </div>
              ));
            })()}
          </div>
        </div>
      </div>
    </div>
  );
}

function SectionSampleDCC() {
  return (
    <section className="dw-section" id="sample-dcc">
      <div className="dw-section__eyebrow">Showpiece · 01</div>
      <h2>Sample DCC tool</h2>
      <p className="dw-section__lead">
        Every primitive in the framework — toolbar, outliner, viewport HUD, timeline, inspector,
        combo fields, gizmos — assembled into a working modeler shell. Play with the transform
        values, scrub the timeline, swap shading modes.
      </p>
      <div className="dw-demo dw-demo--inset" style={{ padding: 0 }}>
        <SampleDCC />
      </div>
    </section>
  );
}

function SectionSampleSynth() {
  return (
    <section className="dw-section" id="sample-synth">
      <div className="dw-section__eyebrow">Showpiece · 02</div>
      <h2>Sample synthesizer</h2>
      <p className="dw-section__lead">
        The same decius primitives, repurposed as a polyphonic synth. Knobs, faders, segmented
        waveform pickers, FX rack, modulation envelope, signal meter. Drag any knob vertically.
      </p>
      <div className="dw-demo dw-demo--inset" style={{ padding: 0 }}>
        <SampleSynth />
      </div>
    </section>
  );
}

Object.assign(window, { SampleDCC, SampleSynth, SectionSampleDCC, SectionSampleSynth });
