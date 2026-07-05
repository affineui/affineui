/* dcs.jsx
   React wrappers for decius.css DCC components.
   These render the same class structure shown in the docs code blocks,
   making the live examples and the documented HTML interchangeable.
*/

const { useState, useRef, useEffect, useMemo, useCallback } = React;

/* ─────────── Panel ─────────── */
function Panel({ title, icon, tools, footer, raised, flat, bordered, closeable, onClose, headerActive, pad, children, style, className = '' }) {
  const [closed, setClosed] = useState(false);
  if (closed) return null;
  return (
    <div className={`dcs-panel${raised ? ' dcs-panel--raised' : ''}${bordered ? ' dcs-panel--bordered' : ''} ${className}`} style={style}>
      {title !== undefined && (
        <div className={`dcs-panel__header${headerActive ? ' dcs-panel__header--active' : ''}`}>
          <div className="dcs-panel__title">
            {icon && <Icon name={icon} />}
            <span>{title}</span>
          </div>
          {tools && <div className="dcs-panel__tools">{tools}</div>}
          {closeable && (
            <button
              className="dcs-panel__close"
              onClick={() => { if (onClose) onClose(); else setClosed(true); }}
              aria-label="Close panel"
            >
              <Icon name="close" size="sm" />
            </button>
          )}
        </div>
      )}
      <div className={`dcs-panel__body${pad === 0 ? ' dcs-panel__body--pad-0' : pad === 'sm' ? ' dcs-panel__body--pad-sm' : ''}`}>
        {children}
      </div>
      {footer && <div className="dcs-panel__footer">{footer}</div>}
    </div>
  );
}

/* ─────────── Button ─────────── */
function Button({ children, primary, ghost, danger, sm, lg, icon, iconLeft, iconRight, pressed, onClick, disabled, className = '', ...rest }) {
  const cls = [
    'dcs-btn',
    primary && 'dcs-btn--primary',
    ghost && 'dcs-btn--ghost',
    danger && 'dcs-btn--danger',
    sm && 'dcs-btn--sm',
    lg && 'dcs-btn--lg',
    icon && 'dcs-btn--icon',
    className,
  ].filter(Boolean).join(' ');
  return (
    <button
      className={cls}
      aria-pressed={pressed !== undefined ? pressed : undefined}
      aria-disabled={disabled || undefined}
      onClick={disabled ? undefined : onClick}
      {...rest}
    >
      {iconLeft && <Icon name={iconLeft} />}
      {children}
      {iconRight && <Icon name={iconRight} />}
    </button>
  );
}

function ButtonGroup({ value, onChange, options }) {
  return (
    <div className="dcs-btn-group" role="group">
      {options.map(opt => (
        <button
          key={opt.value}
          className="dcs-btn"
          aria-pressed={value === opt.value}
          onClick={() => onChange(opt.value)}
        >
          {opt.icon && <Icon name={opt.icon} />}
          {opt.label && <span>{opt.label}</span>}
        </button>
      ))}
    </div>
  );
}

