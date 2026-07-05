/* sections-data.jsx
   Lists · Trees · Tables · Badges
*/
const { useState: useStateD } = React;

function SectionLists() {
  const [sel, setSel] = useStateD(2);
  const items = [
    { id: 0, name: 'Lambert.001',     type: 'material', val: '#4d9fff', icon: 'palette' },
    { id: 1, name: 'CarPaint_red',    type: 'material', val: '#ff6b6b', icon: 'palette' },
    { id: 2, name: 'Concrete_Rough',  type: 'material', val: '#7c8492', icon: 'palette' },
    { id: 3, name: 'GlassGreen_01',   type: 'material', val: '#4ed18a', icon: 'palette' },
    { id: 4, name: 'StudioBack.exr',  type: 'image',    val: '4k · 32-bit', icon: 'image' },
    { id: 5, name: 'AnimWalk.fbx',    type: 'rig',      val: '48 bones', icon: 'bone' },
  ];
  return (
    <section className="dw-section" id="lists">
      <div className="dw-section__eyebrow">Data · 01</div>
      <h2>Lists</h2>
      <p className="dw-section__lead">
        Flat, selectable rows. Use when items don't nest. The 2px accent ribbon on selection is
        intentional — it survives reorder, drag, and quick visual scan.
      </p>
      <Demo frame="app">
        <div style={{ maxWidth: 360 }}>
          <Panel title="Assets" icon="folder" pad={0}
                 tools={<><Button ghost sm icon iconLeft="search" /><Button ghost sm icon iconLeft="plus" /></>}>
            <div className="dcs-list">
              {items.map(it => (
                <div key={it.id} className="dcs-list__item" aria-selected={sel === it.id} onClick={() => setSel(it.id)} style={{ position: 'relative' }}>
                  <Icon name={it.icon} />
                  <span style={{ flex: 1 }}>{it.name}</span>
                  <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text-mute)' }}>{it.val}</span>
                </div>
              ))}
            </div>
          </Panel>
        </div>
      </Demo>
    </section>
  );
}

function SectionTrees() {
  const [expanded, setExpanded] = useStateD(new Set(['scn', 'env', 'chars', 'jane', 'rig']));
  const [sel, setSel] = useStateD('lhand');
  const toggle = (id) => setExpanded(prev => {
    const n = new Set(prev);
    if (n.has(id)) n.delete(id); else n.add(id);
    return n;
  });
  return (
    <section className="dw-section" id="trees">
      <div className="dw-section__eyebrow">Data · 02</div>
      <h2>Trees</h2>
      <p className="dw-section__lead">
        The outliner. Deep hierarchies of scenes, rigs, layers, nodes. Chevrons expand;
        icons signal kind; right-aligned meta gives counts without taking column width.
      </p>
      <Demo frame="app">
        <div style={{ maxWidth: 380 }}>
          <Panel title="Outliner" icon="layers" pad={0}
                 tools={<><Button ghost sm icon iconLeft="search" /><Button ghost sm icon iconLeft="filter-lp" /><Button ghost sm icon iconLeft="more-h" /></>}>
            <div style={{ padding: '4px 0', minHeight: 320 }}>
              <Tree
                expanded={expanded}
                onExpand={toggle}
                selected={sel}
                onSelect={setSel}
                nodes={[{
                  id: 'scn', label: 'Scene_Intro_v014', icon: 'globe', meta: '37',
                  children: [
                    { id: 'env', label: 'Environment', icon: 'folder-open', meta: '4', children: [
                      { id: 'hdri', label: 'Sky_4k.hdr', icon: 'image' },
                      { id: 'fog', label: 'VolumeFog', icon: 'cube' },
                      { id: 'sun', label: 'Sun.001', icon: 'light' },
                      { id: 'ground', label: 'GroundPlane', icon: 'plane' },
                    ]},
                    { id: 'chars', label: 'Characters', icon: 'folder-open', meta: '2', children: [
                      { id: 'jane', label: 'Jane', icon: 'folder-open', children: [
                        { id: 'mesh', label: 'jane_body.geo', icon: 'mesh', meta: '64,201' },
                        { id: 'rig', label: 'jane_rig', icon: 'bone', children: [
                          { id: 'spine', label: 'spine', icon: 'bone' },
                          { id: 'lhand', label: 'L_hand', icon: 'bone', meta: 'IK' },
                          { id: 'rhand', label: 'R_hand', icon: 'bone' },
                        ]},
                        { id: 'mat', label: 'jane_skin.mat', icon: 'palette' },
                      ]},
                      { id: 'eric', label: 'Eric', icon: 'folder' },
                    ]},
                    { id: 'cams', label: 'Cameras', icon: 'folder', meta: '3' },
                    { id: 'fx', label: 'FX', icon: 'folder', meta: '7' },
                  ]
                }]}
              />
            </div>
          </Panel>
        </div>
      </Demo>
    </section>
  );
}

