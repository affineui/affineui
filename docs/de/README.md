<p align="center">
  <sub>
    <b>Sprache:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**Ein kleiner, HTML5-konformer, GPU-beschleunigter UI-Renderer mit integriertem
Komponenten-Framework — ein nativer Ersatz für Electron und Qt.**

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI beim Ausführen des 3D-Druck-Slicers Dender">

*Dender — ein 3D-Druck-Slicer, der den standardmäßigen Decius-CSS-Look von AffineUI zeigt. Andockbare Panels, ein eigener Viewport und ein komplettes App-Layout, alles nativ gerendert.*

AffineUI liefert eine echte HTML/CSS-Layout- und Paint-Engine im Browser-Stil
als Zwei-Datei-C++-Drop-in, als Python-Bibliothek (im Gradio-Stil), als Rust-Crate
und als C# NuGet-Package. Ein Renderer, eine Komponenten-API, vier Host-Sprachen.
Sie läuft mit 120 Hz, animiert flüssig und bettet weder einen Browser, noch eine
JavaScript-VM oder ein großes Framework ein.

Sie ist gebaut für:

- **Game-Tools und In-Game-UI** — Launcher, HUDs, Einstellungsbildschirme, Debug-
  Panels, Editor-Overlays.
- **DCC-artige Content-Tools** — Anwendungen der Klasse Maya / Blender / ZBrush /
  DaVinci Pro mit dichten nativen Oberflächen.
- **Qt-/Electron-Ersatz** — wenn Sie das HTML/CSS-Authoring-Modell wollen, aber
  nicht den Browser oder den aufgeblähten Runtime.
- **Jeden, der eine kleine, plattformübergreifende native Benutzeroberfläche
  ausliefert**, die designt aussehen soll — nicht zusammengeschustert.

**Es lässt sich in ein bestehendes Projekt einbinden und funktioniert einfach.**
Wenn Sie bereits eine SDL2- oder sokol_app-Anwendung haben, sind zum Hinzufügen
von AffineUI zwei Dateien und ein Verdrahtungsaufruf nötig — kein Fork des
Build-Systems, kein Runtime-Download, kein separates Fenster. Einbinden, laden,
rendern, fertig.

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="Skeuomorphe Synth-Demo, gebaut mit AffineUI">

*Eine skeuomorphe modulare Synth-Demo — alles auf dem Bildschirm ist standardmäßiges HTML + CSS: Regler, Kabel, Panel-Texturen, Animationen. Kein eigenes Widget-Toolkit, keine Plugins.*

---

## Was AffineUI *nicht* ist

- **Kein Webbrowser.** Keine Navigation, keine Cookies, kein
  `fetch` / `XMLHttpRequest`, kein Fenster-Management. Das Ziel ist es, so
  klein und schnell zu sein, wie native UI nur sein kann — jedes Feature, das
  es in Richtung „Browser“ wachsen lassen würde, ist bewusst außerhalb des
  Scopes.
- **Kein abgespecktes HTML5.** AffineUI zielt auf *echte* HTML5-Abdeckung ab,
  nicht auf eine minimale Teilmenge. Wenn ein nicht-esoterisches HTML5- oder
  CSS-Feature, auf das sich Frameworks der Klasse Bootstrap, Tailwind oder
  Ant verlassen, nicht korrekt gerendert wird, behandeln Sie es als Bug und
  melden Sie ihn. Die Lücken sind Unvollständigkeit, keine Absicht.
- **Keine Sicherheits-Sandbox.** AffineUI ist dafür gedacht, *Ihr* HTML und
  *Ihr* CSS zu rendern. Geben Sie ihm **niemals** nicht vertrauenswürdiges
  Markup, Stylesheets oder Skripte, die aus dem offenen Web heruntergeladen
  wurden. Es gibt kein Origin-Modell, keine CSP, keine Isolation zwischen
  der UI und dem Host-Prozess.
- **Kein Alles-inklusive-Framework.** AffineUI rendert UI. Es macht keine
  `<video>`-Dekodierung, keine GPU-getriebene Animation beliebiger Szenen,
  kein 3D, kein Audio, kein Networking, kein Asset-Management. Das ist Sache
  Ihrer Anwendung — AffineUI ist das, worauf Sie sie *ausrichten*.
