/* devtools.jsx — "affinetools" debug UI for AffineUI / decius.css
   Four panels: Elements · Sources · Performance · Memory.
   Scoped to what a GPU UI/rendering engine needs — no network, no console.
*/
const { useState: useDT, useEffect: useDTE, useRef: useDTR, useMemo: useDTM } = React;

/* ============================================================
   ELEMENTS — DOM tree + computed styles + box model
   ============================================================ */
const DOM_TREE = {
  tag: 'root', id: 'app', cls: 'dcs', kids: [
    { tag: 'div', cls: 'dcs-menubar', kids: [
      { tag: 'div', cls: 'dcs-menubar__brand', text: 'modeler' },
      { tag: 'button', cls: 'dcs-menubar__item', text: 'File' },
      { tag: 'button', cls: 'dcs-menubar__item', text: 'Edit' },
    ]},
    { tag: 'div', cls: 'dcs-dock', kids: [
      { tag: 'div', cls: 'dcs-dockpane', kids: [
        { tag: 'div', cls: 'dcs-dockpane__tabs', kids: [
          { tag: 'div', cls: 'dcs-dockpane__tab', attrs: { 'aria-selected': 'true' }, text: 'Inspector' },
        ]},
        { tag: 'div', cls: 'dcs-foldouts', sel: true, kids: [
          { tag: 'div', cls: 'dcs-foldout', kids: [
            { tag: 'div', cls: 'dcs-foldout__header', text: 'Transform' },
            { tag: 'div', cls: 'dcs-foldout__body', kids: [
              { tag: 'div', cls: 'dcs-combo', text: '1.428' },
            ]},
          ]},
        ]},
      ]},
    ]},
  ]
};

function domFlatten(node, depth, path, out, expanded) {
  const id = path;
  out.push({ node, depth, id });
  if (node.kids && expanded.has(id)) {
    node.kids.forEach((k, i) => domFlatten(k, depth + 1, `${path}.${i}`, out, expanded));
  }
  return out;
}

function ElementsPanel() {
  const [expanded, setExpanded] = useDT(new Set(['0', '0.1', '0.1.0', '0.1.0.1']));
  const [selected, setSelected] = useDT('0.1.0.1');
  const rows = domFlatten(DOM_TREE, 0, '0', [], expanded);
  const toggle = (id) => setExpanded(p => { const n = new Set(p); n.has(id) ? n.delete(id) : n.add(id); return n; });

  return (
    <div style={{ display: 'flex', height: '100%', minHeight: 0 }}>
      {/* DOM tree */}
      <div style={{ flex: 1, minWidth: 0, overflow: 'auto', borderRight: '1px solid var(--dcs-line)' }}>
        <div style={{ fontFamily: 'var(--dcs-font-mono)', fontSize: 12, padding: '6px 0' }}>
          {rows.map(({ node, depth, id }) => {
            const hasKids = node.kids && node.kids.length;
            const isOpen = expanded.has(id);
            const isSel = selected === id;
            return (
              <div key={id}
                   onClick={() => setSelected(id)}
                   style={{
                     display: 'flex', alignItems: 'center', gap: 4,
                     paddingLeft: 8 + depth * 14, paddingRight: 8,
                     height: 20, cursor: 'default', whiteSpace: 'nowrap',
                     background: isSel ? 'var(--dcs-accent-dim)' : 'transparent',
                     boxShadow: isSel ? 'inset 2px 0 0 var(--dcs-accent)' : 'none',
                   }}>
                <span onClick={(e) => { e.stopPropagation(); if (hasKids) toggle(id); }}
                      style={{ width: 12, display: 'inline-flex', color: 'var(--dcs-text-mute)', cursor: hasKids ? 'pointer' : 'default' }}>
                  {hasKids && <Icon name="chevron-right" size="sm" style={{ transform: isOpen ? 'rotate(90deg)' : 'none', transition: 'transform .1s' }} />}
                </span>
                <span style={{ color: 'var(--dcs-pink)' }}>&lt;{node.tag}</span>
                {node.id && <span><span style={{ color: 'var(--dcs-warn)' }}> id</span><span style={{ color: 'var(--dcs-text-mute)' }}>=</span><span style={{ color: 'var(--dcs-ok)' }}>"{node.id}"</span></span>}
                {node.cls && <span><span style={{ color: 'var(--dcs-warn)' }}> class</span><span style={{ color: 'var(--dcs-text-mute)' }}>=</span><span style={{ color: 'var(--dcs-ok)' }}>"{node.cls}"</span></span>}
                <span style={{ color: 'var(--dcs-pink)' }}>&gt;</span>
                {node.text && <span style={{ color: 'var(--dcs-text-dim)' }}>{node.text}</span>}
                {!isOpen && hasKids && <span style={{ color: 'var(--dcs-text-mute)' }}>…</span>}
              </div>
            );
          })}
        </div>
      </div>

      {/* Styles side panel */}
      <div style={{ width: 320, flex: '0 0 320px', overflow: 'auto', background: 'var(--dcs-bg)' }}>
        <Tabs value="styles" onChange={() => {}} tabs={[
          { value: 'styles', label: 'Styles' },
          { value: 'computed', label: 'Computed' },
          { value: 'layout', label: 'Layout' },
        ]} />
        <div style={{ padding: 12 }}>
          {/* Box model diagram */}
          <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 8 }}>Box model</div>
          <BoxModel />

          {/* Rules */}
          <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', margin: '16px 0 8px' }}>Matched rules</div>
          <div style={{ fontFamily: 'var(--dcs-font-mono)', fontSize: 11.5, lineHeight: 1.6 }}>
            <StyleRule selector=".dcs-combo" file="decius.css:412" decls={[
              ['display', 'inline-flex'],
              ['height', 'var(--dcs-h-in)'],
              ['background', 'var(--dcs-well)'],
              ['border', '1px solid var(--dcs-line)'],
              ['border-radius', 'var(--dcs-r-2)'],
            ]} />
            <StyleRule selector=".dcs-props > .dcs-field" file="decius.css:198" decls={[
              ['height', 'var(--dcs-h-in)'],
              ['justify-content', 'space-between'],
            ]} />
            <StyleRule selector=".dcs" file="decius.css:180" inherited decls={[
              ['font-family', 'var(--dcs-font)'],
              ['color', 'var(--dcs-text)'],
            ]} />
          </div>
        </div>
      </div>
    </div>
  );
}

