/* sections-components.jsx
   Buttons · Inputs · Sliders · Knobs · Combo · Checks · Switches · Toolbar
*/
const { useState: useStateC } = React;

function SectionButtons() {
  const [pressed, setPressed] = useStateC({ snap: true, sym: false, prop: false });
  return (
    <section className="dw-section" id="buttons">
      <div className="dw-section__eyebrow">Components · 01</div>
      <h2>Buttons</h2>
      <p className="dw-section__lead">
        Five intents — default, primary, ghost, danger, toggle — across three sizes. All share the same bevel
        recipe so they stack on a toolbar without breaking rhythm.
      </p>

      <Demo caption="Intents">
        <div className="dcs-row" style={{ gap: 10, flexWrap: 'wrap' }}>
          <Button>Default</Button>
          <Button primary>Primary</Button>
          <Button ghost>Ghost</Button>
          <Button danger>Danger</Button>
          <Button disabled>Disabled</Button>
          <Button pressed>Pressed</Button>
        </div>
      </Demo>

      <Demo caption="With icons, three sizes">
        <div className="dcs-col">
          <div className="dcs-row" style={{ gap: 10 }}>
            <Button sm iconLeft="play">Render</Button>
            <Button sm icon iconLeft="save" />
            <Button sm ghost iconLeft="cog">Prefs</Button>
            <Button sm primary iconLeft="export">Export</Button>
          </div>
          <div className="dcs-row" style={{ gap: 10 }}>
            <Button iconLeft="play">Render</Button>
            <Button icon iconLeft="save" />
            <Button ghost iconLeft="cog">Prefs</Button>
            <Button primary iconLeft="export">Export</Button>
          </div>
          <div className="dcs-row" style={{ gap: 10 }}>
            <Button lg iconLeft="play">Render</Button>
            <Button lg icon iconLeft="save" />
            <Button lg ghost iconLeft="cog">Prefs</Button>
            <Button lg primary iconLeft="export">Export</Button>
          </div>
        </div>
      </Demo>

      <Demo caption="Segmented button group — for mutually exclusive modes">
        <div className="dcs-col">
          <ButtonGroup
            value={pressed.snap}
            onChange={v => setPressed(p => ({ ...p, snap: v }))}
            options={[
              { value: false, icon: 'cube', label: 'Vertex' },
              { value: true,  icon: 'array', label: 'Grid' },
              { value: 'edge', icon: 'spline', label: 'Edge' },
              { value: 'face', icon: 'plane', label: 'Face' },
            ]}
          />
          <ButtonGroup
            value={pressed.sym}
            onChange={v => setPressed(p => ({ ...p, sym: v }))}
            options={[
              { value: 'x', label: 'X' },
              { value: 'y', label: 'Y' },
              { value: 'z', label: 'Z' },
            ]}
          />
        </div>
      </Demo>

      <Demo caption="Tool-button strip — the spine of any DCC viewport">
        <Toolbar>
          <Button ghost sm icon iconLeft="select" pressed />
          <Button ghost sm icon iconLeft="move" />
          <Button ghost sm icon iconLeft="rotate" />
          <Button ghost sm icon iconLeft="scale" />
          <ToolbarSep />
          <Button ghost sm icon iconLeft="brush" />
          <Button ghost sm icon iconLeft="eraser" />
          <Button ghost sm icon iconLeft="fill" />
          <ToolbarSep />
          <Button ghost sm icon iconLeft="pivot" />
          <Button ghost sm icon iconLeft="magnet" pressed />
          <Button ghost sm icon iconLeft="snap" />
          <div style={{ flex: 1 }} />
          <Button ghost sm icon iconLeft="undo" />
          <Button ghost sm icon iconLeft="redo" />
        </Toolbar>
      </Demo>
    </section>
  );
}