- **(Noch) keine JS-basierte native Web-Runtime.** Standardmäßig führt
  AffineUI kein JavaScript aus — das ist eine bewusste Entscheidung für den
  aktuellen Release, um die Story einfach und das Binary klein zu halten.
  JS- + React-Support ist auf der Roadmap und wird als Opt-in-Erweiterung
  landen, wodurch AffineUI ein erstklassiger Electron-Ersatz für Teams wird,
  die ihre Web-App-Codebasis behalten wollen.

---

## Status

**Alpha.** Der Kern-Renderer, das Layout, die Cascade und der Reconciler sind
heute für echte UIs nutzbar — Bootstrap-Dashboards, eigene DCC-Layouts und
komplette Tool-UIs werden alle gerendert. Die Standards-Abdeckung ist breit,
aber unvollständig, es gibt Edge Cases, und einige CSS-Features landen noch.
Rechnen Sie mit Bugs. Rechnen Sie damit, Bugs zu melden. Liefern Sie es noch
nicht an Kunden aus.

---

## Installation

Wählen Sie Ihre Sprache. Alle vier Bindings sitzen auf demselben C++-Kern.

**Python** — wie Gradio, aber nativ:

```bash
pip install affineui
```

**Rust:**

```bash
cargo add affineui
```

**C#:**

```bash
dotnet add package AffineUI
```

**C++ (Drop-in, keine Abhängigkeiten):** Holen Sie sich die zwei Dateien
[`dist/affineui.h`](../../dist/affineui.h) und [`dist/affineui.cpp`](../../dist/affineui.cpp),
fügen Sie sie Ihrem Projekt hinzu und kompilieren Sie `affineui.cpp` einmal als C++20.
Das ist das gesamte SDK — kein Paketmanager, kein Submodul-Baum, keine DLL.

Unterstützte Plattformen: Windows, macOS, Linux, iOS, Android, WebGL. Siehe
[docs/BUILDING.md](../BUILDING.md) für plattformspezifische Hinweise und
Voraussetzungen zum Bauen aus den Quellen.

---

## Hallo Welt

In jeder Sprache werden zwei Varianten ausgeliefert:

- **Komponenten-API** — Beschreibt die UI als Baum getypter Widgets
  (`heading`, `button`, `slider`, …). Reconciler-getrieben; Zustandsänderungen
  erfolgen an Ort und Stelle. Das ist die API, die Sie tatsächlich wollen.
- **Rohes HTML** — Übergibt der Engine einen HTML-String. Das ist der
  Drop-in-Pfad für bestehende Apps und für alles, was Sie lieber als Markup
  schreiben möchten.

### Python

Komponenten-API:

```python
import affineui as ui

view = ui.View(ui.ViewTheme.Decius)
view.begin()
view.heading(1, "Hello from Python")
view.paragraph("AffineUI — native HTML/CSS, no browser, no JS.")
view.button("Click me", key="go").on_click(lambda: print("clicked!"))
view.end()

app = ui.App(title="Hello", width=720, height=480)
app.load_view(view)
raise SystemExit(app.run())
```

Rohes HTML:

```python
import affineui as ui

app = ui.App(title="Hello", width=720, height=480)
app.load_html("""
  <main style="padding: 24px; font-family: sans-serif;">
    <h1>Hello from Python</h1>
    <p>AffineUI is alive.</p>
    <button>Click me</button>
  </main>
""")
raise SystemExit(app.run())
```

### Rust

Komponenten-API:

```rust
use affineui::{App, Config, Theme, View};

fn main() {
    let view = View::new(Theme::Decius);
    view.build(|v| {
        v.heading(1, "Hello from Rust", "", "");
        v.paragraph("AffineUI — native HTML/CSS, no browser, no JS.", "", "");
        v.button("Click me", true, "go").on_click(|| println!("clicked!"));
    });

    let app = App::new(Config::default().title("Hello").size(720, 480));
    app.load_view(&view);
    std::process::exit(app.run());
}
```