function StyleRule({ selector, file, decls, inherited }) {
  return (
    <div style={{ marginBottom: 10, opacity: inherited ? 0.75 : 1 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between' }}>
        <span style={{ color: 'var(--dcs-purple)' }}>{selector} <span style={{ color: 'var(--dcs-text-mute)' }}>{'{'}</span></span>
        <span style={{ color: 'var(--dcs-text-mute)', fontSize: 10 }}>{file}</span>
      </div>
      {decls.map(([k, v]) => (
        <div key={k} style={{ paddingLeft: 14 }}>
          <span style={{ color: 'var(--dcs-accent)' }}>{k}</span>
          <span style={{ color: 'var(--dcs-text-mute)' }}>: </span>
          <span style={{ color: 'var(--dcs-text)' }}>{v}</span>
          <span style={{ color: 'var(--dcs-text-mute)' }}>;</span>
        </div>
      ))}
      <div style={{ color: 'var(--dcs-text-mute)' }}>{'}'}</div>
    </div>
  );
}

function BoxModel() {
  const L = (v, top) => (
    <span style={{ fontSize: 9, color: 'var(--dcs-text-mute)', fontFamily: 'var(--dcs-font-mono)' }}>{v}</span>
  );
  const box = (label, color, pad, children) => (
    <div style={{ background: color, border: '1px solid var(--dcs-line)', padding: pad, position: 'relative', textAlign: 'center' }}>
      <span style={{ position: 'absolute', top: 1, left: 4, fontSize: 8, color: 'var(--dcs-text-mute)', letterSpacing: '.08em' }}>{label}</span>
      {children}
    </div>
  );
  return (
    <div style={{ fontFamily: 'var(--dcs-font-mono)' }}>
      {box('margin', 'rgba(242,177,74,.12)', '18px 24px',
        box('border', 'rgba(180,140,255,.14)', '14px 20px',
          box('padding', 'rgba(78,209,138,.12)', '14px 20px',
            <div style={{ background: 'rgba(77,159,255,.18)', border: '1px solid var(--dcs-accent)', padding: '10px 4px', color: 'var(--dcs-text)', fontSize: 11 }}>
              120 × 22
            </div>
          )
        )
      )}
      <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: 8, fontSize: 10, color: 'var(--dcs-text-dim)' }}>
        <span>padding 0 6</span><span>border 1</span><span>margin 0</span>
      </div>
    </div>
  );
}

