(() => {
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const num = (v, places = 2) => {
    let s = Number(v).toFixed(places);
    while (s.length > 1 && s.endsWith("0")) s = s.slice(0, -1);
    if (s.endsWith(".")) s = s.slice(0, -1);
    return s;
  };
  const pct = (min, max, value) => {
    if (Math.abs(max - min) < 0.00001) return 0;
    return clamp((value - min) / (max - min), 0, 1) * 100;
  };
  const sliderFillStyle = (min, max, value, bipolar) => {
    const p = pct(min, max, value);
    if (bipolar) {
      const start = Math.min(50, p);
      const width = Math.abs(p - 50);
      return `left:${num(start)}%;right:auto;width:${num(width)}%`;
    }
    return `width:${num(p)}%`;
  };
  const sliderThumbStyle = (min, max, value) =>
    `left:${num(pct(min, max, value))}%`;
  const faderStyle = (value) =>
    `--pos:${num((1 - clamp(value, 0, 1)) * 100)}%`;
  const knobIndicatorAngle = (min, max, value) =>
    -135 + (pct(min, max, value) / 100) * 270;
  const knobRingPoint = (deg) => {
    const r = 10.5;
    const rad = (deg * Math.PI) / 180;
    return [12 + r * Math.cos(rad), 12 + r * Math.sin(rad)];
  };
  const knobArcPath = (min, max, value, bipolar) => {
    const p = pct(min, max, value) / 100;
    const sweep = bipolar ? (p - 0.5) * 270 : p * 270;
    if (Math.abs(sweep) <= 0.5) return "";
    const start = bipolar ? -90 : -225;
    const end = start + sweep;
    const [x0, y0] = knobRingPoint(start);
    const [x1, y1] = knobRingPoint(end);
    const large = Math.abs(sweep) > 180 ? 1 : 0;
    const sweepFlag = end >= start ? 1 : 0;
    return `M ${num(x0)} ${num(y0)} A 10.5 10.5 0 ${large} ${sweepFlag} ${num(x1)} ${num(y1)}`;
  };

  const attrFloat = (el, name, fallback) => {
    const v = parseFloat(el.getAttribute(name) || "");
    return Number.isFinite(v) ? v : fallback;
  };
  const setValue = (el, value) => {
    const min = attrFloat(el, "data-min", 0);
    const max = attrFloat(el, "data-max", 1);
    const v = clamp(value, min, max);
    el.setAttribute("data-value", num(v));
    if (el.hasAttribute("data-dcs-slider")) {
      const fill = document.getElementById(`${el.id}__fill`);
      const thumb = document.getElementById(`${el.id}__thumb`);
      const bipolar = el.hasAttribute("data-bipolar");
      if (fill) fill.setAttribute("style", sliderFillStyle(min, max, v, bipolar));
      if (thumb) thumb.setAttribute("style", sliderThumbStyle(min, max, v));
    } else if (el.hasAttribute("data-dcs-fader")) {
      el.setAttribute("style", faderStyle(v));
    } else if (el.hasAttribute("data-dcs-knob")) {
      const indicator = document.getElementById(`${el.id}__indicator`);
      const arc = document.getElementById(`${el.id}__arc`);
      const valueLabel = document.getElementById(`${el.id}__value`);
      const bipolar = el.hasAttribute("data-bipolar");
      if (indicator) indicator.setAttribute("style", `--angle:${num(knobIndicatorAngle(min, max, v))}deg`);
      if (arc) {
        const d = knobArcPath(min, max, v, bipolar);
        if (d) arc.setAttribute("d", d);
        else arc.removeAttribute("d");
      }
      if (valueLabel) valueLabel.textContent = num(v);
    }
  };

  const hydrateValues = () => {
    document
      .querySelectorAll("[data-dcs-slider], [data-dcs-fader], [data-dcs-knob]")
      .forEach((el) => {
        setValue(el, attrFloat(el, "data-value", attrFloat(el, "data-min", 0)));
      });
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", hydrateValues, { once: true });
  } else {
    hydrateValues();
  }

  document.addEventListener("click", (ev) => {
    const check = ev.target.closest(".dcs-check, .dcs-switch");
    if (!check) return;
    if (check.getAttribute("aria-checked") === "true") {
      check.removeAttribute("aria-checked");
    } else {
      check.setAttribute("aria-checked", "true");
    }
  });

  let drag = null;
  document.addEventListener("pointerdown", (ev) => {
    const el = ev.target.closest("[data-dcs-slider], [data-dcs-fader], [data-dcs-knob]");
    if (!el || !el.id) return;
    drag = {
      el,
      rect: el.getBoundingClientRect(),
      startY: ev.clientY,
      startValue: attrFloat(el, "data-value", attrFloat(el, "data-min", 0)),
      kind: el.hasAttribute("data-dcs-slider") ? "slider" :
            el.hasAttribute("data-dcs-fader") ? "fader" : "knob"
    };
    el.setPointerCapture(ev.pointerId);
    ev.preventDefault();
  });
  document.addEventListener("pointermove", (ev) => {
    if (!drag) return;
    const min = attrFloat(drag.el, "data-min", 0);
    const max = attrFloat(drag.el, "data-max", 1);
    if (drag.kind === "slider") {
      setValue(drag.el, min + clamp((ev.clientX - drag.rect.left) / drag.rect.width, 0, 1) * (max - min));
    } else if (drag.kind === "fader") {
      setValue(drag.el, min + (1 - clamp((ev.clientY - drag.rect.top) / drag.rect.height, 0, 1)) * (max - min));
    } else {
      setValue(drag.el, drag.startValue + ((drag.startY - ev.clientY) / 150) * (max - min));
    }
  });
  document.addEventListener("pointerup", () => {
    drag = null;
  });
})();