function SectionTables() {
  return (
    <section className="dw-section" id="tables">
      <div className="dw-section__eyebrow">Data · 03</div>
      <h2>Tables</h2>
      <p className="dw-section__lead">
        Render queues, file lists, channel banks. Sticky-headered, mono-numeric, with selectable
        rows. Sub-header eyebrows act like spreadsheet rulers.
      </p>
      <Demo frame="app" inset>
        <Panel title="Render queue" icon="render" pad={0}
               tools={<><Button ghost sm icon iconLeft="play" /><Button ghost sm icon iconLeft="pause" /><Button ghost sm icon iconLeft="trash" /></>}>
          <table className="dcs-table dcs-table--mono">
            <thead>
              <tr><th style={{width: 28}}></th><th>Job</th><th>Frames</th><th>Engine</th><th>Time</th><th>Status</th></tr>
            </thead>
            <tbody>
              <tr aria-selected="true"><td><Icon name="check-circle" style={{ color: 'var(--dcs-ok)' }} /></td><td style={{ fontFamily: 'var(--dcs-font)' }}>Scene_Intro_v014</td><td>1–240</td><td>Cycles</td><td>04:12:08</td><td><span className="dcs-badge dcs-badge--ok dcs-badge--dot">Done</span></td></tr>
              <tr><td><Icon name="play" style={{ color: 'var(--dcs-accent)' }} /></td><td style={{ fontFamily: 'var(--dcs-font)' }}>Hero_Closeup_v003</td><td>48–96</td><td>Cycles</td><td>00:48:32</td><td><span className="dcs-badge dcs-badge--accent dcs-badge--dot">62%</span></td></tr>
              <tr><td><Icon name="pause" style={{ color: 'var(--dcs-text-mute)' }} /></td><td style={{ fontFamily: 'var(--dcs-font)' }}>Title_Anim_v007</td><td>1–120</td><td>Eevee</td><td>—</td><td><span className="dcs-badge">Queued</span></td></tr>
              <tr><td><Icon name="alert" style={{ color: 'var(--dcs-warn)' }} /></td><td style={{ fontFamily: 'var(--dcs-font)' }}>BgPlate_Lit_v002</td><td>120–240</td><td>Cycles</td><td>00:00:48</td><td><span className="dcs-badge dcs-badge--warn dcs-badge--dot">Missing tex</span></td></tr>
              <tr><td><Icon name="error" style={{ color: 'var(--dcs-danger)' }} /></td><td style={{ fontFamily: 'var(--dcs-font)' }}>Test_Sim_009</td><td>1–48</td><td>Mantaflow</td><td>00:12:04</td><td><span className="dcs-badge dcs-badge--danger dcs-badge--dot">Fault</span></td></tr>
            </tbody>
          </table>
        </Panel>
      </Demo>
    </section>
  );
}

function SectionBadges() {
  return (
    <section className="dw-section" id="badges">
      <div className="dw-section__eyebrow">Data · 04</div>
      <h2>Badges &amp; keys</h2>
      <p className="dw-section__lead">
        Six semantic chips and a kbd glyph for shortcut hints. Composable on any background — pick
        the variant by signal, not by hue you happen to like.
      </p>
      <Demo>
        <div className="dcs-col" style={{ gap: 12 }}>
          <div className="dcs-row" style={{ gap: 8, flexWrap: 'wrap' }}>
            <span className="dcs-badge">DEFAULT</span>
            <span className="dcs-badge dcs-badge--accent">ACCENT</span>
            <span className="dcs-badge dcs-badge--ok dcs-badge--dot">READY</span>
            <span className="dcs-badge dcs-badge--warn dcs-badge--dot">CLIPPING</span>
            <span className="dcs-badge dcs-badge--danger dcs-badge--dot">FAULT</span>
            <span className="dcs-badge dcs-badge--accent">v0.4.0</span>
          </div>
          <div className="dcs-row" style={{ gap: 6, alignItems: 'center' }}>
            <span style={{ fontSize: 12, color: 'var(--dcs-text-dim)' }}>Snap to grid</span>
            <span className="dcs-kbd">⇧</span><span className="dcs-kbd">G</span>
            <span style={{ flex: 1 }} />
            <span style={{ fontSize: 12, color: 'var(--dcs-text-dim)' }}>Frame all</span>
            <span className="dcs-kbd">F</span>
            <span style={{ flex: 1 }} />
            <span style={{ fontSize: 12, color: 'var(--dcs-text-dim)' }}>Render</span>
            <span className="dcs-kbd">⌃</span><span className="dcs-kbd">F12</span>
          </div>
        </div>
      </Demo>
    </section>
  );
}

