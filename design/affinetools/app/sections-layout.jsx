/* sections-layout.jsx
   Panels · Dockpanels · Toolbars
*/
const { useState: useStateL } = React;

function SectionPanels() {
  return (
    <section className="dw-section" id="panels">
      <div className="dw-section__eyebrow">Layout · 01</div>
      <h2>Panels</h2>
      <p className="dw-section__lead">
        The atomic unit of a DCC interface. Every panel is a header strip, a body, and an optional
        status footer — wrapped in a 1px dark frame that gives the page its grid-snap feeling.
      </p>

      <Demo frame="app" caption="The standard panel, raised variant, and a closeable one with an X in the header">
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 12 }}>
          <Panel title="Properties" icon="cog">
            <div className="dcs-col" style={{ gap: 6 }}>
              <div className="dcs-field"><label className="dcs-field__label">Name</label><input className="dcs-input" defaultValue="Cube.003" /></div>
              <div className="dcs-field"><label className="dcs-field__label">Mass</label><Combo value={1.000} onChange={() => {}} min={0} max={100} step={0.001} format={v => v.toFixed(3)} width="100%" /></div>
              <div className="dcs-field"><label className="dcs-field__label">Friction</label><Combo value={0.500} onChange={() => {}} min={0} max={1} step={0.001} format={v => v.toFixed(3)} width="100%" /></div>
            </div>
          </Panel>
          <Panel raised title="Material" icon="palette" headerActive
                 tools={<><Button ghost sm icon iconLeft="plus" /><Button ghost sm icon iconLeft="more-h" /></>}>
            <div className="dcs-col" style={{ gap: 8 }}>
              <Swatch color="#4d9fff" label="Albedo" />
              <Swatch color="#23262e" label="Specular" />
              <Swatch color="#b48cff" label="Emission" />
            </div>
          </Panel>
          <Panel title="Status" icon="info" closeable>
            <div className="dcs-col" style={{ gap: 6, fontSize: 12, color: 'var(--dcs-text-dim)' }}>
              <div><span className="dcs-badge dcs-badge--ok dcs-badge--dot">VALID</span> &nbsp; mesh topology</div>
              <div><span className="dcs-badge dcs-badge--warn dcs-badge--dot">12 NGONS</span></div>
              <div><span className="dcs-badge dcs-badge--accent">120,481 TRI</span></div>
            </div>
          </Panel>
        </div>
      </Demo>

      <Demo frame="app" caption="Tabbed panel — Blender-style header tabs">
        <Panel pad={0}>
          <Tabs
            value="anim"
            onChange={() => {}}
            tabs={[
              { value: 'world', label: 'World', icon: 'globe' },
              { value: 'mesh', label: 'Mesh', icon: 'mesh' },
              { value: 'anim', label: 'Animation', icon: 'curve' },
              { value: 'render', label: 'Render', icon: 'render' },
            ]}
          />
          <div style={{ padding: 16 }}>
            <div className="dcs-col" style={{ gap: 8 }}>
              <div className="dcs-field"><label className="dcs-field__label">FPS</label><Combo value={24} onChange={() => {}} min={1} max={120} step={1} format={v => `${v}`} width={100} /></div>
              <div className="dcs-field"><label className="dcs-field__label">Length</label><Combo value={240} onChange={() => {}} min={1} max={9999} step={1} format={v => `${v} frames`} width={140} /></div>
              <div className="dcs-field"><label className="dcs-field__label">Loop</label><Switch checked onChange={() => {}} /></div>
            </div>
          </div>
        </Panel>
      </Demo>
    </section>
  );
}

