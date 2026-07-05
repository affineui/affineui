/* sections-foundations.jsx
   Colors · Type · Spacing · Icons
*/
const { useState: useStateF } = React;

function Demo({ children, caption, checker, inset, defaultStyle = 'flat', defaultDensity, noDensity, frame = 'panel' }) {
  const [mode, setMode] = useStateF(defaultStyle);
  const [density, setDensity] = useStateF(defaultDensity);
  // When density/style is unset, let the global Tweaks cascade through.
  // Only emit the attribute when the user has actively chosen a value here.
  const demoAttrs = {
    'data-dcs-demo': '',
  };
  if (density) demoAttrs['data-dcs-density'] = density;
  if (mode === '3d') demoAttrs['data-dcs-style'] = '3d';
  return (
    <div style={{ marginBottom: 32 }}>
      <div className={`dw-demo dw-demo--${frame}${checker ? ' dw-demo--checker' : ''}${inset ? ' dw-demo--inset' : ''}`}>
        <div className="dcs" {...demoAttrs}>{children}</div>
      </div>
      <div className="dw-demo__caption" style={{ flexWrap: 'wrap' }}>
        <DemoToggle label="Style" value={mode} onChange={setMode} options={[
          { value: 'flat', label: 'Flat' },
          { value: '3d', label: 'Synth' },
        ]} />
        {!noDensity && (
          <DemoToggle label="Density" value={density || ''} onChange={setDensity} options={[
            { value: '', label: 'auto' },
            { value: 'compact', label: 'sm' },
            { value: 'comfortable', label: 'md' },
            { value: 'spacious', label: 'lg' },
          ]} />
        )}
        {caption && <span style={{ marginLeft: 4 }}>// {caption}</span>}
      </div>
    </div>
  );
}

function DemoToggle({ label, value, onChange, options }) {
  return (
    <div style={{ display: 'inline-flex', alignItems: 'center', gap: 8, padding: '2px 4px 2px 8px', background: 'var(--dw-bg-soft)', border: '1px solid var(--dw-line)', borderRadius: 999 }}>
      <span style={{ fontSize: 11, color: 'var(--dw-text-mute)', textTransform: 'uppercase', letterSpacing: '.06em', fontFamily: 'var(--dw-font-mono)' }}>{label}</span>
      <div style={{ display: 'inline-flex', background: 'var(--dw-bg)', borderRadius: 999, padding: 2, gap: 1, border: '1px solid var(--dw-line)' }}>
        {options.map(opt => (
          <button key={opt.value} onClick={() => onChange(opt.value)} style={{
            border: 'none', background: value === opt.value ? '#14161c' : 'transparent',
            color: value === opt.value ? '#fff' : 'var(--dw-text-dim)',
            fontFamily: 'var(--dw-font-mono)', fontSize: 11, padding: '3px 10px',
            borderRadius: 999, cursor: 'pointer',
          }}>{opt.label}</button>
        ))}
      </div>
    </div>
  );
}

function CodeBlock({ lang, code }) {
  const html = highlight(code, lang || 'css');
  return (
    <pre>
      <code dangerouslySetInnerHTML={{ __html: html }} />
    </pre>
  );
}

// Tiny regex-based highlighter, just for visual rhythm
function highlight(code, lang) {
  let s = code
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
  if (lang === 'html') {
    s = s.replace(/(&lt;\/?)([\w-]+)/g, '$1<span class="tk-t">$2</span>')
         .replace(/\s([\w-]+)=&quot;([^&]*?)&quot;/g, ' <span class="tk-k">$1</span>=<span class="tk-s">&quot;$2&quot;</span>');
  } else if (lang === 'css') {
    s = s.replace(/(\/\*[\s\S]*?\*\/)/g, '<span class="tk-c">$1</span>')
         .replace(/(--[\w-]+)/g, '<span class="tk-v">$1</span>')
         .replace(/([\.\#][\w-]+)/g, '<span class="tk-t">$1</span>')
         .replace(/(#[0-9a-f]{3,8})\b/gi, '<span class="tk-s">$1</span>')
         .replace(/\b(\d+(\.\d+)?(px|rem|em|%|deg|s|ms)?)\b/g, '<span class="tk-n">$1</span>');
  } else if (lang === 'js' || lang === 'jsx') {
    s = s.replace(/(\/\/[^\n]*)/g, '<span class="tk-c">$1</span>')
         .replace(/\b(const|let|var|function|return|if|else|for|import|from|export|new|class)\b/g, '<span class="tk-k">$1</span>')
         .replace(/&quot;([^&]*?)&quot;/g, '<span class="tk-s">&quot;$1&quot;</span>')
         .replace(/\b(\d+(\.\d+)?)\b/g, '<span class="tk-n">$1</span>');
  }
  return s;
}

/* ─────────── Colors ─────────── */
function SectionColors() {
  const palette = [
    { name: '--dcs-bg-app',     v: '#1f222a', note: 'Window / app shell' },
    { name: '--dcs-bg',         v: '#2a2e38', note: 'Default panel surface' },
    { name: '--dcs-surface-1',  v: '#323744', note: 'Raised' },
    { name: '--dcs-surface-2',  v: '#3c424f', note: 'Hover · raised x2' },
    { name: '--dcs-surface-3',  v: '#474e5d', note: 'Active · pressed' },
    { name: '--dcs-surface-4',  v: '#555d6e', note: 'Inset · cap' },
    { name: '--dcs-well',       v: '#20232b', note: 'Sunken inputs' },
    { name: '--dcs-line',       v: '#14161c', note: 'Hard separator' },
  ];
  const accents = [
    { name: '--dcs-accent',    v: '#4d9fff', note: 'DCC blue · key signal' },
    { name: '--dcs-accent-hi', v: '#6fb3ff', note: 'Hover' },
    { name: '--dcs-accent-lo', v: '#2f86ee', note: 'Active' },
    { name: '--dcs-ok',        v: '#4ed18a', note: 'Success · armed' },
    { name: '--dcs-warn',      v: '#f2b14a', note: 'Warning · clipping' },
    { name: '--dcs-danger',    v: '#ef6b6b', note: 'Error · destructive' },
    { name: '--dcs-purple',    v: '#b48cff', note: 'Aux · MIDI' },
    { name: '--dcs-teal',      v: '#4ad5d5', note: 'Aux · channel' },
  ];
  const Swatchy = ({ name, v, note, light }) => (
    <div className={`palette-swatch${light ? ' palette-swatch--light' : ''}`}>
      <div className="palette-swatch__chip" style={{ background: v }} />
      <div className="palette-swatch__meta">
        <div className="palette-swatch__hex">{v}</div>
        <div className="palette-swatch__name">{name}</div>
        <div className="palette-swatch__note">{note}</div>
      </div>
    </div>
  );
  return (
    <section className="dw-section" id="colors">
      <div className="dw-section__eyebrow">Foundations · 01</div>
      <h2>Color</h2>
      <p className="dw-section__lead">
        A grayscale rail with a single bright signal color. The grays bias slightly cool to keep the
        interface feeling crisp under long sessions; the accent is the only saturated color you ever
        see in chrome — destinations earn their visibility.
      </p>

      <div className="dw-subhead"><h3>Surfaces</h3><div className="dw-subhead__meta">8 tokens · WCAG-safe pairs</div></div>
      <div className="palette-grid">
        {palette.map(p => <Swatchy key={p.name} {...p} />)}
      </div>

      <div className="dw-subhead"><h3>Accent &amp; semantic</h3><div className="dw-subhead__meta">Use sparingly</div></div>
      <div className="palette-grid">
        {accents.map(p => <Swatchy key={p.name} {...p} light />)}
      </div>

      <div className="dw-subhead"><h3>In context</h3><div className="dw-subhead__meta">Compose, don't decorate</div></div>
      <Demo caption="Buttons inherit token relationships — no per-component overrides needed.">
        <div className="dcs-row" style={{ flexWrap: 'wrap', gap: 8 }}>
          <Button primary iconLeft="play">Render</Button>
          <Button iconLeft="save">Save</Button>
          <Button ghost iconLeft="cog">Settings</Button>
          <Button danger iconLeft="trash">Delete</Button>
          <Button pressed iconLeft="magnet">Snap</Button>
        </div>
      </Demo>
    </section>
  );
}

/* ─────────── Type ─────────── */
function SectionType() {
  return (
    <section className="dw-section" id="type">
      <div className="dw-section__eyebrow">Foundations · 02</div>
      <h2>Type</h2>
      <p className="dw-section__lead">
        IBM Plex Sans for chrome and prose; JetBrains Mono for numerics, codes, addresses,
        coordinates, and anything users will want to read at a glance and trust.
      </p>

      <div className="dw-subhead"><h3>Scale</h3><div className="dw-subhead__meta">Density-aware</div></div>
      <Demo>
        <div className="type-spec">
          <div className="type-row"><div className="type-row__label dcs-mono">3xl · 32 / 1.15</div><div style={{ fontSize: 32, fontWeight: 600, letterSpacing: '-.02em' }}>Decius shapes the cursor.</div></div>
          <div className="type-row"><div className="type-row__label dcs-mono">2xl · 24 / 1.2</div><div style={{ fontSize: 24, fontWeight: 600, letterSpacing: '-.015em' }}>Panel titles &amp; modal headers</div></div>
          <div className="type-row"><div className="type-row__label dcs-mono">xl  · 18 / 1.3</div><div style={{ fontSize: 18, fontWeight: 500 }}>Section headlines</div></div>
          <div className="type-row"><div className="type-row__label dcs-mono">lg  · 15 / 1.45</div><div style={{ fontSize: 15 }}>Body text in the docs site</div></div>
          <div className="type-row"><div className="type-row__label dcs-mono">md  · 13 / 1.45</div><div style={{ fontSize: 13 }}>Default control text in DCC</div></div>
          <div className="type-row"><div className="type-row__label dcs-mono">sm  · 11 / 1.4</div><div style={{ fontSize: 11 }}>Labels, tab text, hints</div></div>
          <div className="type-row"><div className="type-row__label dcs-mono">xs  · 10 / 1.3</div><div style={{ fontSize: 10, textTransform: 'uppercase', letterSpacing: '.06em', color: 'var(--dcs-text-mute)' }}>EYEBROWS · STATUS · METRICS</div></div>
        </div>
      </Demo>

      <div className="dw-subhead"><h3>Numerics</h3><div className="dw-subhead__meta">tabular-nums, .04em</div></div>
      <Demo caption="Numbers are mono-spaced, tabular, lining figures — values can be compared by column.">
        <div className="dcs-mono" style={{ fontSize: 18, color: 'var(--dcs-text)', letterSpacing: '.02em' }}>
          <div>0.000  ·  +1.428  ·  −0.952  ·  ∞</div>
          <div style={{ marginTop: 8, color: 'var(--dcs-accent)' }}>440.00 Hz  ·  −18.2 dB  ·  120.00 BPM</div>
          <div style={{ marginTop: 8, color: 'var(--dcs-text-dim)' }}>X 1.0000  Y −0.4321  Z  3.1415</div>
        </div>
      </Demo>
    </section>
  );
}

/* ─────────── Spacing / Density ─────────── */
function SectionSpacing() {
  const [d, setD] = useStateF('comfortable');
  return (
    <section className="dw-section" id="spacing">
      <div className="dw-section__eyebrow">Foundations · 03</div>
      <h2>Spacing &amp; density</h2>
      <p className="dw-section__lead">
        A 4-pixel grid with three density modes. Compact for power users on large monitors;
        comfortable as the default; spacious for trackpad navigation and presentation.
      </p>
      <Demo caption="Switch modes — every control re-fits without changing its visual relationships.">
        <div className="dcs-row" style={{ marginBottom: 16 }}>
          <ButtonGroup value={d} onChange={setD} options={[
            { value: 'compact', label: 'Compact' },
            { value: 'comfortable', label: 'Comfortable' },
            { value: 'spacious', label: 'Spacious' },
          ]} />
        </div>
        <div className="dcs" data-dcs-density={d} style={{ background: 'transparent' }}>
          <div className="dcs-row" style={{ flexWrap: 'wrap', gap: 'var(--dcs-s-3)' }}>
            <Button iconLeft="cube">Mesh</Button>
            <Button iconLeft="light">Light</Button>
            <Button iconLeft="camera">Camera</Button>
            <Button primary iconLeft="render">Render</Button>
            <div className="dcs-divider--v" />
            <Check checked onChange={() => {}}>Auto-key</Check>
            <Switch checked onChange={() => {}} />
          </div>
        </div>
      </Demo>
    </section>
  );
}

/* ─────────── Icons catalog ─────────── */
function SectionIcons() {
  const [q, setQ] = useStateF('');
  const lower = q.toLowerCase();
  return (
    <section className="dw-section" id="icons">
      <div className="dw-section__eyebrow">Foundations · 04</div>
      <h2>Icons</h2>
      <p className="dw-section__lead">
        93 icons drawn at 24px on a 1.25px stroke. One sprite, one
        <code>&lt;use&gt;</code> per call. Inherit color from <code>currentColor</code> so they
        work the same in chrome, signals, and tinted states.
      </p>

      <div className="dw-row" style={{ display: 'flex', gap: 12, marginBottom: 20, alignItems: 'center' }}>
        <div className="dw-search" style={{ width: 320 }}>
          <Icon name="search" />
          <input
            value={q}
            onChange={e => setQ(e.target.value)}
            placeholder="Filter icons by name…"
            style={{ flex: 1, border: 'none', outline: 'none', background: 'transparent', color: 'inherit', fontSize: 13 }}
          />
        </div>
        <div className="dw-meta">{ICON_CATALOG.reduce((n, g) => n + g.icons.length, 0)} total</div>
      </div>

      <div className="icon-catalog">
        {ICON_CATALOG.map(group => {
          const filtered = group.icons.filter(i => !lower || i.includes(lower));
          if (!filtered.length) return null;
          return (
            <div key={group.group} className="icon-catalog__group">
              <div className="icon-catalog__group-name">{group.group}</div>
              <div className="icon-catalog__grid">
                {filtered.map(name => (
                  <div key={name} className="icon-cell" title={name}>
                    <div className="icon-cell__icon"><Icon name={name} size="lg" /></div>
                    <div className="icon-cell__name">{name}</div>
                  </div>
                ))}
              </div>
            </div>
          );
        })}
      </div>
    </section>
  );
}

Object.assign(window, {
  Demo, CodeBlock,
  SectionColors, SectionType, SectionSpacing, SectionIcons,
});
