/* app.jsx — main docs site wiring */
const { useState: useStateApp, useEffect: useEffectApp } = React;

const TWEAK_DEFAULTS = /*EDITMODE-BEGIN*/{
  "accent": "blue",
  "darkness": "default",
  "density": "comfortable",
  "radius": "default"
}/*EDITMODE-END*/;

const NAV = [
  { group: 'Overview', items: [
    { id: 'top',    label: 'Introduction',  icon: 'decius' },
    { id: 'why',    label: 'Why decius',    icon: 'help' },
    { id: 'install',label: 'Install',       icon: 'rocket' },
  ]},
  { group: 'Foundations', items: [
    { id: 'colors',  label: 'Color',    icon: 'palette' },
    { id: 'type',    label: 'Type',     icon: 'edit' },
    { id: 'spacing', label: 'Spacing',  icon: 'grid' },
    { id: 'icons',   label: 'Icons',    icon: 'star' },
  ]},
  { group: 'Components', items: [
    { id: 'buttons', label: 'Buttons',  icon: 'check-circle' },
    { id: 'inputs',  label: 'Inputs',   icon: 'edit' },
    { id: 'sliders', label: 'Sliders & faders',  icon: 'wave-sine' },
    { id: 'knobs',   label: 'Knobs',    icon: 'cog' },
    { id: 'combo',   label: 'Combo fields', icon: 'caret-up-down' },
    { id: 'checks',  label: 'Checks · switches', icon: 'check' },
  ]},
  { group: 'Layout', items: [
    { id: 'panels',  label: 'Panels',     icon: 'array' },
    { id: 'subpanels', label: 'Subpanels', icon: 'menu' },
    { id: 'foldouts',  label: 'Foldouts',  icon: 'chevron-down' },
    { id: 'dock',    label: 'Dock panels', icon: 'layers' },
  ]},
  { group: 'Data', items: [
    { id: 'lists',  label: 'Lists',  icon: 'menu' },
    { id: 'trees',  label: 'Trees',  icon: 'folder-open' },
    { id: 'tables', label: 'Tables', icon: 'grid' },
    { id: 'cards',  label: 'Cards',  icon: 'image' },
    { id: 'badges', label: 'Badges & keys', icon: 'tag' },
  ]},
  { group: 'Feedback', items: [
    { id: 'alerts', label: 'Alerts', icon: 'alert' },
    { id: 'modals', label: 'Modals', icon: 'info' },
  ]},
  { group: 'Editors', items: [
    { id: 'color-pickers', label: 'Color pickers', icon: 'droplet' },
    { id: 'curve',         label: 'Curve editor',  icon: 'curve' },
    { id: 'graph',         label: 'Node graph',    icon: 'graph' },
    { id: 'textures',      label: 'Texture pickers', icon: 'texture' },
  ]},
  { group: 'Showpieces', items: [
    { id: 'sample-dcc',   label: 'Sample DCC tool', icon: 'cube' },
    { id: 'sample-synth', label: 'Sample synth',    icon: 'wave-saw' },
    { id: 'skeuomorphic', label: 'Skeuomorphic hw', icon: 'cable' },
  ]},
];

function Sidebar({ active, onJump }) {
  return (
    <aside className="dw-sidebar">
      {NAV.map(group => (
        <div key={group.group}>
          <div className="dw-sidebar__group">{group.group}</div>
          {group.items.map(it => (
            <a key={it.id} href={`#${it.id}`}
               className={active === it.id ? 'active' : ''}
               onClick={(e) => { e.preventDefault(); onJump(it.id); }}>
              <Icon name={it.icon} size="sm" /> {it.label}
            </a>
          ))}
        </div>
      ))}
    </aside>
  );
}

function TopBar() {
  return (
    <header className="dw-top">
      <a href="#top" className="dw-brand">
        <span className="dw-brand__mark">
          <Icon name="decius" size="lg" />
        </span>
        <span>decius<span style={{ color: 'var(--dw-accent-lo)' }}>.css</span></span>
        <span className="dw-brand__version">v0.4.0 Mus</span>
      </a>
      <nav className="dw-nav">
        <a href="#install" className="active">Docs</a>
        <a href="#buttons">Components</a>
        <a href="#sample-dcc">Showpieces</a>
        <a href="#install">Install</a>
      </nav>
      <div className="dw-top__spacer" />
      <div className="dw-top__actions">
        <div className="dw-search">
          <Icon name="search" size="sm" />
          <span>Search components…</span>
          <kbd>⌘K</kbd>
        </div>
        <a href="#" className="dw-cta dw-cta--ghost" style={{ height: 32, padding: '0 12px' }}>
          <Icon name="export" size="sm" /> GitHub
        </a>
      </div>
    </header>
  );
}

const ACCENT_OPTIONS = [
  { name: 'blue',   hex: '#4d9fff' },
  { name: 'cyan',   hex: '#00b8d4' },
  { name: 'orange', hex: '#ff8a3a' },
  { name: 'violet', hex: '#8b6dff' },
  { name: 'green',  hex: '#3dd68a' },
];

