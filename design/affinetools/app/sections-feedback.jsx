/* sections-feedback.jsx
   Alerts · Modals · Toasts
*/
const { useState: useStateFB } = React;

function SectionAlerts() {
  return (
    <section className="dw-section" id="alerts">
      <div className="dw-section__eyebrow">Feedback · 01</div>
      <h2>Alerts</h2>
      <p className="dw-section__lead">
        Inline messages with a 3px semantic border-left and a leading icon. Slot them into panel
        bodies, headers, or full-width docks — they keep their rhythm.
      </p>
      <Demo>
        <div className="dcs-col" style={{ gap: 10 }}>
          <div className="dcs-alert dcs-alert--ok">
            <div className="dcs-alert__icon"><Icon name="check-circle" /></div>
            <div className="dcs-alert__body">
              <div className="dcs-alert__title">Bake complete</div>
              <div className="dcs-alert__msg">240 frames · 4 h 12 m · cached to <code style={{ fontFamily: 'var(--dcs-font-mono)' }}>./cache/intro_v014</code></div>
            </div>
            <Button ghost sm icon iconLeft="close" />
          </div>
          <div className="dcs-alert">
            <div className="dcs-alert__icon"><Icon name="info" /></div>
            <div className="dcs-alert__body">
              <div className="dcs-alert__title">New version available</div>
              <div className="dcs-alert__msg">decius v0.5 "Tullus" introduces vector field knobs and node thumbnails.</div>
            </div>
          </div>
          <div className="dcs-alert dcs-alert--warn">
            <div className="dcs-alert__icon"><Icon name="alert" /></div>
            <div className="dcs-alert__body">
              <div className="dcs-alert__title">GPU clipping detected</div>
              <div className="dcs-alert__msg">2 channels exceeded headroom on frame 081. Lower exposure or enable tone-map clamp.</div>
            </div>
          </div>
          <div className="dcs-alert dcs-alert--danger">
            <div className="dcs-alert__icon"><Icon name="error" /></div>
            <div className="dcs-alert__body">
              <div className="dcs-alert__title">Plugin failed to load</div>
              <div className="dcs-alert__msg"><code style={{ fontFamily: 'var(--dcs-font-mono)' }}>libfx_volumetrics.so</code> — symbol <code style={{ fontFamily: 'var(--dcs-font-mono)' }}>fx_init_v3</code> not found.</div>
            </div>
          </div>
        </div>
      </Demo>
    </section>
  );
}