/* ─────────── Drag helper ─────────── */
function useDrag(onDrag, opts = {}) {
  const ref = useRef({ active: false, startX: 0, startY: 0, startVal: 0 });
  return useCallback((e, startVal) => {
    e.preventDefault();
    const startX = e.clientX, startY = e.clientY;
    ref.current = { active: true, startX, startY, startVal };
    const move = (ev) => {
      const dx = ev.clientX - startX;
      const dy = ev.clientY - startY;
      onDrag({ dx, dy, startVal, shift: ev.shiftKey, ctrl: ev.ctrlKey || ev.metaKey });
    };
    const up = () => {
      ref.current.active = false;
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      if (opts.onEnd) opts.onEnd();
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }, [onDrag, opts.onEnd]);
}

/* ─────────── Slider (DCC horizontal) ─────────── */
function Slider({ value, onChange, min = 0, max = 1, step = 0, bipolar, width, ticks }) {
  const trackRef = useRef(null);
  const v = Math.max(min, Math.min(max, value));
  const pct = ((v - min) / (max - min)) * 100;
  const handleDown = (e) => {
    const rect = trackRef.current.getBoundingClientRect();
    const update = (clientX) => {
      const t = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
      let nv = min + t * (max - min);
      if (step) nv = Math.round(nv / step) * step;
      onChange(nv);
    };
    update(e.clientX);
    const move = ev => update(ev.clientX);
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  };
  let fillStyle;
  if (bipolar) {
    const center = ((-min) / (max - min)) * 100;
    if (v >= 0) fillStyle = { left: `${center}%`, width: `${pct - center}%` };
    else fillStyle = { right: `${100 - center}%`, left: 'auto', width: `${center - pct}%` };
  } else {
    fillStyle = { width: `${pct}%` };
  }
  return (
    <div className="dcs-slider" style={{ width: width || '100%' }} onPointerDown={handleDown}>
      <div className="dcs-slider__track" ref={trackRef}>
        <div className="dcs-slider__fill" style={fillStyle} />
        {ticks && ticks.map((t, i) => {
          const left = ((t - min) / (max - min)) * 100;
          return <div key={i} className="dcs-slider__tick" style={{ left: `${left}%` }} />;
        })}
        <div className="dcs-slider__thumb" style={{ left: `${pct}%` }} />
      </div>
    </div>
  );
}

/* ─────────── Fader (vertical, synth) ─────────── */
function Fader({ value, onChange, min = 0, max = 1, height = 140, label }) {
  const trackRef = useRef(null);
  const v = Math.max(min, Math.min(max, value));
  const pct = 100 - ((v - min) / (max - min)) * 100;
  const handleDown = (e) => {
    const rect = trackRef.current.getBoundingClientRect();
    const update = (clientY) => {
      const t = Math.max(0, Math.min(1, (clientY - rect.top) / rect.height));
      onChange(min + (1 - t) * (max - min));
    };
    update(e.clientY);
    const move = ev => update(ev.clientY);
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  };
  return (
    <div className="dcs-fader" style={{ height }} ref={trackRef} onPointerDown={handleDown}>
      <div className="dcs-fader__track" />
      {[0.25, 0.5, 0.75].map(t => <div key={t} className="dcs-fader__tick" style={{ top: `${t * 100}%` }} />)}
      <div className="dcs-fader__thumb" style={{ top: `${pct}%` }} />
    </div>
  );
}

/* ─────────── Knob ─────────── */
function Knob({ value, onChange, min = 0, max = 1, size = 56, label, bipolar, format }) {
  const v = Math.max(min, Math.min(max, value));
  const norm = (v - min) / (max - min);
  // Angle range: -135° to +135°
  const angle = -135 + norm * 270;
  const arcAngle = norm * 270;
  const startAngle = bipolar ? 0 : -135;
  const sweepDeg = bipolar ? (norm - 0.5) * 270 : arcAngle;

  const handleDown = (e) => {
    const startY = e.clientY;
    const startVal = v;
    const range = max - min;
    const move = (ev) => {
      const dy = startY - ev.clientY;
      const scale = ev.shiftKey ? 400 : 150;
      const delta = (dy / scale) * range;
      onChange(Math.max(min, Math.min(max, startVal + delta)));
    };
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  };

  // Arc track
  const cx = 12, cy = 12, r = 10.5;
  const arcStart = bipolar ? -90 : -225;
  const arcEnd = bipolar ? -90 + sweepDeg : -225 + arcAngle;
  const polar = (ang) => [cx + r * Math.cos(ang * Math.PI / 180), cy + r * Math.sin(ang * Math.PI / 180)];
  const [sx, sy] = polar(arcStart);
  const [ex, ey] = polar(arcEnd);
  const arcLen = Math.abs(arcEnd - arcStart);
  const largeArc = arcLen > 180 ? 1 : 0;
  const sweep = arcEnd >= arcStart ? 1 : 0;

  // Track ring (full sweep)
  const [tsx, tsy] = polar(-225);
  const [tex, tey] = polar(45);

  const formatted = format ? format(v) : v.toFixed(2);
  return (
    <div className="dcs-knob" style={{ '--knob-size': `${size}px` }} onPointerDown={handleDown}>
      <svg className="dcs-knob__ring" viewBox="0 0 24 24">
        <path d={`M ${tsx} ${tsy} A ${r} ${r} 0 1 1 ${tex} ${tey}`} fill="none" stroke="rgba(255,255,255,.08)" strokeWidth="1.5" strokeLinecap="round" />
        {Math.abs(sweepDeg) > 0.5 && (
          <path d={`M ${sx} ${sy} A ${r} ${r} 0 ${largeArc} ${sweep} ${ex} ${ey}`}
                fill="none" stroke="var(--dcs-accent)" strokeWidth="1.75" strokeLinecap="round" />
        )}
      </svg>
      <div className="dcs-knob__cap" />
      <div className="dcs-knob__indicator" style={{ '--angle': `${angle}deg` }} />
      {label && <div className="dcs-knob__label">{label}</div>}
      {label && <div className="dcs-knob__value">{formatted}</div>}
    </div>
  );
}

/* ─────────── Combo (ZBrush-style number/slider) ─────────── */
function Combo({ value, onChange, min = 0, max = 1, step = 0.01, label, format, width = 140, sm, lg }) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState('');
  const inputRef = useRef(null);
  const containerRef = useRef(null);
  const v = Math.max(min, Math.min(max, value));
  const pct = ((v - min) / (max - min)) * 100;

  useEffect(() => {
    if (editing && inputRef.current) {
      inputRef.current.focus();
      inputRef.current.select();
    }
  }, [editing]);

  const handleDown = (e) => {
    if (editing) return;
    if (e.target.classList.contains('dcs-combo__btn')) return;
    const rect = containerRef.current.getBoundingClientRect();
    const startX = e.clientX;
    let dragged = false;
    const range = max - min;
    const startVal = v;
    const move = (ev) => {
      const dx = ev.clientX - startX;
      if (Math.abs(dx) > 3) dragged = true;
      const scale = ev.shiftKey ? 4 : 1;
      const delta = (dx / rect.width) * range * scale;
      let nv = Math.max(min, Math.min(max, startVal + delta));
      if (step) nv = Math.round(nv / step) * step;
      onChange(nv);
    };
    const up = (ev) => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      if (!dragged) {
        setDraft(format ? format(v) : v.toString());
        setEditing(true);
      }
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  };

  const commit = () => {
    const parsed = parseFloat(draft);
    if (!Number.isNaN(parsed)) onChange(Math.max(min, Math.min(max, parsed)));
    setEditing(false);
  };

  const cls = ['dcs-combo', editing && 'dcs-combo--editing', sm && 'dcs-combo--sm', lg && 'dcs-combo--lg'].filter(Boolean).join(' ');
  return (
    <div ref={containerRef}
         className={cls}
         style={{ width, '--fill': `${pct}%` }}
         onPointerDown={handleDown}>
      <div className="dcs-combo__fill" />
      <div className="dcs-combo__btn" onPointerDown={(e) => { e.stopPropagation(); onChange(Math.max(min, v - (step || 0.01))); }}>
        <Icon name="chevron-left" size="sm" />
      </div>
      {label && <div className="dcs-combo__label">{label}</div>}
      <div className="dcs-combo__value">{format ? format(v) : v.toFixed(2)}</div>
      <div className="dcs-combo__btn" onPointerDown={(e) => { e.stopPropagation(); onChange(Math.min(max, v + (step || 0.01))); }}>
        <Icon name="chevron-right" size="sm" />
      </div>
      {editing && (
        <input
          ref={inputRef}
          className="dcs-combo__edit"
          value={draft}
          onChange={e => setDraft(e.target.value)}
          onBlur={commit}
          onKeyDown={e => {
            if (e.key === 'Enter') commit();
            else if (e.key === 'Escape') setEditing(false);
          }}
        />
      )}
    </div>
  );
}