Rohes HTML:

```rust
use affineui::{App, Config};

fn main() {
    let app = App::new(Config::default().title("Hello").size(720, 480));
    app.load_html(r#"
      <main style="padding: 24px; font-family: sans-serif;">
        <h1>Hello from Rust</h1>
        <p>AffineUI is alive.</p>
        <button>Click me</button>
      </main>
    "#);
    std::process::exit(app.run());
}
```

### C#

Komponenten-API:

```csharp
using AffineUI;

using var view = new View(Theme.Decius);
view.Build(v =>
{
    v.Heading(1, "Hello from C#");
    v.Paragraph("AffineUI — native HTML/CSS, no browser, no JS.");
    v.Button("Click me", key: "go").OnClick(() => Console.WriteLine("clicked!"));
});

using var app = new App(new AppConfig { Title = "Hello", Width = 720, Height = 480 });
app.LoadView(view);
return app.Run();
```

Rohes HTML:

```csharp
using AffineUI;

using var app = new App(new AppConfig { Title = "Hello", Width = 720, Height = 480 });
app.LoadHtml("""
  <main style="padding: 24px; font-family: sans-serif;">
    <h1>Hello from C#</h1>
    <p>AffineUI is alive.</p>
    <button>Click me</button>
  </main>
""");
return app.Run();
```

### C++

Komponenten-API (unter Verwendung des amalgamierten Drop-in):

```cpp
#include "affineui.h"

int main() {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    view.heading(1, "Hello from C++");
    view.paragraph("AffineUI — native HTML/CSS, no browser, no JS.");
    view.button("Click me", /*primary=*/false, "go")
        .on_click([] { std::puts("clicked!"); });
    view.end();

    affineui::App::Config cfg;
    cfg.title  = "Hello";
    cfg.width  = 720;
    cfg.height = 480;
    affineui::App app{cfg};
    app.load_view(view);
    return app.run();
}
```

Rohes HTML:

```cpp
#include "affineui.h"

int main() {
    affineui::App::Config cfg;
    cfg.title  = "Hello";
    cfg.width  = 720;
    cfg.height = 480;
    affineui::App app{cfg};
    app.load_html(R"(
      <main style="padding: 24px; font-family: sans-serif;">
        <h1>Hello from C++</h1>
        <p>AffineUI is alive.</p>
        <button>Click me</button>
      </main>
    )");
    return app.run();
}
```

---

## Eine bescheidene App

Ein bisschen mehr als Hello World — ein Zähler, der sich live aktualisiert und
die Komponenten-API verwendet. Der Zustand lebt in der Host-Sprache; der
Reconciler vergleicht Ihre View mit dem letzten Frame und patcht das DOM.
CSS-Hover/Focus/Animation läuft zwischen den Updates weiter.

Python:

```python
import affineui as ui

count = 0
app = ui.App(title="Counter", width=480, height=240)

def build_view() -> ui.View:
    view = ui.View(ui.ViewTheme.Decius)
    view.begin()
    view.heading(1, f"Count: {count}")
    view.button("Increment", key="inc").on_click(bump)
    view.button("Reset",     key="reset").on_click(reset)
    view.end()
    return view

def bump():
    global count
    count += 1
    app.load_view(build_view())

def reset():
    global count
    count = 0
    app.load_view(build_view())

app.load_view(build_view())
raise SystemExit(app.run())
```

Die Rust-, C#- und C++-Äquivalente sind eins zu eins — dieselben Methodennamen,
dasselbe Reconciler-Verhalten. Siehe [`examples/`](../../examples/) für größere
End-to-End-Programme (ein vollständiger Foto-Editor, ein Game-Editor, ein
modularer Synth, ein 3D-Druck-Slicer), die mit derselben API gebaut sind.

---

## HTML, CSS und Design-Systeme