function SectionInputs() {
  const [name, setName] = useStateC('untitled.dcs');
  const [num, setNum] = useStateC(440);
  return (
    <section className="dw-section" id="inputs">
      <div className="dw-section__eyebrow">Components · 02</div>
      <h2>Text &amp; number fields</h2>
      <p className="dw-section__lead">
        Sunken inputs with mono numerics by default. The well shadow signals "type here" without
        a placeholder label fight.
      </p>
      <Demo>
        <div className="dcs-col" style={{ maxWidth: 380, gap: 10 }}>
          <div className="dcs-field">
            <label className="dcs-field__label">Project</label>
            <input className="dcs-input" value={name} onChange={e => setName(e.target.value)} />
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Frequency</label>
            <input className="dcs-input dcs-input--num" value={num.toFixed(2)} onChange={e => setNum(parseFloat(e.target.value) || 0)} />
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Comment</label>
            <textarea className="dcs-textarea" rows={3} defaultValue="// safe to render — caches warm" />
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Mode</label>
            <select className="dcs-select" defaultValue="cyc">
              <option value="cyc">Cycles · Pathtraced</option>
              <option value="eev">Eevee · Realtime</option>
              <option value="wf">Wireframe only</option>
            </select>
          </div>
        </div>
      </Demo>
    </section>
  );
}

function SectionSliders() {
  const [vol, setVol] = useStateC(0.62);
  const [pan, setPan] = useStateC(-0.2);
  const [time, setTime] = useStateC(0.4);
  const [fader1, setFader1] = useStateC(0.75);
  const [fader2, setFader2] = useStateC(0.5);
  const [fader3, setFader3] = useStateC(0.3);
  const [fader4, setFader4] = useStateC(0.85);

  return (
    <section className="dw-section" id="sliders">
      <div className="dw-section__eyebrow">Components · 03</div>
      <h2>Sliders &amp; faders</h2>
      <p className="dw-section__lead">
        Horizontal sliders for parameters with continuous semantics. Bipolar variant for centered
        ranges. Vertical faders for synth-style mix surfaces.
      </p>

      <Demo caption="DCC-style horizontal slider">
        <div className="dcs-col" style={{ maxWidth: 420, gap: 10 }}>
          <div className="dcs-field">
            <label className="dcs-field__label">Roughness</label>
            <Slider value={vol} onChange={setVol} />
            <span className="dcs-mono" style={{ width: 50, textAlign: 'right' }}>{vol.toFixed(3)}</span>
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Pan</label>
            <Slider value={pan} onChange={setPan} min={-1} max={1} bipolar ticks={[-1, 0, 1]} />
            <span className="dcs-mono" style={{ width: 50, textAlign: 'right' }}>{pan >= 0 ? '+' : ''}{pan.toFixed(2)}</span>
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Timeline</label>
            <Slider value={time} onChange={setTime} ticks={Array.from({ length: 9 }, (_, i) => i/8)} />
            <span className="dcs-mono" style={{ width: 50, textAlign: 'right' }}>{Math.round(time * 240)}f</span>
          </div>
        </div>
      </Demo>

      <Demo caption="Synth fader column — drag vertically">
        <div className="dcs-row" style={{ alignItems: 'flex-end', gap: 24, padding: '12px 0' }}>
          {[
            { v: fader1, set: setFader1, l: 'KCK' },
            { v: fader2, set: setFader2, l: 'SNR' },
            { v: fader3, set: setFader3, l: 'HAT' },
            { v: fader4, set: setFader4, l: 'BUS' },
          ].map(f => (
            <div key={f.l} style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
              <span className="dcs-mono dcs-accent-text" style={{ fontSize: 11 }}>
                {Math.round((f.v - 0.75) * 60)} dB
              </span>
              <Fader value={f.v} onChange={f.set} />
              <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text-dim)', letterSpacing: '.08em' }}>{f.l}</span>
            </div>
          ))}
          <div style={{ width: 1, alignSelf: 'stretch', background: 'var(--dcs-line)', margin: '0 8px' }} />
          <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
            <span className="dcs-mono dcs-accent-text" style={{ fontSize: 11, color: 'var(--dcs-ok)' }}>
              -3 dB
            </span>
            <Fader value={0.92} onChange={() => {}} />
            <span className="dcs-mono" style={{ fontSize: 10, color: 'var(--dcs-text)', letterSpacing: '.08em' }}>MASTER</span>
          </div>
        </div>
      </Demo>
    </section>
  );
}