/* ─────────── Check / Radio / Switch ─────────── */
function Check({ checked, onChange, children, radio }) {
  return (
    <div className={`dcs-check${radio ? ' dcs-radio' : ''}`} role={radio ? 'radio' : 'checkbox'} aria-checked={checked} onClick={() => onChange(!checked)}>
      <div className="dcs-check__box">
        {!radio && checked && <Icon name="check" />}
      </div>
      <span>{children}</span>
    </div>
  );
}

function Switch({ checked, onChange }) {
  return <div className="dcs-switch" role="switch" aria-checked={checked} onClick={() => onChange(!checked)} />;
}

/* ─────────── Tab bar ─────────── */
function Tabs({ tabs, value, onChange }) {
  return (
    <div className="dcs-tabs" role="tablist">
      {tabs.map(t => (
        <button key={t.value}
                className="dcs-tab"
                role="tab"
                aria-selected={value === t.value}
                onClick={() => onChange(t.value)}>
          {t.icon && <Icon name={t.icon} />}
          {t.label}
        </button>
      ))}
    </div>
  );
}

/* ─────────── Toolbar ─────────── */
function Toolbar({ children, vertical }) {
  return <div className={`dcs-toolbar${vertical ? ' dcs-toolbar--v' : ''}`}>{children}</div>;
}
function ToolbarSep() { return <div className="dcs-toolbar__sep" />; }