function SectionModals() {
  const [open, setOpen] = useStateFB(null);
  return (
    <section className="dw-section" id="modals">
      <div className="dw-section__eyebrow">Feedback · 02</div>
      <h2>Modals</h2>
      <p className="dw-section__lead">
        For destructive confirmations, large-form preferences, and "are you sure" gates. The
        backdrop blurs the world behind it; the modal lifts on a strong shadow.
      </p>

      <Demo>
        <div className="dcs-row" style={{ gap: 10 }}>
          <Button onClick={() => setOpen('confirm')} iconLeft="trash">Delete selected…</Button>
          <Button primary onClick={() => setOpen('save')} iconLeft="save">Save as…</Button>
          <Button ghost onClick={() => setOpen('prefs')} iconLeft="cog">Preferences…</Button>
        </div>
      </Demo>

      {open === 'confirm' && (
        <div className="dcs">
          <div className="dcs-modal-backdrop" onClick={() => setOpen(null)}>
            <div className="dcs-modal" style={{ width: 420 }} onClick={e => e.stopPropagation()}>
              <div className="dcs-modal__header"><Icon name="alert" style={{ color: 'var(--dcs-warn)' }} /><span>Delete 3 objects?</span></div>
              <div className="dcs-modal__body">
                <p style={{ margin: 0, color: 'var(--dcs-text-dim)' }}>This will remove <strong style={{ color: 'var(--dcs-text)' }}>Cube.003, Cube.004, Light.002</strong> from the scene. Linked instances stay intact.</p>
                <div style={{ marginTop: 12 }}>
                  <Check checked={false} onChange={() => {}}>Also delete from outliner collections</Check>
                </div>
              </div>
              <div className="dcs-modal__footer">
                <Button ghost onClick={() => setOpen(null)}>Cancel</Button>
                <Button danger iconLeft="trash" onClick={() => setOpen(null)}>Delete</Button>
              </div>
            </div>
          </div>
        </div>
      )}
      {open === 'save' && (
        <div className="dcs">
          <div className="dcs-modal-backdrop" onClick={() => setOpen(null)}>
            <div className="dcs-modal" style={{ width: 500 }} onClick={e => e.stopPropagation()}>
              <div className="dcs-modal__header"><Icon name="save" /><span>Save project as…</span></div>
              <div className="dcs-modal__body">
                <div className="dcs-col" style={{ gap: 10 }}>
                  <div className="dcs-field"><label className="dcs-field__label">Name</label><input className="dcs-input" defaultValue="Scene_Intro_v014.dcs" style={{ flex: 1 }} /></div>
                  <div className="dcs-field"><label className="dcs-field__label">Location</label><input className="dcs-input" defaultValue="~/work/intro/" style={{ flex: 1 }} /><Button sm ghost iconLeft="folder">Browse…</Button></div>
                  <div className="dcs-field"><label className="dcs-field__label">Format</label>
                    <select className="dcs-select" style={{ flex: 1 }}>
                      <option>.dcs · native (zip)</option>
                      <option>.usd · packaged</option>
                      <option>.fbx · export</option>
                    </select>
                  </div>
                  <div className="dcs-field"><label className="dcs-field__label">&nbsp;</label><Check checked onChange={() => {}}>Bundle textures</Check></div>
                  <div className="dcs-field"><label className="dcs-field__label">&nbsp;</label><Check checked={false} onChange={() => {}}>Snapshot to history</Check></div>
                </div>
              </div>
              <div className="dcs-modal__footer">
                <Button ghost onClick={() => setOpen(null)}>Cancel</Button>
                <Button primary iconLeft="save" onClick={() => setOpen(null)}>Save</Button>
              </div>
            </div>
          </div>
        </div>
      )}
      {open === 'prefs' && (
        <div className="dcs">
          <div className="dcs-modal-backdrop" onClick={() => setOpen(null)}>
            <div className="dcs-modal" style={{ width: 640 }} onClick={e => e.stopPropagation()}>
              <div className="dcs-modal__header"><Icon name="cog" /><span>Preferences</span><span style={{ flex: 1 }} /><Button ghost sm icon iconLeft="close" onClick={() => setOpen(null)} /></div>
              <div style={{ display: 'flex', minHeight: 320 }}>
                <div style={{ width: 160, borderRight: '1px solid var(--dcs-line)', background: 'var(--dcs-surface-1)' }}>
                  <div className="dcs-tree" style={{ padding: '6px 0' }}>
                    {['Interface', 'Theme', 'Viewport', 'Animation', 'Files', 'Performance', 'Shortcuts', 'Plugins'].map((n, i) => (
                      <div key={n} className="dcs-tree__row" aria-selected={i === 1} style={{ '--depth': 0, paddingLeft: 14 }}>
                        <span style={{ width: 14 }} />
                        <Icon name={['cog', 'palette', 'cube', 'curve', 'folder', 'cpu', 'bolt', 'rocket'][i]} className="dcs-tree__icon" />
                        <span className="dcs-tree__label">{n}</span>
                      </div>
                    ))}
                  </div>
                </div>
                <div style={{ flex: 1, padding: 18 }}>
                  <div style={{ fontSize: 14, fontWeight: 500, marginBottom: 12 }}>Theme</div>
                  <div className="dcs-col" style={{ gap: 8 }}>
                    <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 100 }}>Accent</label>
                      <ButtonGroup value="blue" onChange={() => {}} options={[
                        { value: 'blue', label: 'Blue' }, { value: 'cyan', label: 'Cyan' }, { value: 'orange', label: 'Orange' }, { value: 'violet', label: 'Violet' },
                      ]} />
                    </div>
                    <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 100 }}>Darkness</label>
                      <ButtonGroup value="cool" onChange={() => {}} options={[
                        { value: 'dark', label: 'Darker' }, { value: 'cool', label: 'Cool mid' }, { value: 'light', label: 'Lighter' },
                      ]} />
                    </div>
                    <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 100 }}>Density</label>
                      <ButtonGroup value="comf" onChange={() => {}} options={[
                        { value: 'cmp', label: 'Compact' }, { value: 'comf', label: 'Comfortable' }, { value: 'sp', label: 'Spacious' },
                      ]} />
                    </div>
                    <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 100 }}>Smooth UI</label><Switch checked onChange={() => {}} /></div>
                    <div className="dcs-field"><label className="dcs-field__label" style={{ minWidth: 100 }}>Hi-DPI line scale</label><Slider value={0.6} onChange={() => {}} /></div>
                  </div>
                </div>
              </div>
              <div className="dcs-modal__footer">
                <Button ghost onClick={() => setOpen(null)}>Close</Button>
                <Button primary onClick={() => setOpen(null)}>Apply</Button>
              </div>
            </div>
          </div>
        </div>
      )}
    </section>
  );
}

Object.assign(window, { SectionAlerts, SectionModals });