function SectionKnobs() {
  const [k, setK] = useStateC({ cutoff: 0.65, res: 0.3, drive: 0.2, env: 0.5, freq: 0.4, mix: 0.5, fb: 0.25 });
  const upd = (key, v) => setK(o => ({ ...o, [key]: v }));
  return (
    <section className="dw-section" id="knobs">
      <div className="dw-section__eyebrow">Components · 04</div>
      <h2>Knobs</h2>
      <p className="dw-section__lead">
        Synth-style rotary controls. Drag vertically to change; hold <code>Shift</code> for fine
        adjustment. The arc fills counter-clockwise from <code>−135°</code>, or radiates from
        center in bipolar mode.
      </p>

      <Demo caption="Three sizes · 40 / 56 / 72">
        <div className="dcs-row" style={{ gap: 36, padding: '16px 8px 12px', flexWrap: 'wrap' }}>
          <Knob value={k.cutoff} onChange={v => upd('cutoff', v)} size={40} label="CUTOFF" format={v => `${Math.round(v * 22050)}Hz`} />
          <Knob value={k.res} onChange={v => upd('res', v)} size={56} label="RESO" format={v => `${(v * 100).toFixed(0)}%`} />
          <Knob value={k.drive} onChange={v => upd('drive', v)} size={72} label="DRIVE" format={v => `${(v * 24).toFixed(1)}dB`} />
        </div>
      </Demo>

      <Demo caption="Bipolar — for ±N parameters">
        <div className="dcs-row" style={{ gap: 36, padding: '16px 8px 12px', flexWrap: 'wrap' }}>
          <Knob value={k.env}  onChange={v => upd('env', v)}  size={56} label="ENV ±" bipolar format={v => `${((v - 0.5) * 2).toFixed(2)}`} />
          <Knob value={k.freq} onChange={v => upd('freq', v)} size={56} label="DETUNE" bipolar format={v => `${((v - 0.5) * 100).toFixed(0)}ct`} />
          <Knob value={k.mix}  onChange={v => upd('mix', v)}  size={56} label="WET/DRY" bipolar format={v => v < 0.5 ? `DRY ${Math.round((0.5 - v) * 200)}` : `WET ${Math.round((v - 0.5) * 200)}`} />
          <Knob value={k.fb}   onChange={v => upd('fb', v)}   size={56} label="F/BACK" format={v => `${(v * 100).toFixed(0)}%`} />
        </div>
      </Demo>

      <Demo frame="app" caption="Knob rack — DSP signal flow legible at a glance">
        <Panel title="Filter ▸ Lowpass 24dB" icon="filter-lp">
          <div className="dcs-row" style={{ gap: 28, padding: '14px 4px 14px', flexWrap: 'wrap', justifyContent: 'space-around' }}>
            <Knob value={k.cutoff} onChange={v => upd('cutoff', v)} size={56} label="CUT" format={v => `${Math.round(v * 22050)}`} />
            <Knob value={k.res}  onChange={v => upd('res', v)}  size={56} label="RES"  format={v => `${(v*100).toFixed(0)}`} />
            <Knob value={k.drive} onChange={v => upd('drive', v)} size={56} label="DRIVE" format={v => `${(v*24).toFixed(1)}`} />
            <Knob value={k.env}  onChange={v => upd('env', v)}  size={56} label="ENV"  bipolar format={v => `${((v-0.5)*2).toFixed(2)}`} />
            <Knob value={k.mix}  onChange={v => upd('mix', v)}  size={56} label="LFO"  bipolar format={v => `${((v-0.5)*200).toFixed(0)}`} />
          </div>
        </Panel>
      </Demo>
    </section>
  );
}