/* ─────────── Tree ─────────── */
function Tree({ nodes, selected, onSelect, expanded, onExpand }) {
  const rows = [];
  const walk = (items, depth) => {
    items.forEach(node => {
      const isOpen = expanded.has(node.id);
      const hasChildren = node.children && node.children.length;
      rows.push(
        <div
          key={node.id}
          className="dcs-tree__row"
          style={{ '--depth': depth }}
          aria-selected={selected === node.id}
          onClick={() => {
            onSelect(node.id);
            if (hasChildren) onExpand(node.id);
          }}
        >
          <div
            className={`dcs-tree__chevron${isOpen ? ' dcs-tree__chevron--open' : ''}`}
            onClick={(e) => { e.stopPropagation(); if (hasChildren) onExpand(node.id); }}
          >
            {hasChildren && <Icon name="chevron-right" size="sm" />}
          </div>
          {node.icon && <Icon className="dcs-tree__icon" name={node.icon} />}
          <span className="dcs-tree__label">{node.label}</span>
          {node.meta && <span className="dcs-tree__meta">{node.meta}</span>}
          {node.actions && <div className="dcs-tree__actions">{node.actions}</div>}
        </div>
      );
      if (hasChildren && isOpen) walk(node.children, depth + 1);
    });
  };
  walk(nodes, 0);
  return <div className="dcs-tree">{rows}</div>;
}

/* ─────────── Color swatch / picker (simple) ─────────── */
function Swatch({ color, label }) {
  return (
    <div className="dcs-swatch">
      <div className="dcs-swatch__chip" style={{ '--c': color }} />
      <span>{label || color}</span>
    </div>
  );
}

/* ─────────── Foldout — Blender-style soft section ─────────── */
function Foldout({ title, icon, meta, tools, defaultOpen = true, children, accent }) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <div className={`dcs-foldout${!open ? ' dcs-foldout--collapsed' : ''}${accent ? ' dcs-foldout--accent' : ''}`}>
      <div className="dcs-foldout__header" onClick={() => setOpen(o => !o)}>
        <div className={`dcs-foldout__chevron${open ? ' dcs-foldout__chevron--open' : ''}`}>
          <Icon name="chevron-right" size="sm" />
        </div>
        {icon && <Icon className="dcs-foldout__icon" name={icon} size="sm" />}
        <div className="dcs-foldout__title">{title}</div>
        {meta && <div className="dcs-foldout__meta">{meta}</div>}
        {tools && <div className="dcs-foldout__tools" onClick={(e) => e.stopPropagation()}>{tools}</div>}
      </div>
      <div className="dcs-foldout__body">{children}</div>
    </div>
  );
}

function Foldouts({ children, style }) {
  return <div className="dcs-foldouts" style={style}>{children}</div>;
}

/* ─────────── SubPanel ─────────── */
function SubPanel({ title, icon, onClose, defaultOpen = true, children, tools }) {
  const [open, setOpen] = useState(defaultOpen);
  const [closed, setClosed] = useState(false);
  if (closed) return null;
  return (
    <div className={`dcs-subpanel${!open ? ' dcs-subpanel--collapsed' : ''}`}>
      <div className="dcs-subpanel__header" onClick={() => setOpen(o => !o)}>
        <div className={`dcs-subpanel__chevron${open ? ' dcs-subpanel__chevron--open' : ''}`}>
          <Icon name="chevron-right" size="sm" />
        </div>
        {icon && <Icon name={icon} size="sm" style={{ width: 12, height: 12 }} />}
        <div className="dcs-subpanel__title">{title}</div>
        {tools && <div onClick={(e) => e.stopPropagation()}>{tools}</div>}
        <div className="dcs-subpanel__close" onClick={(e) => { e.stopPropagation(); if (onClose) onClose(); else setClosed(true); }}>
          <Icon name="close" size="sm" />
        </div>
      </div>
      <div className="dcs-subpanel__body">{children}</div>
    </div>
  );
}