function Tweaks() {
  const [t, setTweak] = useTweaks(TWEAK_DEFAULTS);

  // Apply tweaks at the document level — the CSS vars then cascade through
  // every .dcs subtree. Demos that have their own attribute explicitly set
  // override locally (per-demo toggle wins; otherwise the global cascades in).
  useEffectApp(() => {
    const root = document.body;
    if (t.accent === 'blue') root.removeAttribute('data-dcs-accent');
    else root.setAttribute('data-dcs-accent', t.accent);
    if (t.darkness === 'default') root.removeAttribute('data-dcs-dark');
    else root.setAttribute('data-dcs-dark', t.darkness);
    root.setAttribute('data-dcs-density', t.density);
    if (t.radius === 'default') root.removeAttribute('data-dcs-radius');
    else root.setAttribute('data-dcs-radius', t.radius);
  }, [t]);

  const currentHex = (ACCENT_OPTIONS.find(o => o.name === t.accent) || ACCENT_OPTIONS[0]).hex;

  return (
    <TweaksPanel title="Tweaks">
      <TweakSection title="Theme">
        <TweakColor
          label="Accent"
          options={ACCENT_OPTIONS.map(o => o.hex)}
          value={currentHex}
          onChange={hex => {
            const opt = ACCENT_OPTIONS.find(o => o.hex === hex);
            if (opt) setTweak('accent', opt.name);
          }}
        />
        <TweakRadio
          label="Darkness"
          options={[
            { value: 'darker',  label: 'Darker' },
            { value: 'default', label: 'Default' },
            { value: 'lighter', label: 'Lighter' },
          ]}
          value={t.darkness}
          onChange={v => setTweak('darkness', v)}
        />
      </TweakSection>
      <TweakSection title="Sizing">
        <TweakRadio
          label="Density"
          options={[
            { value: 'compact', label: 'Compact' },
            { value: 'comfortable', label: 'Cozy' },
            { value: 'spacious', label: 'Spacious' },
          ]}
          value={t.density}
          onChange={v => setTweak('density', v)}
        />
        <TweakRadio
          label="Radius"
          options={[
            { value: 'sharp', label: 'Sharp' },
            { value: 'default', label: 'Default' },
            { value: 'round', label: 'Round' },
          ]}
          value={t.radius}
          onChange={v => setTweak('radius', v)}
        />
      </TweakSection>
    </TweaksPanel>
  );
}

function App() {
  const [active, setActive] = useStateApp('top');
  useEffectApp(() => {
    // Sync hash → active
    const sync = () => {
      const h = window.location.hash.slice(1);
      if (h) setActive(h);
    };
    sync();
    window.addEventListener('hashchange', sync);

    // Intersection observer for scroll-spy
    const sections = NAV.flatMap(g => g.items.map(i => i.id));
    const els = sections.map(id => document.getElementById(id)).filter(Boolean);
    const obs = new IntersectionObserver(entries => {
      entries.forEach(e => {
        if (e.isIntersecting) setActive(e.target.id);
      });
    }, { rootMargin: '-30% 0% -60% 0%', threshold: 0 });
    els.forEach(el => obs.observe(el));

    return () => {
      window.removeEventListener('hashchange', sync);
      obs.disconnect();
    };
  }, []);

  const jump = (id) => {
    setActive(id);
    const el = document.getElementById(id);
    if (el) {
      const y = el.getBoundingClientRect().top + window.scrollY - 72;
      window.scrollTo({ top: y, behavior: 'smooth' });
      history.replaceState(null, '', `#${id}`);
    }
  };

  return (
    <div className="dw-page">
      <TopBar />
      <Sidebar active={active} onJump={jump} />
      <main className="dw-main">
        <SectionHero />
        <SectionWhy />
        <SectionInstall />

        <hr />
        <SectionColors />
        <SectionType />
        <SectionSpacing />
        <SectionIcons />

        <hr />
        <SectionButtons />
        <SectionInputs />
        <SectionSliders />
        <SectionKnobs />
        <SectionCombo />
        <SectionChecksSwitches />

        <hr />
        <SectionPanels />
        <SectionSubpanels />
        <SectionFoldouts />
        <SectionDock />

        <hr />
        <SectionLists />
        <SectionTrees />
        <SectionTables />
        <SectionCards />
        <SectionBadges />

        <hr />
        <SectionAlerts />
        <SectionModals />

        <hr />
        <SectionColorPicker />
        <SectionCurve />
        <SectionGraph />
        <SectionTexture />

        <hr />
        <SectionSampleDCC />
        <SectionSampleSynth />
        <SectionSkeuomorphic />

        <footer style={{ marginTop: 80, paddingTop: 32, borderTop: '1px solid var(--dw-line)', display: 'flex', gap: 28, color: 'var(--dw-text-mute)', fontSize: 13, flexWrap: 'wrap', alignItems: 'center' }}>
          <span className="dw-brand" style={{ fontSize: 14 }}>
            <span className="dw-brand__mark" style={{ width: 24, height: 24 }}><Icon name="decius" /></span>
            decius.css
          </span>
          <span>v0.4.0 "Mus" · MIT</span>
          <span style={{ flex: 1 }} />
          <a href="#install">Install</a>
          <a href="#colors">Tokens</a>
          <a href="#sample-dcc">Showpieces</a>
          <a href="#">GitHub</a>
          <a href="#">Changelog</a>
        </footer>
      </main>
      <Tweaks />
    </div>
  );
}

// Mount once icons are loaded
function mount() {
  ReactDOM.createRoot(document.getElementById('root')).render(<App />);
}
if (window.__deciusIconsLoaded) mount();
else window.addEventListener('decius-icons-loaded', mount);
