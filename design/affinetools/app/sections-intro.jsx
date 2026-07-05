/* sections-intro.jsx
   Hero · Why · Install
*/
const { useState: useStateH, useEffect: useEffectH } = React;

function HeroDeck() {
  // Animated little knob array that fills out the hero — alive but not noisy
  const [t, setT] = useStateH(0);
  useEffectH(() => {
    const id = setInterval(() => setT(v => v + 0.02), 50);
    return () => clearInterval(id);
  }, []);

  const ring = (i, base) => 0.5 + 0.45 * Math.sin(t * 0.7 + i * 0.9 + base);

  return (
    <div className="dw-stage" style={{ overflow: 'hidden' }}>
      <div className="dw-stage__chrome">
        <div className="dw-stage__lights">
          <div className="dw-stage__light" />
          <div className="dw-stage__light" />
          <div className="dw-stage__light" />
        </div>
        <div className="dw-stage__title">decius · live preview · 1920×1080</div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <Icon name="fullscreen" size="sm" style={{ color: 'var(--dcs-text-mute)' }} />
        </div>
      </div>

      <div className="dcs" style={{ padding: 20, background: 'var(--dcs-bg-app)' }}>
        <div style={{ display: 'grid', gridTemplateColumns: '1.4fr 1fr', gap: 12 }}>
          {/* Left: viewport-y card with knob rack */}
          <Panel title="Filter Bank ▸ Lowpass" icon="filter-lp" headerActive
                 tools={<><Button ghost sm icon iconLeft="bolt" /><Button ghost sm icon iconLeft="more-h" /></>}>
            <div style={{ display: 'flex', justifyContent: 'space-around', padding: '14px 6px 18px', gap: 8 }}>
              <Knob value={ring(0, 0)}     onChange={() => {}} size={56} label="CUT"   format={v => `${Math.round(v * 22050)}`} />
              <Knob value={ring(1, 1.2)}   onChange={() => {}} size={56} label="RES"   format={v => `${(v*100).toFixed(0)}%`} />
              <Knob value={ring(2, 0.4)}   onChange={() => {}} size={56} label="DRIVE" format={v => `${(v*24).toFixed(1)}`} />
              <Knob value={ring(3, 2.1)}   onChange={() => {}} size={56} label="ENV"   bipolar format={v => `${((v-0.5)*2).toFixed(2)}`} />
              <Knob value={ring(4, 1.7)}   onChange={() => {}} size={56} label="LFO"   bipolar format={v => `${((v-0.5)*100).toFixed(0)}`} />
            </div>

            <div style={{ height: 84, marginTop: 6, position: 'relative' }}>
              <svg viewBox="0 0 400 80" preserveAspectRatio="none" style={{ width: '100%', height: '100%', background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3 }}>
                <defs>
                  <linearGradient id="hero-fill" x1="0" x2="0" y1="0" y2="1">
                    <stop offset="0%" stopColor="var(--dcs-accent)" stopOpacity=".4" />
                    <stop offset="100%" stopColor="var(--dcs-accent)" stopOpacity="0" />
                  </linearGradient>
                </defs>
                {[0.25, 0.5, 0.75].map(y => <line key={y} x1="0" y1={y * 80} x2="400" y2={y * 80} stroke="rgba(255,255,255,.05)" strokeWidth=".5" />)}
                {(() => {
                  const cutoff = ring(0, 0);
                  const reso = ring(1, 1.2);
                  let d = `M 0 ${75 - 5 * reso} `;
                  for (let x = 0; x <= 400; x += 4) {
                    const f = x / 400;
                    const resonance = Math.exp(-Math.pow((f - cutoff) * 24, 2)) * reso * 35;
                    const drop = f < cutoff ? 0 : Math.min(60, (f - cutoff) * 180);
                    const y = 75 - resonance + drop;
                    d += `L ${x} ${Math.min(78, y)} `;
                  }
                  return <>
                    <path d={d + ' L 400 80 L 0 80 Z'} fill="url(#hero-fill)" />
                    <path d={d} fill="none" stroke="var(--dcs-accent)" strokeWidth="1.5" />
                  </>;
                })()}
              </svg>
              <div style={{ position: 'absolute', top: 6, left: 8, fontFamily: 'var(--dcs-font-mono)', fontSize: 9, color: 'var(--dcs-text-mute)' }}>FREQ RESP</div>
            </div>
          </Panel>

          {/* Right: outliner + inspector */}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            <Panel title="Outliner" icon="layers" pad={0}>
              <div style={{ padding: '4px 0', minHeight: 0 }}>
                <Tree
                  expanded={new Set(['scn', 'rig'])}
                  onExpand={() => {}}
                  selected="hand"
                  onSelect={() => {}}
                  nodes={[{
                    id: 'scn', label: 'Scene', icon: 'globe',
                    children: [
                      { id: 'jane', label: 'Jane', icon: 'cube', meta: '64k' },
                      { id: 'rig', label: 'jane_rig', icon: 'bone', children: [
                        { id: 'spine', label: 'spine', icon: 'bone' },
                        { id: 'hand', label: 'L_hand', icon: 'bone', meta: 'IK' },
                      ]},
                      { id: 'sun', label: 'Sun.001', icon: 'light' },
                    ]
                  }]}
                />
              </div>
            </Panel>
            <Panel title="Inspector" icon="cog">
              <div className="dcs-col" style={{ gap: 6 }}>
                <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 20 }}>X</label><Combo value={1.428}  onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} width="100%" /></div>
                <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 20 }}>Y</label><Combo value={-0.952} onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} width="100%" /></div>
                <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 20 }}>Z</label><Combo value={3.000}  onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} width="100%" /></div>
              </div>
            </Panel>
          </div>
        </div>

        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr 1fr', gap: 8, marginTop: 10 }}>
          {[
            { l: 'attack', v: ring(5, 0) },
            { l: 'decay', v: ring(6, 0.5) },
            { l: 'sustain', v: ring(7, 1.1) },
            { l: 'release', v: ring(8, 1.6) },
          ].map(s => (
            <div key={s.l} className="dcs-field" style={{ background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3, padding: '4px 6px', gap: 4 }}>
              <span style={{ fontSize: 10, color: 'var(--dcs-text-mute)', textTransform: 'uppercase', letterSpacing: '.06em', minWidth: 0 }}>{s.l}</span>
              <Slider value={s.v} onChange={() => {}} />
              <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-accent)', minWidth: 30, textAlign: 'right' }}>{Math.round(s.v * 100)}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

function SectionHero() {
  return (
    <section className="dw-section" id="top" style={{ marginBottom: 60 }}>
      <a href="#" style={{
        display: 'flex', alignItems: 'center', gap: 10,
        padding: '8px 14px',
        background: 'var(--dw-accent-dim)',
        border: '1px solid rgba(77,159,255,.35)',
        borderRadius: 999,
        fontSize: 12,
        color: 'var(--dw-accent-lo)',
        marginBottom: 28,
        textDecoration: 'none',
        width: 'fit-content',
        fontFamily: 'var(--dw-font-mono)',
        letterSpacing: '.02em',
      }}>
        <Icon name="bolt" size="sm" />
        <span style={{ color: 'var(--dw-text)', fontWeight: 600 }}>Built for AffineUI</span>
        <span>·</span>
        <span>the single-file, zero-dependency, GPU-accelerated UI engine</span>
        <Icon name="chevron-right" size="sm" />
      </a>
      <div className="dw-hero">
        <div>
          <div className="dw-hero__eyebrow">decius.css · v0.4.0 "Mus"</div>
          <h1>The CSS framework for things that aren't websites.</h1>
          <p className="lede">
            A complete component system for digital content creation tools, synths, video editors, and
            pro desktop interfaces — built around the conventions of Blender, DaVinci Resolve, and Vital,
            but distributed as plain CSS and SVG. <strong style={{ color: 'var(--dw-text)' }}>One stylesheet.
            Zero runtime.</strong>
          </p>
          <div className="dw-hero__actions">
            <button className="dw-cta dw-cta--primary">
              <Icon name="rocket" size="sm" /> Get started
            </button>
            <button className="dw-cta dw-cta--ghost">
              <Icon name="graph" size="sm" /> View components
            </button>
            <span className="dw-pill dw-pill--accent dw-pill--dot">MIT license</span>
          </div>
        </div>
        <HeroDeck />
      </div>

      <div className="dw-install">
        <span className="dw-install__prompt">$</span>
        <span className="dw-install__cmd">npm install decius.css</span>
        <button className="dw-install__copy"><Icon name="copy" size="sm" /> Copy</button>
      </div>
    </section>
  );
}

function SectionWhy() {
  const features = [
    { icon: 'cpu',    title: 'Zero runtime',     desc: 'A single CSS file and one SVG sprite. No tokens compile step, no design-system tax. Drop the link tag in and ship.' },
    { icon: 'palette', title: 'Tokenized to the wall',  desc: 'Every color, size, radius, shadow and motion value lives in a CSS custom property. Re-theme by overriding seven variables.' },
    { icon: 'magnet', title: 'Pixel-snapped',    desc: 'Lines, bevels, and 1.25px icon strokes line up at 100% zoom. Built for Sundays-on-a-23-inch-screen.' },
    { icon: 'rocket', title: 'Built for pros',   desc: 'Combo number fields, knobs, bipolar sliders, dockable panels. The vocabulary your power users already speak.' },
    { icon: 'globe',  title: 'Framework-agnostic', desc: 'Plain CSS classes. Use it with React, Vue, Svelte, htmx, vanilla JS, Electron, Tauri. Same markup, same shadows.' },
    { icon: 'eye',    title: 'Designed in pairs', desc: 'A light docs-web stylesheet ships alongside, so your marketing site and your app stay siblings, not strangers.' },
  ];
  return (
    <section className="dw-section" id="why">
      <div className="dw-section__eyebrow">Why decius</div>
      <h2>Built for the interfaces nobody else dresses up.</h2>
      <p className="dw-section__lead">
        Bootstrap is for websites. Material is for apps. Decius is for the dense, instrument-panel
        UIs that sit between you and a render — modelers, sequencers, samplers, mixers, editors,
        DAWs, color-graders, terminals.
      </p>
      <div className="dw-feature-grid">
        {features.map(f => (
          <div key={f.title} className="dw-feature">
            <div className="dw-feature__icon"><Icon name={f.icon} /></div>
            <h4>{f.title}</h4>
            <p>{f.desc}</p>
          </div>
        ))}
      </div>
    </section>
  );
}

function SectionInstall() {
  return (
    <section className="dw-section" id="install">
      <div className="dw-section__eyebrow">Getting started</div>
      <h2>Install</h2>
      <p className="dw-section__lead">
        Two files. One stylesheet, one icon sprite. Add the <code>.dcs</code> class to the root of
        anything you want styled — the framework is fully scoped, so it won't leak into the rest of
        your page.
      </p>

      <div className="dw-subhead"><h3>1. Include the stylesheet</h3><div className="dw-subhead__meta">~28 kB gzipped</div></div>
      <CodeBlock lang="html" code={`<link rel="stylesheet" href="https://cdn.decius.css/0.4/decius.css">
<link rel="preload" as="image" href="https://cdn.decius.css/0.4/icons.svg">`} />

      <div className="dw-subhead"><h3>2. Load icons once</h3><div className="dw-subhead__meta">A single fetch</div></div>
      <CodeBlock lang="js" code={`// inject the sprite into the page so <use href="#di-…"> resolves
fetch('https://cdn.decius.css/0.4/icons.svg')
  .then(r => r.text())
  .then(svg => document.body.insertAdjacentHTML('afterbegin', svg));`} />

      <div className="dw-subhead"><h3>3. Wrap your app</h3><div className="dw-subhead__meta">.dcs scopes the system</div></div>
      <CodeBlock lang="html" code={`<div class="dcs" data-dcs-density="comfortable">
  <button class="dcs-btn dcs-btn--primary">
    <svg class="dcs-icon"><use href="#di-render"/></svg>
    Render
  </button>
</div>`} />

      <div style={{ marginTop: 28 }}>
        <div className="dcs-alert" style={{ background: 'var(--dw-bg-soft)', borderColor: 'var(--dw-line)', borderLeftColor: 'var(--dw-accent)', color: 'var(--dw-text)' }}>
          <div className="dcs-alert__icon"><Icon name="info" /></div>
          <div className="dcs-alert__body">
            <div className="dcs-alert__title" style={{ color: 'var(--dw-text)' }}>This page is decius.web</div>
            <div className="dcs-alert__msg" style={{ color: 'var(--dw-text-dim)' }}>
              The white chrome around the embedded panels is <code>decius-web.css</code>, a companion stylesheet
              meant for your marketing site, docs, and changelogs. The dark panels embedded throughout are
              <code>decius.css</code> proper — siblings, sharing the same accent, type, and icon family.
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}

Object.assign(window, { SectionHero, SectionWhy, SectionInstall, HeroDeck });