Object.assign(window, { SectionLists, SectionTrees, SectionTables, SectionBadges, SectionCards });

function SectionCards() {
  const [sel, setSel] = useStateD('m2');
  const materials = [
    { id: 'm1', name: 'Lambert.001',   meta: 'PBR · 4K · sRGB', g: 'linear-gradient(135deg, #4d9fff, #1c4080)' },
    { id: 'm2', name: 'CarPaint_Red',  meta: 'PBR · 4K · clearcoat', g: 'linear-gradient(135deg, #ff7070, #8a1818)' },
    { id: 'm3', name: 'Concrete_R',    meta: 'PBR · 2K', g: 'linear-gradient(135deg, #aab0bd, #6b7180)' },
    { id: 'm4', name: 'GlassGreen_01', meta: 'PBR · 1K · transmissive', g: 'linear-gradient(135deg, #4ed18a, #1a6e44)' },
    { id: 'm5', name: 'Brushed_Steel', meta: 'PBR · 4K', g: 'repeating-linear-gradient(45deg, #cfd4dc, #cfd4dc 1px, #aab0bd 1px, #aab0bd 3px)' },
    { id: 'm6', name: 'Marble_Statue', meta: 'PBR · 4K', g: 'linear-gradient(45deg, #e7e9ee, #aab0bd)' },
  ];
  const recent = [
    { id: 'r1', name: 'jane_walk_v02.fbx', meta: '48 bones · 2.4 MB · 4 minutes ago', icon: 'bone' },
    { id: 'r2', name: 'crate_lod1.usd',    meta: '128 verts · 14 KB · today',          icon: 'cube' },
    { id: 'r3', name: 'sky_dawn_4k.hdr',   meta: 'HDR · 32-bit · yesterday',           icon: 'image' },
  ];
  return (
    <section className="dw-section" id="cards">
      <div className="dw-section__eyebrow">Data · 05</div>
      <h2>Cards</h2>
      <p className="dw-section__lead">
        A reusable container for browseable items: assets, presets, recent files, render queue
        thumbnails. Cards compose into <code>CardGrid</code> for visual browsers and
        <code>CardList</code> for vertical stacks. Hover reveals tools and an optional close X.
      </p>

      <Demo frame="app" caption="Card grid — material browser, with selection, hover tools, and close affordance">
        <CardGrid>
          {materials.map(m => (
            <Card
              key={m.id}
              selected={sel === m.id}
              onClick={() => setSel(m.id)}
              closeable
              media={<div style={{ aspectRatio: '1', background: m.g }} />}
              title={m.name}
              meta={m.meta}
              badges={<span className="dcs-badge dcs-badge--soft">PBR</span>}
              tools={<><Button sm icon iconLeft="eye" /><Button sm icon iconLeft="more-v" /></>}
            />
          ))}
        </CardGrid>
      </Demo>

      <Demo frame="app" caption="Card list — horizontal layout for vertical stacks">
        <CardList style={{ maxWidth: 520 }}>
          {recent.map(r => (
            <Card
              key={r.id}
              horizontal
              closeable
              media={<div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--dcs-accent)' }}><Icon name={r.icon} size="xl" /></div>}
              title={r.name}
              meta={r.meta}
              tools={<><Button sm icon iconLeft="export" /><Button sm icon iconLeft="copy" /></>}
            />
          ))}
        </CardList>
      </Demo>

      <Demo frame="app" caption="Inside a panel — close button removes the card from the layout">
        <Panel title="Active layers" icon="layers" pad="sm" style={{ maxWidth: 380 }}>
          <CardList>
            <Card title="Background_v3" meta="2,048 × 2,048 · 12 KB" closeable
                  media={<div style={{ aspectRatio: '4/1', background: 'linear-gradient(90deg, #1c4080, #4d9fff)' }} />}
                  badges={<span className="dcs-badge dcs-badge--ok dcs-badge--dot">VISIBLE</span>}
            />
            <Card title="Hero_Pose" meta="4 channels · 144 KB" closeable
                  media={<div style={{ aspectRatio: '4/1', background: 'radial-gradient(circle at 30% 50%, #ff7ab8, #6f4eea)' }} />}
                  badges={<span className="dcs-badge dcs-badge--accent">ALPHA</span>}
            />
            <Card title="FX_Sparks" meta="emitter · 1,200 particles" closeable
                  media={<div style={{ aspectRatio: '4/1', background: 'radial-gradient(ellipse at 70% 50%, #f2b14a, #14161c)' }} />}
                  badges={<span className="dcs-badge dcs-badge--warn dcs-badge--dot">SOLO</span>}
            />
          </CardList>
        </Panel>
      </Demo>
    </section>
  );
}