/* ============================================================
   SOURCES — file tree + source viewer (HTML/CSS)
   ============================================================ */
const SOURCE_FILES = {
  'inspector.html': {
    lang: 'html',
    lines: [
      '<div class="dcs" data-dcs-density="compact">',
      '  <div class="dcs-menubar">',
      '    <div class="dcs-menubar__brand">modeler</div>',
      '    <button class="dcs-menubar__item">File</button>',
      '  </div>',
      '  <div class="dcs-dock">',
      '    <div class="dcs-dockpane" id="inspector">',
      '      <div class="dcs-foldouts">',
      '        <!-- transform properties -->',
      '        <div class="dcs-foldout">…</div>',
      '      </div>',
      '    </div>',
      '  </div>',
      '</div>',
    ],
    bp: [3, 9],
  },
  'theme.css': {
    lang: 'css',
    lines: [
      ':root {',
      '  --dcs-bg: #2a2e38;',
      '  --dcs-accent: #4d9fff;',
      '  --dcs-h-in: 22px;',
      '}',
      '.dcs-combo {',
      '  height: var(--dcs-h-in);',
      '  background: var(--dcs-well);',
      '  border-radius: var(--dcs-r-2);',
      '}',
    ],
    bp: [],
  },
};

function SourcesPanel() {
  const [file, setFile] = useDT('inspector.html');
  const [expanded] = useDT(new Set(['ui', 'styles']));
  const cur = SOURCE_FILES[file];

  const tree = [
    { id: 'ui', label: 'ui/', icon: 'folder-open', depth: 0 },
    { id: 'inspector.html', label: 'inspector.html', icon: 'file', depth: 1 },
    { id: 'toolbar.html', label: 'toolbar.html', icon: 'file', depth: 1 },
    { id: 'styles', label: 'styles/', icon: 'folder-open', depth: 0 },
    { id: 'theme.css', label: 'theme.css', icon: 'file', depth: 1 },
    { id: 'layout.css', label: 'layout.css', icon: 'file', depth: 1 },
  ];

  return (
    <div style={{ display: 'flex', height: '100%', minHeight: 0 }}>
      {/* File tree */}
      <div style={{ width: 200, flex: '0 0 200px', borderRight: '1px solid var(--dcs-line)', overflow: 'auto', background: 'var(--dcs-bg)' }}>
        <div style={{ padding: '6px 0' }}>
          {tree.map(t => {
            const isFile = SOURCE_FILES[t.id];
            const isSel = file === t.id;
            return (
              <div key={t.id}
                   onClick={() => isFile && setFile(t.id)}
                   style={{
                     display: 'flex', alignItems: 'center', gap: 6,
                     height: 22, paddingLeft: 10 + t.depth * 14, paddingRight: 8,
                     fontSize: 12, cursor: isFile ? 'pointer' : 'default',
                     color: isSel ? 'var(--dcs-text)' : 'var(--dcs-text-dim)',
                     background: isSel ? 'var(--dcs-accent-dim)' : 'transparent',
                     boxShadow: isSel ? 'inset 2px 0 0 var(--dcs-accent)' : 'none',
                   }}>
                <Icon name={t.icon} size="sm" style={{ color: isSel ? 'var(--dcs-accent)' : 'var(--dcs-text-mute)' }} />
                <span style={{ fontFamily: 'var(--dcs-font-mono)' }}>{t.label}</span>
              </div>
            );
          })}
        </div>
      </div>

      {/* Code viewer */}
      <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ display: 'flex', alignItems: 'center', height: 26, borderBottom: '1px solid var(--dcs-line)', background: 'var(--dcs-surface-1)', padding: '0 10px', gap: 8 }}>
          <span style={{ fontFamily: 'var(--dcs-font-mono)', fontSize: 12, color: 'var(--dcs-text)' }}>{file}</span>
          <span className="dcs-badge dcs-badge--soft">{cur.lang}</span>
          <span style={{ flex: 1 }} />
          <span style={{ fontSize: 11, color: 'var(--dcs-text-mute)', fontFamily: 'var(--dcs-font-mono)' }}>{cur.lines.length} lines</span>
        </div>
        <div style={{ flex: 1, overflow: 'auto', fontFamily: 'var(--dcs-font-mono)', fontSize: 12, lineHeight: 1.6, background: 'var(--dcs-well)' }}>
          {cur.lines.map((ln, i) => {
            const hasBp = cur.bp.includes(i);
            return (
              <div key={i} style={{ display: 'flex', minHeight: 20 }}>
                <div style={{
                  width: 44, flex: '0 0 44px', textAlign: 'right', paddingRight: 8,
                  color: hasBp ? '#0a1220' : 'var(--dcs-text-mute)',
                  background: hasBp ? 'var(--dcs-accent)' : 'transparent',
                  userSelect: 'none', position: 'relative',
                }}>{i + 1}</div>
                <div style={{ paddingLeft: 10, whiteSpace: 'pre', color: 'var(--dcs-text-dim)' }}
                     dangerouslySetInnerHTML={{ __html: hl(ln, cur.lang) }} />
              </div>
            );
          })}
        </div>
        {/* Breakpoints strip (layout invalidation points) */}
        <div style={{ height: 24, borderTop: '1px solid var(--dcs-line)', background: 'var(--dcs-surface-1)', display: 'flex', alignItems: 'center', padding: '0 10px', gap: 8, fontSize: 11, color: 'var(--dcs-text-mute)', fontFamily: 'var(--dcs-font-mono)' }}>
          <Icon name="cross-target" size="sm" style={{ color: 'var(--dcs-accent)' }} />
          <span>{cur.bp.length} layout watch{cur.bp.length === 1 ? '' : 'es'}</span>
        </div>
      </div>
    </div>
  );
}