function SectionCombo() {
  const [pos, setPos] = useStateC({ x: 1.428, y: -0.952, z: 3.0 });
  const [scale, setScale] = useStateC(1);
  const [rot, setRot] = useStateC(45);
  const [sub, setSub] = useStateC(3);
  return (
    <section className="dw-section" id="combo">
      <div className="dw-section__eyebrow">Components · 05</div>
      <h2>Combo number fields</h2>
      <p className="dw-section__lead">
        ZBrush-style hybrid controls. Drag horizontally to scrub the value; click without dragging
        to type a number directly; the chevrons step by a single unit. Hold <code>Shift</code>
        while dragging to coarsen, hold <code>Ctrl</code> for precision.
      </p>

      <Demo caption="A property inspector row — what 80% of a DCC inspector actually looks like">
        <div className="dcs-col" style={{ maxWidth: 480 }}>
          <div className="dcs-field">
            <label className="dcs-field__label">Position</label>
            <Combo value={pos.x} onChange={v => setPos(p => ({ ...p, x: v }))} min={-10} max={10} step={0.001} label="X" format={v => v.toFixed(3)} width={120} />
            <Combo value={pos.y} onChange={v => setPos(p => ({ ...p, y: v }))} min={-10} max={10} step={0.001} label="Y" format={v => v.toFixed(3)} width={120} />
            <Combo value={pos.z} onChange={v => setPos(p => ({ ...p, z: v }))} min={-10} max={10} step={0.001} label="Z" format={v => v.toFixed(3)} width={120} />
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Scale</label>
            <Combo value={scale} onChange={setScale} min={0.01} max={10} step={0.01} format={v => v.toFixed(2)} width={120} />
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Rotation</label>
            <Combo value={rot} onChange={setRot} min={-180} max={180} step={1} format={v => `${v.toFixed(0)}°`} width={120} />
          </div>
          <div className="dcs-field">
            <label className="dcs-field__label">Subdivision</label>
            <Combo value={sub} onChange={setSub} min={0} max={8} step={1} format={v => `${v.toFixed(0)}×`} width={120} />
          </div>
        </div>
      </Demo>
    </section>
  );
}

function SectionChecksSwitches() {
  const [c, setC] = useStateC({ a: true, b: false, c: true, s1: true, s2: false, s3: true, r: 'b' });
  return (
    <section className="dw-section" id="checks">
      <div className="dw-section__eyebrow">Components · 06</div>
      <h2>Checks, radios, switches</h2>
      <p className="dw-section__lead">
        Three discrete pickers for three contexts: checks for additive options, radios for exclusive
        choices, switches for live-applied bypasses.
      </p>
      <Demo>
        <div className="dcs-row" style={{ gap: 32 }}>
          <div className="dcs-col">
            <Check checked={c.a} onChange={v => setC(o => ({ ...o, a: v }))}>Cast shadows</Check>
            <Check checked={c.b} onChange={v => setC(o => ({ ...o, b: v }))}>Receive shadows</Check>
            <Check checked={c.c} onChange={v => setC(o => ({ ...o, c: v }))}>Ray-visible</Check>
          </div>
          <div className="dcs-col">
            <Check radio checked={c.r === 'a'} onChange={() => setC(o => ({ ...o, r: 'a' }))}>BVH</Check>
            <Check radio checked={c.r === 'b'} onChange={() => setC(o => ({ ...o, r: 'b' }))}>kd-Tree</Check>
            <Check radio checked={c.r === 'c'} onChange={() => setC(o => ({ ...o, r: 'c' }))}>Embree</Check>
          </div>
          <div className="dcs-col">
            <div className="dcs-row"><Switch checked={c.s1} onChange={v => setC(o => ({ ...o, s1: v }))} /><span style={{ fontSize: 12 }}>Auto-bake</span></div>
            <div className="dcs-row"><Switch checked={c.s2} onChange={v => setC(o => ({ ...o, s2: v }))} /><span style={{ fontSize: 12 }}>Cache to disk</span></div>
            <div className="dcs-row"><Switch checked={c.s3} onChange={v => setC(o => ({ ...o, s3: v }))} /><span style={{ fontSize: 12 }}>Watch live</span></div>
          </div>
        </div>
      </Demo>
    </section>
  );
}

Object.assign(window, {
  SectionButtons, SectionInputs, SectionSliders, SectionKnobs, SectionCombo, SectionChecksSwitches,
});