AffineUI ist ein echter HTML5-/CSS-Renderer, nicht ein Layout-Solver in
HTML-Form. HTML-Tokenisierung und DOM kommen von [lexbor](https://github.com/lexbor/lexbor);
die Flexbox-Mathematik kommt von [Yoga](https://github.com/facebook/yoga);
Paint läuft über [NanoVG](https://github.com/memononen/nanovg) in
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU).
Cascade, Computed Style, Hit-Testing, Selector-Routing und der Reconciler
sind unsere.

### Decius CSS ist der Standard

Das mitgelieferte Standard-Framework ist [Decius CSS](https://deciuscss.com) —
ein modernes Komponenten-Framework, das parallel zu AffineUI entwickelt und
bewusst so abgestimmt wurde, dass es auf dieser Engine pixelgenau rendert.
Wenn Sie die Komponenten-API ohne die Angabe eines Stylesheets verwenden,
bekommen Sie Decius, und es funktioniert einfach.

### Eigenes CSS mitbringen

Decius ist der Standard, **keine Voraussetzung**. Jedes CSS, dessen Selektoren
auf die Klassennamen passen, die AffineUI ausgibt, wird die eingebauten
Komponenten stylen, und jedes handgeschriebene HTML, das Sie über den Rohpfad
laden, kann jedes gewünschte CSS mitbringen. Bootstrap 4.6, Tailwind-artige
Utility-Klassen und Ant-artiges Komponenten-Markup werden alle sofort
gerendert — siehe
[`examples/01_bootstrap`](../../examples/01_bootstrap) und
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI rendert Bootstrap CSS">

*Die unveränderte Bootstrap-4.6-CSS-Bibliothek, nativ gerendert — Cards, Navbars, Buttons und Hover-/Active-States, direkt aus der echten `.min.css`.*

### Framework-JavaScript

Es gibt keine JS-Engine (siehe [Was AffineUI *nicht* ist](#was-affineui-nicht-ist)).
Framework-Interaktivität — Bootstraps Dropdowns, Ants Modale und so weiter —
wird stattdessen auf natives C++-Verhalten abgebildet, statt als Skript
ausgeführt zu werden.

---

## Einbetten in eine bestehende App

Wenn Sie bereits ein Fenster und eine Frame-Schleife haben, wird AffineUI über
einen Adapter zur Kompilierzeit verdrahtet. Zwei sind eingebaut — SDL2 und
sokol_app — und für alles andere gibt es einen manuellen Pfad.

**SDL2:**

```cpp
#define AFFINEUI_WITH_SDL
#include "affineui.h"

affineui::Ui ui;
ui.load("Hello.html");

while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (affineui::sdl::dispatch(ui, ev)) continue;
        // your event handling
    }
    // your rendering
    affineui::sdl::render(ui, window);
    SDL_GL_SwapWindow(window);
}
```

**sokol_app:**

```cpp
#define AFFINEUI_WITH_SOKOL
#include "affineui.h"

int main() {
    affineui::Ui ui;
    ui.load("Hello.html");
    ui.on_click("#quit", []{ sapp_request_quit(); });

    sapp_desc desc{};
    desc.width = 1024; desc.height = 768;
    desc.window_title = "My Game";
    desc.high_dpi = true;
    affineui::sokol::wire(desc, ui);   // installs frame + event callbacks
    sapp_run(&desc);
}
```

**Manuell:** Rufen Sie `ui.dispatch(event)` für jedes Eingabeereignis und
`ui.render(width, height, dpi_scale)` einmal pro Frame auf. Siehe
[docs/EMBEDDING.md](../EMBEDDING.md) für die vollständige API.

Beide Adapter liefern HiDPI, Cursor-Änderungen, hochpräzise Eingabe und
CSS-Selektor-basiertes Klick-Routing ohne jeglichen Glue-Code.

---

## Die Demos ausführen

Klonen Sie das Repo und bauen Sie mit CMake — der `examples/`-Ordner liefert
rund zwanzig End-to-End-Anwendungen, die Game-Tools, DCC-UIs und
Framework-Kompatibilität abdecken.

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

Nennenswerte:

| Demo | Pfad | Was sie zeigt |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | Das kleinstmögliche funktionierende Programm |
| Bootstrap-Dashboard | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | Echtes Bootstrap-4.6-CSS, Cards, Navbars, Tabellen |
| Game-Editor | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Andockbare Panels, Tree-View, Inspector |
| Dender (3D-Druck-Slicer) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Komplettes App-Layout mit Viewport |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | Emulator-UI eingebettet in ein natives Fenster |

Mit dem Task-Runner starten – er baut, was die Demo braucht, und führt sie aus (`./build.sh list` zeigt alle). Unter Windows `build.ps1` verwenden.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_game_editor.png" width="720" alt="Game-Editor-Demo">

*Decius-Game-Editor-Demo — andockbare Panels, Tree-View, Inspector und Toolbars im standardmäßigen Decius-CSS-Look von AffineUI.*

Die Python- und Rust-Bindings liefern eigene ausführbare Beispiele:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Dokumentation

| Dokument | Was drin steht |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Engine-Interna — Cascade, Resolver, Reconciler, Paint |
| [docs/BUILDING.md](../BUILDING.md) | Plattformspezifische Build-Hinweise |
| [docs/EMBEDDING.md](../EMBEDDING.md) | Verdrahten von AffineUI in ein bestehendes Fenster / eine bestehende Frame-Schleife |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Wie die Python-/Rust-/C#-Bindings den C++-Kern exponieren |
| [docs/RELEASING.md](../RELEASING.md) | Release-Prozess, Versionierung, Installationsbefehle pro Registry |
| [docs/ROADMAP.md](../ROADMAP.md) | Was als Nächstes ausgeliefert wird |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | Wie man beiträgt |

---

## Compile-Time-Schalter (C++-Drop-in)

| Makro | Zweck |
| --- | --- |
| `AFFINEUI_WITH_SDL` | Aktiviert den SDL2-Adapter. |
| `AFFINEUI_WITH_SOKOL` | Aktiviert den sokol_app-Adapter. |
| `AFFINEUI_NO_IMM` | Lässt die Immediate-Mode-Schicht weg. |
| `AFFINEUI_NO_C_API` | Lässt die C-ABI weg (nötig für die Sprach-Bindings). |
| `AFFINEUI_HTML_ENTITIES_FULL` | Bindet die vollständige HTML5-Named-Entity-Tabelle ein (Standard: kompakt). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | Gibt keine sokol-Implementierungssymbole aus. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | Gibt keine NanoVG-Implementierungssymbole aus. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | Gibt keine stb_image-Implementierungssymbole aus. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | Gibt keine stb_truetype-Implementierungssymbole aus. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | Gibt keine fontstash-Implementierungssymbole aus. |

Standard-GL-Backend-Defines: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`,
`AFFINEUI_BACKEND_GL`.

---

## Stack

| Schicht | Bibliothek | Lizenz | Warum |
| --- | --- | --- | --- |
| HTML5- + CSS-Parsing, DOM, Selektor-Matching | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Spezifikationstreu, gewartet |
| Flexbox-Mathematik | [Yoga](https://github.com/facebook/yoga) | MIT | Über React Native kampferprobt |
| 2D-Vektor-Painter | [NanoVG](https://github.com/memononen/nanovg) | zlib | Antialiased Strokes / Fills / Gradients / Text |
| Windowing + GPU-Abstraktion | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU hinter einer API |
| Schriften | fontstash + stb_truetype | zlib / MIT | Atlas-basierter Glyph-Cache |
| Raster-Bilder | stb_image | MIT / public | `<img>`-Dekodierung für PNG / JPG / GIF |

**Eigen entwickelt:** Cascade, Computed Style, Layout-Adapter, Paint-Driver,
Hit-Test, Klick-Routing, Reconciler, Komponenten-API. Alles, wo Design-Urteil
zählt.

**Delegiert:** HTML5-Tokenisierung, CSS3-Tokenisierung, Selektor-Matching,
Flexbox-Mathematik, Glyph-Rasterisierung, Vektor-Painting, Fenster + Eingabe.
Alles, wo Spezifikationstreue und Kampferprobung zählen.

---

## Lizenz

[MIT](../../LICENSE). Vendored Third-Party-Komponenten behalten ihre
ursprünglichen Lizenzen — siehe [external/README.md](../../external/README.md).