function hl(s, lang) {
  let e = s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  if (lang === 'html') {
    e = e.replace(/(&lt;!--.*?--&gt;)/g, '<span style="color:#767c8a">$1</span>')
         .replace(/(&lt;\/?)([\w-]+)/g, '$1<span style="color:#ff7ab8">$2</span>')
         .replace(/([\w-]+)=(&quot;|")([^"&]*)\2/g, '<span style="color:#f2b14a">$1</span>=<span style="color:#4ed18a">"$3"</span>');
  } else {
    e = e.replace(/(--[\w-]+)/g, '<span style="color:#4d9fff">$1</span>')
         .replace(/([.#][\w-]+)(?=\s*\{)/g, '<span style="color:#b48cff">$1</span>')
         .replace(/(#[0-9a-f]{3,8})\b/gi, '<span style="color:#4ed18a">$1</span>')
         .replace(/\b(\d+(?:px|%|rem|em)?)\b/g, '<span style="color:#f2b14a">$1</span>');
  }
  return e;
}

/* ============================================================
   PERFORMANCE — multi-channel timeline + probe flame + event log
   ============================================================ */
const PERF_CHANNELS = [
  { key: 'cpu',    label: 'CPU',    color: '#4d9fff', base: 3.5, jitter: 2.5 },
  { key: 'layout', label: 'Layout', color: '#b48cff', base: 1.6, jitter: 1.4 },
  { key: 'paint',  label: 'Paint',  color: '#4ed18a', base: 1.9, jitter: 1.2 },
  { key: 'gpu',    label: 'GPU',    color: '#f2b14a', base: 1.2, jitter: 0.9 },
];

// Named probes the app inserts via AffineUI's perf API — surfaced as tags
const PROBE_EVENTS = [
  { t: '16:42:08.204', tag: 'scene.load',        ms: 12.4, ch: 'cpu',    tone: 'warn' },
  { t: '16:42:08.216', tag: 'inspector.rebuild', ms: 3.1,  ch: 'layout', tone: '' },
  { t: '16:42:08.219', tag: 'combo.drag',        ms: 0.8,  ch: 'cpu',    tone: '' },
  { t: '16:42:08.231', tag: 'foldout.toggle',    ms: 2.2,  ch: 'layout', tone: '' },
  { t: '16:42:08.244', tag: 'viewport.repaint',  ms: 4.6,  ch: 'paint',  tone: '' },
  { t: '16:42:08.259', tag: 'gpu.submit',        ms: 1.2,  ch: 'gpu',    tone: '' },
  { t: '16:42:08.271', tag: 'atlas.upload',      ms: 5.9,  ch: 'gpu',    tone: 'warn' },
];

function PerformancePanel() {
  const [series, setSeries] = useDT(() =>
    Object.fromEntries(PERF_CHANNELS.map(c => [c.key,
      Array.from({ length: 60 }, () => c.base + Math.random() * c.jitter)]))
  );
  const [log, setLog] = useDT(() => PROBE_EVENTS.slice());
  const [recording, setRecording] = useDT(true);
  const [selCh, setSelCh] = useDT(Object.fromEntries(PERF_CHANNELS.map(c => [c.key, true])));
  const logRef = useDTR(null);

  useDTE(() => {
    if (!recording) return;
    let tick = 0;
    const id = setInterval(() => {
      setSeries(s => {
        const next = { ...s };
        PERF_CHANNELS.forEach(c => {
          const spike = Math.random() > 0.93;
          const v = c.base + Math.random() * c.jitter + (spike ? 4 + Math.random() * 6 : 0);
          next[c.key] = [...s[c.key].slice(1), v];
        });
        return next;
      });
      // Occasionally emit a probe event to the breadcrumb log
      if (Math.random() > 0.6) {
        const probes = ['input.dispatch', 'style.invalidate', 'layout.reflow', 'text.shape', 'raf.commit', 'hit.test', 'scroll.update'];
        const chs = ['cpu', 'layout', 'paint', 'gpu'];
        const ms = +(Math.random() * 7).toFixed(1);
        const now = new Date();
        const t = `16:42:${String(9 + (tick % 50)).padStart(2, '0')}.${String(Math.floor(Math.random() * 999)).padStart(3, '0')}`;
        setLog(l => [...l.slice(-40), {
          t, tag: probes[Math.floor(Math.random() * probes.length)],
          ms, ch: chs[Math.floor(Math.random() * chs.length)],
          tone: ms > 5 ? 'warn' : '',
        }]);
      }
      tick++;
    }, 150);
    return () => clearInterval(id);
  }, [recording]);

  useDTE(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [log]);

  const budget = 16.67;
  const maxT = 26;
  // Total per-frame = sum of channels
  const N = series.cpu.length;
  const totals = Array.from({ length: N }, (_, i) =>
    PERF_CHANNELS.reduce((a, c) => a + (selCh[c.key] ? series[c.key][i] : 0), 0));
  const avg = totals.reduce((a, b) => a + b, 0) / N;
  const p95 = [...totals].sort((a, b) => a - b)[Math.floor(N * 0.95)];

  const chMeta = (k) => PERF_CHANNELS.find(c => c.key === k);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', minHeight: 0 }}>
      {/* Toolbar */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '8px 12px', borderBottom: '1px solid var(--dcs-line)' }}>
        <Button sm iconLeft={recording ? 'pause' : 'record'} primary={recording} onClick={() => setRecording(r => !r)}>
          {recording ? 'Recording' : 'Paused'}
        </Button>
        <Button sm ghost iconLeft="trash" onClick={() => { setSeries(Object.fromEntries(PERF_CHANNELS.map(c => [c.key, Array.from({ length: 60 }, () => c.base)]))); setLog([]); }}>Clear</Button>
        <div style={{ width: 1, height: 18, background: 'var(--dcs-line)' }} />
        {/* Channel toggles */}
        <div style={{ display: 'flex', gap: 4 }}>
          {PERF_CHANNELS.map(c => (
            <button key={c.key} onClick={() => setSelCh(s => ({ ...s, [c.key]: !s[c.key] }))}
                    style={{
                      display: 'inline-flex', alignItems: 'center', gap: 5,
                      padding: '2px 8px', borderRadius: 999, cursor: 'pointer',
                      border: '1px solid var(--dcs-line)',
                      background: selCh[c.key] ? 'var(--dcs-surface-2)' : 'transparent',
                      color: selCh[c.key] ? 'var(--dcs-text)' : 'var(--dcs-text-mute)',
                      fontSize: 10.5, fontFamily: 'var(--dcs-font-mono)',
                      opacity: selCh[c.key] ? 1 : 0.5,
                    }}>
              <span style={{ width: 8, height: 8, borderRadius: 2, background: c.color }} />
              {c.label}
            </button>
          ))}
        </div>
        <div style={{ flex: 1 }} />
        <Stat label="avg" value={`${avg.toFixed(1)} ms`} />
        <Stat label="p95" value={`${p95.toFixed(1)} ms`} tone={p95 > budget ? 'warn' : 'ok'} />
        <Stat label="fps" value={`${Math.min(60, Math.round(1000 / Math.max(avg, 16.67)))}`} tone="ok" />
      </div>

      {/* Multi-channel stacked timeline */}
      <div style={{ padding: 12, borderBottom: '1px solid var(--dcs-line)' }}>
        <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 8 }}>Frame time · {N} frames · stacked channels</div>
        <div style={{ position: 'relative', height: 96, display: 'flex', alignItems: 'flex-end', gap: 2, background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3, padding: '0 4px' }}>
          <div style={{ position: 'absolute', left: 0, right: 0, bottom: `${(budget / maxT) * 100}%`, borderTop: '1px dashed var(--dcs-warn)', opacity: .6 }}>
            <span style={{ position: 'absolute', right: 4, top: -13, fontSize: 9, color: 'var(--dcs-warn)', fontFamily: 'var(--dcs-font-mono)' }}>16.7ms</span>
          </div>
          {Array.from({ length: N }).map((_, i) => {
            const frameTotal = PERF_CHANNELS.reduce((a, c) => a + (selCh[c.key] ? series[c.key][i] : 0), 0);
            return (
              <div key={i} className="dcs-tooltip" data-tip={`frame ${i - N + 1} · ${frameTotal.toFixed(1)} ms`}
                   style={{ flex: 1, minWidth: 2, height: '100%', display: 'flex', flexDirection: 'column-reverse', cursor: 'default' }}>
                {PERF_CHANNELS.filter(c => selCh[c.key]).map(c => (
                  <div key={c.key} style={{
                    height: `${Math.min(100, (series[c.key][i] / maxT) * 100)}%`,
                    background: c.color,
                    opacity: i === N - 1 ? 1 : 0.85,
                  }} />
                ))}
              </div>
            );
          })}
        </div>
      </div>

      {/* Flame graph + probe breadcrumb log side by side */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>
        {/* Flame */}
        <div style={{ flex: 1, minWidth: 0, overflow: 'auto', padding: 12, borderRight: '1px solid var(--dcs-line)' }}>
          <div className="dw-caption" style={{ color: 'var(--dcs-text-mute)', marginBottom: 8 }}>Last frame · probe stack</div>
          <div style={{ position: 'relative', minHeight: 100 }}>
            {[
              { name: 'raf.commit', t: 6.2, color: '#4d9fff', depth: 0, x: 0 },
              { name: 'style.resolve', t: 0.8, color: '#b48cff', depth: 1, x: 0 },
              { name: 'layout.reflow', t: 1.6, color: '#b48cff', depth: 1, x: 0.8 },
              { name: 'text.shape', t: 0.5, color: '#c8a8ff', depth: 2, x: 0.9 },
              { name: 'paint.nanovg', t: 1.9, color: '#4ed18a', depth: 1, x: 2.4 },
              { name: 'fill.paths', t: 1.1, color: '#5be8a0', depth: 2, x: 2.6 },
              { name: 'glyph.atlas', t: 0.6, color: '#5be8a0', depth: 2, x: 3.7 },
              { name: 'gpu.submit', t: 1.2, color: '#f2b14a', depth: 1, x: 4.3 },
            ].map((p, i) => (
              <div key={i} className="dcs-tooltip" data-tip={`${p.name} · ${p.t.toFixed(1)}ms`}
                   style={{
                     position: 'absolute',
                     left: `${(p.x / 6.2) * 100}%`,
                     width: `calc(${(p.t / 6.2) * 100}% - 2px)`,
                     top: p.depth * 24,
                     height: 20,
                     background: p.color, borderRadius: 2,
                     display: 'flex', alignItems: 'center', padding: '0 6px',
                     fontSize: 10, fontFamily: 'var(--dcs-font-mono)',
                     color: '#0a1220', fontWeight: 500,
                     overflow: 'hidden', whiteSpace: 'nowrap', cursor: 'default',
                   }}>
                {p.name}
              </div>
            ))}
          </div>
        </div>

        {/* Probe breadcrumb log */}
        <div style={{ width: 300, flex: '0 0 300px', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
          <div style={{ display: 'flex', alignItems: 'center', height: 24, padding: '0 10px', borderBottom: '1px solid var(--dcs-line)', background: 'var(--dcs-surface-1)' }}>
            <Icon name="timeline" size="sm" style={{ color: 'var(--dcs-accent)' }} />
            <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--dcs-text-dim)', fontWeight: 500 }}>Probe log</span>
            <span style={{ flex: 1 }} />
            <span style={{ fontSize: 10, color: 'var(--dcs-text-mute)', fontFamily: 'var(--dcs-font-mono)' }}>{log.length}</span>
          </div>
          <div ref={logRef} style={{ flex: 1, overflow: 'auto', fontFamily: 'var(--dcs-font-mono)', fontSize: 11 }}>
            {log.map((e, i) => {
              const c = chMeta(e.ch);
              return (
                <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '2px 10px', borderBottom: '1px solid var(--dcs-line-soft)', color: 'var(--dcs-text-dim)' }}>
                  <span style={{ color: 'var(--dcs-text-mute)', fontSize: 10 }}>{e.t}</span>
                  <span style={{ width: 7, height: 7, borderRadius: 2, background: c ? c.color : 'var(--dcs-text-mute)', flex: '0 0 auto' }} />
                  <span style={{ flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', color: 'var(--dcs-text)' }}>{e.tag}</span>
                  <span style={{ color: e.tone === 'warn' ? 'var(--dcs-warn)' : 'var(--dcs-text-dim)', fontVariantNumeric: 'tabular-nums' }}>{e.ms.toFixed(1)}ms</span>
                </div>
              );
            })}
          </div>
        </div>
      </div>
    </div>
  );
}

function Stat({ label, value, tone }) {
  const c = tone === 'warn' ? 'var(--dcs-warn)' : tone === 'ok' ? 'var(--dcs-ok)' : 'var(--dcs-text)';
  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end', minWidth: 52 }}>
      <span style={{ fontSize: 9, textTransform: 'uppercase', letterSpacing: '.08em', color: 'var(--dcs-text-mute)', fontFamily: 'var(--dcs-font-mono)' }}>{label}</span>
      <span style={{ fontSize: 13, color: c, fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums' }}>{value}</span>
    </div>
  );
}

/* ============================================================
   MEMORY — allocation breakdown by category
   ============================================================ */
function MemoryPanel() {
  const cats = [
    { name: 'DOM nodes',      count: 1284, bytes: 411008,  color: '#4d9fff', desc: 'lexbor element + attr storage' },
    { name: 'Computed styles', count: 1284, bytes: 328704, color: '#b48cff', desc: 'resolved style structs' },
    { name: 'Layout boxes',   count: 1102, bytes: 211584,  color: '#4ed18a', desc: 'flex/grid box tree' },
    { name: 'Glyph atlas',    count: 3,    bytes: 2097152, color: '#f2b14a', desc: 'GPU font texture (2048²)' },
    { name: 'Vertex buffers', count: 42,   bytes: 786432,  color: '#4ad5d5', desc: 'nanovg path geometry' },
    { name: 'Textures',       count: 18,   bytes: 5242880, color: '#ff7ab8', desc: 'images + gradients' },
    { name: 'Text runs',      count: 640,  bytes: 92160,   color: '#6fb3ff', desc: 'shaped glyph runs' },
  ];
  const total = cats.reduce((a, c) => a + c.bytes, 0);
  const fmt = (b) => b > 1048576 ? `${(b / 1048576).toFixed(1)} MB` : `${(b / 1024).toFixed(0)} KB`;
  const [sel, setSel] = useDT(3);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', minHeight: 0 }}>
      {/* Toolbar */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '8px 12px', borderBottom: '1px solid var(--dcs-line)' }}>
        <Button sm iconLeft="record">Take snapshot</Button>
        <Button sm ghost iconLeft="trash">Collect</Button>
        <div style={{ flex: 1 }} />
        <Stat label="heap" value={fmt(total)} />
        <Stat label="objects" value={cats.reduce((a, c) => a + c.count, 0).toLocaleString()} />
      </div>

      {/* Stacked bar */}
      <div style={{ padding: 12, borderBottom: '1px solid var(--dcs-line)' }}>
        <div style={{ display: 'flex', height: 28, borderRadius: 3, overflow: 'hidden', border: '1px solid var(--dcs-line)' }}>
          {cats.map((c, i) => (
            <div key={c.name} className="dcs-tooltip" data-tip={`${c.name} · ${fmt(c.bytes)}`}
                 onClick={() => setSel(i)}
                 style={{ width: `${(c.bytes / total) * 100}%`, background: c.color, cursor: 'pointer', opacity: sel === i ? 1 : 0.7 }} />
          ))}
        </div>
      </div>

      {/* Table */}
      <div style={{ flex: 1, overflow: 'auto' }}>
        <table className="dcs-table dcs-table--mono">
          <thead>
            <tr><th style={{ width: 20 }}></th><th>Category</th><th>Count</th><th>Size</th><th>%</th></tr>
          </thead>
          <tbody>
            {cats.map((c, i) => (
              <tr key={c.name} aria-selected={sel === i} onClick={() => setSel(i)} style={{ cursor: 'pointer' }}>
                <td><span style={{ display: 'inline-block', width: 10, height: 10, borderRadius: 2, background: c.color }} /></td>
                <td style={{ fontFamily: 'var(--dcs-font)' }}>
                  <div>{c.name}</div>
                  <div style={{ fontSize: 10, color: 'var(--dcs-text-mute)' }}>{c.desc}</div>
                </td>
                <td>{c.count.toLocaleString()}</td>
                <td>{fmt(c.bytes)}</td>
                <td>{((c.bytes / total) * 100).toFixed(1)}%</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

/* ============================================================
   SHELL
   ============================================================ */
const TABS = [
  { id: 'elements', label: 'Elements', icon: 'array', Comp: ElementsPanel },
  { id: 'sources', label: 'Sources', icon: 'file', Comp: SourcesPanel },
  { id: 'performance', label: 'Performance', icon: 'graph', Comp: PerformancePanel },
  { id: 'memory', label: 'Memory', icon: 'cpu', Comp: MemoryPanel },
];

function DevTools() {
  const [tab, setTab] = useDT('elements');
  const Active = TABS.find(t => t.id === tab).Comp;
  return (
    <div className="dcs" data-dcs-density="compact" style={{ height: '100vh', display: 'flex', flexDirection: 'column', background: 'var(--dcs-bg)' }}>
      {/* Top tab strip */}
      <div style={{ display: 'flex', alignItems: 'stretch', height: 30, background: 'var(--dcs-surface-1)', borderBottom: '1px solid var(--dcs-line)' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '0 12px', borderRight: '1px solid var(--dcs-line)', color: 'var(--dcs-accent)' }}>
          <Icon name="cross-target" />
          <span style={{ fontWeight: 600, fontSize: 12, letterSpacing: '.02em', color: 'var(--dcs-text)' }}>affinetools</span>
        </div>
        {TABS.map(t => (
          <button key={t.id} onClick={() => setTab(t.id)}
                  style={{
                    display: 'inline-flex', alignItems: 'center', gap: 6,
                    padding: '0 16px', border: 'none', cursor: 'pointer',
                    fontSize: 12, fontFamily: 'var(--dcs-font)',
                    background: tab === t.id ? 'var(--dcs-bg)' : 'transparent',
                    color: tab === t.id ? 'var(--dcs-text)' : 'var(--dcs-text-dim)',
                    borderBottom: tab === t.id ? '2px solid var(--dcs-accent)' : '2px solid transparent',
                  }}>
            <Icon name={t.icon} size="sm" style={{ color: tab === t.id ? 'var(--dcs-accent)' : 'var(--dcs-text-mute)' }} />
            {t.label}
          </button>
        ))}
        <div style={{ flex: 1 }} />
        <button style={{ display: 'flex', alignItems: 'center', padding: '0 12px', border: 'none', background: 'transparent', color: 'var(--dcs-text-mute)', cursor: 'pointer' }}>
          <Icon name="cog" size="sm" />
        </button>
        <button style={{ display: 'flex', alignItems: 'center', padding: '0 12px', border: 'none', background: 'transparent', color: 'var(--dcs-text-mute)', cursor: 'pointer' }}>
          <Icon name="close" size="sm" />
        </button>
      </div>

      {/* Active panel */}
      <div style={{ flex: 1, minHeight: 0, overflow: 'hidden' }}>
        <Active />
      </div>

      {/* Status bar */}
      <div style={{ height: 22, display: 'flex', alignItems: 'center', gap: 12, padding: '0 12px', background: 'var(--dcs-surface-1)', borderTop: '1px solid var(--dcs-line)', fontSize: 10, fontFamily: 'var(--dcs-font-num)', color: 'var(--dcs-text-mute)' }}>
        <span className="dcs-badge dcs-badge--ok dcs-badge--dot" style={{ fontSize: 9, height: 14 }}>ATTACHED</span>
        <span>AffineUI 0.4 · sokol_gfx · Metal</span>
        <span style={{ flex: 1 }} />
        <span>1284 nodes · 60.0 fps · 9.1 MB</span>
      </div>
    </div>
  );
}

function mountDT() {
  ReactDOM.createRoot(document.getElementById('root')).render(<DevTools />);
}
if (window.__deciusIconsLoaded) mountDT();
else window.addEventListener('decius-icons-loaded', mountDT);