/* ─────────── DockPane — tabbed dockable view ─────────── */
function DockPane({ tabs, value, onChange, onClose, tools, children }) {
  const [closed, setClosed] = useState(new Set());
  const visible = tabs.filter(t => !closed.has(t.value));
  if (!visible.length) return null;
  return (
    <div className="dcs-dockpane">
      <div className="dcs-dockpane__tabs">
        {visible.map(t => (
          <div
            key={t.value}
            className="dcs-dockpane__tab"
            aria-selected={value === t.value}
            onClick={() => onChange(t.value)}
          >
            {t.icon && <Icon name={t.icon} size="sm" />}
            <span>{t.label}</span>
            {(t.closeable !== false) && (
              <div
                className="dcs-dockpane__tab-close"
                onClick={(e) => {
                  e.stopPropagation();
                  if (onClose) onClose(t.value);
                  else {
                    const n = new Set(closed); n.add(t.value); setClosed(n);
                    if (value === t.value && visible.length > 1) {
                      const next = visible.find(x => x.value !== t.value);
                      if (next) onChange(next.value);
                    }
                  }
                }}
              >
                <Icon name="close" size="sm" />
              </div>
            )}
          </div>
        ))}
        {tools && <div className="dcs-dockpane__tools">{tools}</div>}
      </div>
      <div className="dcs-dockpane__body">{children}</div>
    </div>
  );
}

/* ─────────── MenuBar ─────────── */
function MenuBar({ brand, items, meta }) {
  return (
    <div className="dcs-menubar">
      {brand && (
        <div className="dcs-menubar__brand">
          {brand.icon && <Icon name={brand.icon} />}
          <span>{brand.label}</span>
        </div>
      )}
      {items && items.map(m => (
        <button key={m} className="dcs-menubar__item">{m}</button>
      ))}
      <div className="dcs-menubar__spacer" />
      {meta && <div className="dcs-menubar__meta">{meta}</div>}
    </div>
  );
}

/* ─────────── Card ─────────── */
function Card({
  title, meta, desc, media, badges, footer, tools, closeable, onClose,
  selected, onClick, horizontal, className = '', style, children,
}) {
  const [closed, setClosed] = useState(false);
  if (closed) return null;
  const cls = [
    'dcs-card',
    horizontal && 'dcs-card--horizontal',
    onClick && 'dcs-card--clickable',
    className,
  ].filter(Boolean).join(' ');
  return (
    <div
      className={cls}
      aria-selected={selected || undefined}
      onClick={onClick}
      style={style}
    >
      {badges && <div className="dcs-card__badges">{badges}</div>}
      {closeable && (
        <div
          className="dcs-card__close"
          onClick={(e) => { e.stopPropagation(); if (onClose) onClose(); else setClosed(true); }}
        >
          <Icon name="close" />
        </div>
      )}
      {media && <div className="dcs-card__media">{media}</div>}
      {(title || meta || desc || children) && (
        <div className="dcs-card__body">
          {title && <div className="dcs-card__title">{title}</div>}
          {meta && <div className="dcs-card__meta">{meta}</div>}
          {desc && <div className="dcs-card__desc">{desc}</div>}
          {children}
        </div>
      )}
      {tools && <div className="dcs-card__tools">{tools}</div>}
      {footer && <div className="dcs-card__footer">{footer}</div>}
    </div>
  );
}

function CardGrid({ children, style }) {
  return <div className="dcs-card-grid" style={style}>{children}</div>;
}
function CardList({ children, style }) {
  return <div className="dcs-card-list" style={style}>{children}</div>;
}

Object.assign(window, {
  Panel, SubPanel, Foldout, Foldouts, Button, ButtonGroup, Slider, Fader, Knob, Combo,
  Check, Switch, Tabs, Toolbar, ToolbarSep, Tree, Swatch, DockPane, MenuBar,
  Card, CardGrid, CardList,
});