function SectionDock() {
  const [leftTab, setLeftTab] = useStateL('hier');
  const [centerTab, setCenterTab] = useStateL('scene');
  const [rightTab, setRightTab] = useStateL('inspector');
  const [bottomTab, setBottomTab] = useStateL('console');

  const [pos, setPos] = useStateL({ x: 1.428, y: -0.952, z: 3.000 });
  const [rough, setRough] = useStateL(0.42);
  const [metallic, setMetallic] = useStateL(0.08);
  const [wire, setWire] = useStateL(false);
  const [shadow, setShadow] = useStateL(true);
  const [shading, setShading] = useStateL('shaded');
  const [tool, setTool] = useStateL('select');

  return (
    <section className="dw-section" id="dock">
      <div className="dw-section__eyebrow">Layout · 03</div>
      <h2>Dock panels</h2>
      <p className="dw-section__lead">
        Compose panels into a workspace by stacking <code>dcs-dock</code> containers separated by
        1px line seams. Each dock pane carries a tab strip so multiple views share one slot —
        Unity-style — with X close affordances on hover. The seams between panes become hot when
        hovered for drag-resize.
      </p>

      <Demo frame="app" inset caption="A full app shell. Click tabs to switch views. Hover a tab to close it. Drag seams to resize.">
        <div style={{ display: 'flex', flexDirection: 'column', height: 540 }}>
          <MenuBar
            brand={{ icon: 'decius', label: 'modeler' }}
            items={['File', 'Edit', 'View', 'Mesh', 'Animation', 'Render', 'Window', 'Help']}
            meta={<>
              <span>Scene_Intro_v014.dcs</span>
              <span className="dcs-badge dcs-badge--accent">MODIFIED</span>
            </>}
          />
          <div className="dcs-dock" style={{ flex: 1, minHeight: 0 }}>
            {/* Left dock */}
            <div style={{ flex: '0 0 220px', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
              <DockPane
                tabs={[
                  { value: 'hier', label: 'Hierarchy', icon: 'layers' },
                  { value: 'proj', label: 'Project', icon: 'folder' },
                ]}
                value={leftTab}
                onChange={setLeftTab}
                tools={<Button ghost sm icon iconLeft="search" />}
              >
                {leftTab === 'hier' ? (
                  <Tree
                    expanded={new Set(['scene', 'env', 'chars'])}
                    onExpand={() => {}}
                    selected="cube"
                    onSelect={() => {}}
                    nodes={[{
                      id: 'scene', label: 'Scene_Intro', icon: 'globe', meta: '37',
                      children: [
                        { id: 'env', label: 'Environment', icon: 'folder-open', children: [
                          { id: 'sky', label: 'Sky_4k.hdr', icon: 'image' },
                          { id: 'fog', label: 'VolumeFog', icon: 'cube' },
                          { id: 'sun', label: 'Sun.001', icon: 'light' },
                        ]},
                        { id: 'chars', label: 'Characters', icon: 'folder-open', children: [
                          { id: 'cube', label: 'jane_body', icon: 'mesh', meta: '64k' },
                          { id: 'rig', label: 'jane_rig', icon: 'bone' },
                          { id: 'mat', label: 'jane_skin', icon: 'palette' },
                        ]},
                        { id: 'cam', label: 'Camera.001', icon: 'camera' },
                      ]
                    }]}
                  />
                ) : (
                  <div className="dcs-tree" style={{ padding: '4px 0' }}>
                    {[
                      { name: 'Assets', icon: 'folder-open' },
                      { name: 'Materials', icon: 'palette' },
                      { name: 'Textures', icon: 'texture' },
                      { name: 'Models', icon: 'mesh' },
                      { name: 'Sounds', icon: 'volume' },
                      { name: 'Scripts', icon: 'cpu' },
                    ].map((n, i) => (
                      <div key={n.name} className="dcs-tree__row" aria-selected={i === 2}>
                        <span style={{ width: 14 }} />
                        <Icon name={n.icon} className="dcs-tree__icon" />
                        <span className="dcs-tree__label">{n.name}</span>
                      </div>
                    ))}
                  </div>
                )}
              </DockPane>
            </div>
            <div className="dcs-splitter" />

            {/* Center column: viewport + bottom console */}
            <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0, minWidth: 0 }}>
              <DockPane
                tabs={[
                  { value: 'scene', label: 'Scene', icon: 'cube' },
                  { value: 'game', label: 'Game', icon: 'play' },
                  { value: 'uv', label: 'UV Editor', icon: 'uv' },
                ]}
                value={centerTab}
                onChange={setCenterTab}
                tools={<>
                  <Button ghost sm icon iconLeft="fit" />
                  <Button ghost sm icon iconLeft="fullscreen" />
                </>}
              >
                <div className="dcs-viewport">
                  <svg viewBox="-100 -60 200 120" preserveAspectRatio="xMidYMid slice" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
                    <defs>
                      <pattern id="dock-grid-v2" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="skewX(0) scale(1.5, 0.5)">
                        <path d="M 10 0 L 0 0 0 10" fill="none" stroke="rgba(255,255,255,.07)" strokeWidth=".25" />
                      </pattern>
                    </defs>
                    <rect x="-100" y="0" width="200" height="60" fill="url(#dock-grid-v2)" />
                    <line x1="-100" y1="0" x2="100" y2="0" stroke="rgba(255,255,255,.15)" strokeWidth=".4" />
                    <g transform="translate(-10 -8) rotate(18)" filter="drop-shadow(0 4px 12px rgba(0,0,0,.5))">
                      {wire ? (
                        <>
                          <path d="M-20-20 L20-20 L25-15 L25 15 L-16 22 L-20 12 Z" fill="none" stroke="#4d9fff" strokeWidth=".5"/>
                          <path d="M-20-20 L25-15 L25 15 L-16 22" fill="none" stroke="#4d9fff" strokeOpacity=".5" strokeWidth=".3"/>
                          <path d="M-20-20 L0 0 L20-20 M-20 12 L0 0 L25 15" fill="none" stroke="#4d9fff" strokeOpacity=".3" strokeWidth=".2"/>
                        </>
                      ) : (
                        <>
                          <path d="M-20-20 L20-20 L25-15 L25 15 L-16 22 L-20 12 Z" fill="rgba(77,159,255,.22)" stroke="#4d9fff" strokeWidth=".4" />
                          <path d="M-20-20 L25-15 L25 15 L-16 22 Z" fill="rgba(77,159,255,.10)" stroke="rgba(77,159,255,.35)" strokeWidth=".3" />
                        </>
                      )}
                    </g>
                  </svg>

                  {/* Floating tool ribbon — top-left */}
                  <div className="dcs-viewport__overlay dcs-viewport__overlay--tl">
                    <div className="dcs-viewport__floater">
                      <Button sm icon iconLeft="select" pressed={tool === 'select'} onClick={() => setTool('select')} />
                      <Button sm icon iconLeft="move" pressed={tool === 'move'} onClick={() => setTool('move')} />
                      <Button sm icon iconLeft="rotate" pressed={tool === 'rotate'} onClick={() => setTool('rotate')} />
                      <Button sm icon iconLeft="scale-corners" pressed={tool === 'scale'} onClick={() => setTool('scale')} />
                    </div>
                    <div className="dcs-viewport__floater">
                      <Button sm icon iconLeft="view-wire" pressed={shading === 'wire'} onClick={() => { setShading('wire'); setWire(true); }} />
                      <Button sm icon iconLeft="view-solid" pressed={shading === 'shaded'} onClick={() => { setShading('shaded'); setWire(false); }} />
                      <Button sm icon iconLeft="view-tex" pressed={shading === 'tex'} onClick={() => { setShading('tex'); setWire(false); }} />
                      <Button sm icon iconLeft="view-lit" pressed={shading === 'lit'} onClick={() => { setShading('lit'); setWire(false); }} />
                    </div>
                  </div>

                  {/* HUD */}
                  <div className="dcs-viewport__overlay dcs-viewport__overlay--tr">
                    <span className="dcs-badge dcs-badge--accent">PERSPECTIVE</span>
                    <span className="dcs-badge dcs-badge--soft">F 048 / 240</span>
                  </div>

                  {/* Right-side gizmos */}
                  <div className="dcs-viewport__overlay dcs-viewport__overlay--br">
                    <div style={{ fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums', fontSize: 10, color: 'rgba(255,255,255,.6)', textAlign: 'right', lineHeight: 1.5 }}>
                      <div>jane_body · 64,201 tri</div>
                      <div style={{ color: 'var(--dcs-accent)' }}>60 fps · 4.1 ms</div>
                    </div>
                  </div>

                  {/* Snap/pivot overlay — bottom-left */}
                  <div className="dcs-viewport__overlay dcs-viewport__overlay--bl">
                    <div className="dcs-viewport__floater">
                      <Button sm icon iconLeft="magnet" pressed />
                      <Button sm icon iconLeft="snap-grid" />
                      <Button sm icon iconLeft="gizmo" pressed />
                    </div>
                  </div>
                </div>
              </DockPane>

              <div className="dcs-splitter" />

              <div style={{ flex: '0 0 130px', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
                <DockPane
                  tabs={[
                    { value: 'console', label: 'Console', icon: 'cpu' },
                    { value: 'animation', label: 'Animation', icon: 'curve' },
                    { value: 'timeline', label: 'Timeline', icon: 'timeline' },
                  ]}
                  value={bottomTab}
                  onChange={setBottomTab}
                  tools={<>
                    <Button ghost sm icon iconLeft="trash" />
                    <Button ghost sm icon iconLeft="more-h" />
                  </>}
                >
                  {bottomTab === 'console' && (
                    <div style={{ padding: 'var(--dcs-s-3) var(--dcs-s-5)', fontFamily: 'var(--dcs-font-mono)', fontSize: 11, lineHeight: 1.6 }}>
                      <div style={{ color: 'var(--dcs-text-mute)' }}><span style={{ color: 'var(--dcs-ok)' }}>[ok]</span> Scene cached · 312 MB</div>
                      <div style={{ color: 'var(--dcs-text-mute)' }}><span style={{ color: 'var(--dcs-accent)' }}>[info]</span> 4 modifiers evaluated · 4.1 ms</div>
                      <div style={{ color: 'var(--dcs-text-mute)' }}><span style={{ color: 'var(--dcs-warn)' }}>[warn]</span> jane_body has 12 n-gons</div>
                      <div style={{ color: 'var(--dcs-text)' }}>
                        <span style={{ color: 'var(--dcs-accent)' }}>›</span>{' '}<span style={{ width: 6, height: 12, background: 'var(--dcs-accent)', display: 'inline-block', animation: 'dcs-blink 1s steps(1) infinite', verticalAlign: 'middle' }} />
                      </div>
                    </div>
                  )}
                  {bottomTab === 'animation' && (
                    <div style={{ padding: 8 }}>
                      <div style={{ height: 100, position: 'relative', background: 'var(--dcs-well)', border: '1px solid var(--dcs-line)', borderRadius: 3 }}>
                        {Array.from({ length: 13 }).map((_, i) => (
                          <div key={i} style={{ position: 'absolute', left: `${(i / 12) * 100}%`, top: 0, bottom: 0, width: 1, background: i % 4 === 0 ? 'rgba(255,255,255,.12)' : 'rgba(255,255,255,.04)' }} />
                        ))}
                        {[12, 24, 48, 64, 96, 128, 160].map(f => (
                          <div key={f} style={{ position: 'absolute', left: `${(f / 240) * 100}%`, top: '50%', width: 8, height: 8, marginLeft: -4, marginTop: -4, background: 'var(--dcs-accent)', transform: 'rotate(45deg)' }} />
                        ))}
                        <div style={{ position: 'absolute', left: '20%', top: 0, bottom: 0, width: 2, background: 'var(--dcs-accent-hi)' }} />
                      </div>
                    </div>
                  )}
                  {bottomTab === 'timeline' && (
                    <div style={{ padding: 'var(--dcs-s-5)', fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums', fontSize: 12, color: 'var(--dcs-text-dim)' }}>
                      Timeline tracks would render here…
                    </div>
                  )}
                </DockPane>
              </div>
            </div>

            <div className="dcs-splitter" />

            {/* Right dock — Inspector with foldouts */}
            <div style={{ flex: '0 0 260px', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
              <DockPane
                tabs={[
                  { value: 'inspector', label: 'Inspector', icon: 'cog' },
                  { value: 'lighting', label: 'Lighting', icon: 'light' },
                ]}
                value={rightTab}
                onChange={setRightTab}
                tools={<Button ghost sm icon iconLeft="pin" />}
              >
                {rightTab === 'inspector' ? (
                  <Foldouts>
                    <Foldout title="Transform" icon="move" tools={<Button ghost sm icon iconLeft="key" />}>
                      <div className="dcs-props">
                        {[['X', '#ef6b6b', pos.x, 'x'], ['Y', '#4ed18a', pos.y, 'y'], ['Z', '#4d9fff', pos.z, 'z']].map(([k, c, v, key]) => (
                          <div key={k} className="dcs-field">
                            <span className="dcs-field__label" style={{ flex: '0 0 16px', color: c, fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums', fontSize: 11 }}>{k}</span>
                            <Combo value={v} onChange={nv => setPos(p => ({ ...p, [key]: nv }))} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} />
                          </div>
                        ))}
                      </div>
                    </Foldout>
                    <Foldout title="Material" icon="palette" meta="Lambert.001">
                      <div className="dcs-props">
                        <div className="dcs-field">
                          <span className="dcs-field__label">Albedo</span>
                          <div className="dcs-swatch"><div className="dcs-swatch__chip" style={{ '--c': '#4d9fff' }} /><span>#4D9FFF</span></div>
                        </div>
                        <div className="dcs-field">
                          <span className="dcs-field__label">Rough</span>
                          <Slider value={rough} onChange={setRough} />
                        </div>
                        <div className="dcs-field">
                          <span className="dcs-field__label">Metallic</span>
                          <Slider value={metallic} onChange={setMetallic} />
                        </div>
                      </div>
                    </Foldout>
                    <Foldout title="Display" icon="eye">
                      <div className="dcs-props">
                        <div className="dcs-field"><span className="dcs-field__label">Wireframe</span><Switch checked={wire} onChange={setWire} /></div>
                        <div className="dcs-field"><span className="dcs-field__label">Cast shadow</span><Switch checked={shadow} onChange={setShadow} /></div>
                        <div className="dcs-field"><span className="dcs-field__label">In viewport</span><Switch checked onChange={() => {}} /></div>
                      </div>
                    </Foldout>
                    <Foldout title="Modifiers" icon="bolt" defaultOpen={false} meta="3" />
                    <Foldout title="Subdivision" icon="subdivide" defaultOpen={false} meta="L2" />
                    <Foldout title="UV Maps" icon="uv" defaultOpen={false} meta="1" />
                  </Foldouts>
                ) : (
                  <Foldouts>
                    <Foldout title="Ambient" icon="globe">
                      <div className="dcs-props">
                        <div className="dcs-field"><span className="dcs-field__label">Color</span><div className="dcs-swatch"><div className="dcs-swatch__chip" style={{ '--c': '#7c8492' }} /><span>#7C8492</span></div></div>
                        <div className="dcs-field"><span className="dcs-field__label">Intensity</span><Slider value={0.4} onChange={() => {}} /></div>
                      </div>
                    </Foldout>
                    <Foldout title="Sun" icon="light">
                      <div className="dcs-props">
                        <div className="dcs-field"><span className="dcs-field__label">Color</span><div className="dcs-swatch"><div className="dcs-swatch__chip" style={{ '--c': '#fff1d5' }} /><span>#FFF1D5</span></div></div>
                        <div className="dcs-field"><span className="dcs-field__label">Exposure</span><Slider value={0.6} onChange={() => {}} /></div>
                        <div className="dcs-field"><span className="dcs-field__label">Soft</span><Switch checked onChange={() => {}} /></div>
                      </div>
                    </Foldout>
                  </Foldouts>
                )}
              </DockPane>
            </div>
          </div>
        </div>
      </Demo>
    </section>
  );
}

Object.assign(window, { SectionPanels, SectionDock, SectionSubpanels, SectionFoldouts });

function SectionFoldouts() {
  return (
    <section className="dw-section" id="foldouts">
      <div className="dw-section__eyebrow">Layout · 04</div>
      <h2>Foldouts</h2>
      <p className="dw-section__lead">
        Blender-style soft section cards. Use these inside a scrolling property inspector to group
        related controls into rounded, lighter-bg containers with comfortable margins. Click a
        header to collapse. Pair them with subpanels — foldouts are the soft, breathy version.
      </p>
      <Demo frame="app">
        <div style={{ display: 'grid', gridTemplateColumns: '320px 1fr', gap: 16, alignItems: 'flex-start' }}>
          <Panel title="Inspector ▸ jane_body" icon="cog" pad={0}
                 tools={<><Button ghost sm icon iconLeft="pin" /><Button ghost sm icon iconLeft="more-h" /></>}>
            <div style={{ maxHeight: 420, overflowY: 'auto' }}>
              <Foldouts>
                <Foldout title="Transform" icon="move" meta="Local" tools={<Button ghost sm icon iconLeft="key" />}>
                  <div className="dcs-props">
                    {[['X', '#ef6b6b', 1.428], ['Y', '#4ed18a', -0.952], ['Z', '#4d9fff', 3.000]].map(([k, c, v]) => (
                      <div key={k} className="dcs-field">
                        <span className="dcs-field__label" style={{ color: c, flex: '0 0 16px', fontFamily: 'var(--dcs-font-num)', fontVariantNumeric: 'tabular-nums', fontSize: 11 }}>{k}</span>
                        <Combo value={v} onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} />
                      </div>
                    ))}
                  </div>
                </Foldout>
                <Foldout title="Material" icon="palette" meta="Lambert.001">
                  <div className="dcs-props">
                    <div className="dcs-field">
                      <span className="dcs-field__label">Albedo</span>
                      <div className="dcs-swatch"><div className="dcs-swatch__chip" style={{ '--c': '#4d9fff' }} /><span>#4D9FFF</span></div>
                    </div>
                    <div className="dcs-field">
                      <span className="dcs-field__label">Roughness</span>
                      <Slider value={0.42} onChange={() => {}} />
                    </div>
                    <div className="dcs-field">
                      <span className="dcs-field__label">Metallic</span>
                      <Slider value={0.08} onChange={() => {}} />
                    </div>
                    <div className="dcs-field">
                      <span className="dcs-field__label">Specular</span>
                      <Combo value={0.5} onChange={() => {}} min={0} max={1} step={0.01} format={v => v.toFixed(2)} />
                    </div>
                    <div className="dcs-field">
                      <span className="dcs-field__label">Use texture</span>
                      <Switch checked onChange={() => {}} />
                    </div>
                    <div className="dcs-field">
                      <span className="dcs-field__label">Tag</span>
                      <input className="dcs-input" defaultValue="opaque/metallic" />
                    </div>
                  </div>
                </Foldout>
                <Foldout title="Display" icon="eye">
                  <div className="dcs-props">
                    {[
                      { l: 'Wireframe', v: false },
                      { l: 'Smooth shading', v: true },
                      { l: 'Cast shadow', v: true },
                      { l: 'In viewport', v: true },
                      { l: 'In render', v: true },
                    ].map(it => (
                      <div key={it.l} className="dcs-field">
                        <span className="dcs-field__label">{it.l}</span>
                        <Switch checked={it.v} onChange={() => {}} />
                      </div>
                    ))}
                  </div>
                </Foldout>
                <Foldout title="Constraints" icon="link" defaultOpen={false} meta="0" />
                <Foldout title="Modifiers" icon="bolt" defaultOpen={false} meta="3" />
                <Foldout title="Subdivision" icon="subdivide" defaultOpen={false} meta="L2" />
                <Foldout title="UV Maps" icon="uv" defaultOpen={false} meta="1" />
                <Foldout title="Vertex Groups" icon="mesh" defaultOpen={false} meta="0" />
              </Foldouts>
            </div>
          </Panel>
          <div style={{ fontSize: 13, color: 'var(--dw-text-dim)', lineHeight: 1.65, paddingTop: 6 }}>
            <p><strong style={{ color: 'var(--dw-text)' }}>Foldout vs SubPanel.</strong> Both collapse. The visual difference is rhythm.</p>
            <p><strong style={{ color: 'var(--dw-text)' }}>Foldouts</strong> are soft, lighter-bg cards with margins — they feel like grouped properties. Use for inspectors with many heterogeneous sections (Blender's N-panel, Unity's Inspector).</p>
            <p><strong style={{ color: 'var(--dw-text)' }}>Subpanels</strong> are full-width seams in a stack — they feel like rigid rows. Use when controls are dense and visual chunking matters more than soft separation (Maya's Channel Box, Houdini's parameter editor).</p>
          </div>
        </div>
      </Demo>
    </section>
  );
}

function SectionSubpanels() {
  return (
    <section className="dw-section" id="subpanels">
      <div className="dw-section__eyebrow">Layout · 03</div>
      <h2>Subpanels</h2>
      <p className="dw-section__lead">
        Inside a panel, group properties into collapsible subpanels with a single chevron and a hover-revealed
        close affordance. Subpanels share the panel's chrome — they're horizontal seams in a stack, not
        nested boxes.
      </p>
      <Demo frame="app">
        <div style={{ maxWidth: 320 }}>
          <Panel title="Inspector ▸ jane_body" icon="cog" pad={0}
                 tools={<><Button ghost sm icon iconLeft="pin" /><Button ghost sm icon iconLeft="more-h" /></>}>
            <SubPanel title="Transform" icon="move">
              <div className="dcs-col" style={{ gap: 3 }}>
                <div className="dcs-field" style={{ gap: 6 }}>
                  <span style={{ fontSize: 10, color: 'var(--dcs-danger)', width: 12, fontFamily: 'var(--dcs-font-mono)' }}>X</span>
                  <Combo value={1.428} onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} width="100%" />
                </div>
                <div className="dcs-field" style={{ gap: 6 }}>
                  <span style={{ fontSize: 10, color: 'var(--dcs-ok)', width: 12, fontFamily: 'var(--dcs-font-mono)' }}>Y</span>
                  <Combo value={-0.952} onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} width="100%" />
                </div>
                <div className="dcs-field" style={{ gap: 6 }}>
                  <span style={{ fontSize: 10, color: 'var(--dcs-accent)', width: 12, fontFamily: 'var(--dcs-font-mono)' }}>Z</span>
                  <Combo value={3.000} onChange={() => {}} min={-10} max={10} step={0.001} format={v => v.toFixed(3)} width="100%" />
                </div>
              </div>
            </SubPanel>
            <SubPanel title="Display" icon="eye">
              <div className="dcs-col" style={{ gap: 4 }}>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}>
                  <span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Wireframe</span>
                  <Switch checked={false} onChange={() => {}} />
                </div>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}>
                  <span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Smooth shade</span>
                  <Switch checked onChange={() => {}} />
                </div>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}>
                  <span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Cast shadow</span>
                  <Switch checked onChange={() => {}} />
                </div>
              </div>
            </SubPanel>
            <SubPanel title="Material" icon="palette" defaultOpen={false}>
              <div className="dcs-col" style={{ gap: 6 }}>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}>
                  <span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Albedo</span>
                  <div className="dcs-swatch"><div className="dcs-swatch__chip" style={{ '--c': '#4d9fff' }} /><span>#4D9FFF</span></div>
                </div>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}>
                  <span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Roughness</span>
                  <Slider value={0.42} onChange={() => {}} />
                </div>
              </div>
            </SubPanel>
            <SubPanel title="Constraints" icon="link" defaultOpen={false}>
              <div className="dcs-col" style={{ gap: 4 }}>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}><span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Lock X</span><Switch checked={false} onChange={() => {}} /></div>
                <div className="dcs-field" style={{ justifyContent: 'space-between' }}><span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Track to</span><Combo value={0} onChange={() => {}} min={0} max={4} step={1} format={() => 'Cam.001'} width={90} /></div>
              </div>
            </SubPanel>
            <SubPanel title="Subdivision" icon="subdivide" defaultOpen={false}>
              <div className="dcs-field" style={{ justifyContent: 'space-between' }}>
                <span style={{ fontSize: 11, color: 'var(--dcs-text-dim)' }}>Level</span>
                <Combo value={2} onChange={() => {}} min={0} max={6} step={1} format={v => `${v}×`} width={70} />
              </div>
            </SubPanel>
          </Panel>
        </div>
      </Demo>
    </section>
  );
}
