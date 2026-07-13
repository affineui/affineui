<p align="center">
  <sub>
    <b>इसे पढ़ें:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**एक छोटा, HTML5-अनुरूप, GPU-त्वरित UI renderer, जिसमें एक एकीकृत component framework शामिल है — Electron और Qt का एक native विकल्प।**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="Dender 3D-print slicer चलाता हुआ AffineUI">

*Dender — एक 3D-print slicer जो AffineUI का डिफ़ॉल्ट Decius CSS look दिखा रहा है। Docked panels, एक custom viewport, और पूरे-app का layout — सब कुछ natively रेंडर किया गया।*

AffineUI एक असली browser-style HTML/CSS layout और paint engine प्रदान करता है, जो एक दो-फ़ाइल वाले C++ drop-in, एक Python library (Gradio-style), एक Rust crate, और एक C# NuGet package के रूप में उपलब्ध है। एक renderer, एक component API, चार host languages। यह 120 Hz पर चलता है, सहजता से animate करता है, और इसमें कोई browser, JavaScript VM, या भारी framework embed नहीं है।

यह इनके लिए बनाया गया है:

- **Game tools और in-game UI** — launchers, HUDs, settings screens, debug
  panels, editor overlays।
- **DCC-style content tools** — Maya / Blender / ZBrush / DaVinci Pro-श्रेणी
  के applications, जिनमें घने native UIs हों।
- **Qt / Electron का विकल्प** — जब आप HTML/CSS authoring model तो चाहते हैं,
  लेकिन browser या runtime का भारीपन नहीं।
- **कोई भी जो एक छोटा, cross-platform native UI** ship करना चाहता है, जो
  डिज़ाइन किया हुआ दिखे — जोड़ा-तोड़ा नहीं।

**यह एक existing project में drop-in हो जाता है और बस काम करता है।** अगर आपके पास पहले से कोई SDL2 या sokol_app application है, तो AffineUI जोड़ना बस दो फ़ाइलें और एक wire-up call है — कोई build-system fork नहीं, कोई runtime download नहीं, कोई अलग window नहीं। Include, load, render, हो गया।

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="AffineUI से बनाया गया skeuomorphic synth demo">

*एक skeuomorphic modular synth demo — screen पर दिखने वाली हर चीज़ standard HTML + CSS है: knobs, cables, panel textures, animations। कोई custom widget toolkit नहीं, कोई plugins नहीं।*

---

## AffineUI क्या *नहीं* है

- **यह web browser नहीं है।** कोई navigation नहीं, कोई cookies नहीं, कोई
  `fetch` / `XMLHttpRequest` नहीं, कोई window management नहीं। लक्ष्य है
  उतना ही छोटा और तेज़ होना जितना native UI हो सकता है — हर वह feature
  जो इसे "browser" की ओर बढ़ाए, जानबूझकर scope से बाहर है।
- **यह कमज़ोर किया गया HTML5 नहीं है।** AffineUI का लक्ष्य *असली* HTML5
  coverage है, कोई न्यूनतम subset नहीं। यदि कोई गैर-दुर्लभ HTML5 या CSS
  feature, जिस पर Bootstrap-, Tailwind-, या Ant-श्रेणी के frameworks
  निर्भर करते हैं, सही से render नहीं होता, तो उसे bug मानें और report करें।
  खालीपन अधूरेपन का है, इरादे का नहीं।
- **यह security sandbox नहीं है।** AffineUI का उद्देश्य *आपका* HTML और
  *आपका* CSS render करना है। खुले web से डाउनलोड किए गए भरोसे-योग्य नहीं
  markup, stylesheets, या scripts **कभी भी** इसमें न डालें। कोई origin
  model नहीं है, कोई CSP नहीं है, UI और host process के बीच कोई isolation
  नहीं है।
- **यह सब-कुछ-शामिल framework नहीं है।** AffineUI UI render करता है। यह
  `<video>` decoding, arbitrary scenes का GPU-driven animation, 3D, audio,
  networking, या asset management नहीं करता। ये आपके application का
  काम हैं — AffineUI वह है जिस पर आप उन्हें *इंगित* करते हैं।
- **यह (अभी तक) JS-आधारित native web runtime नहीं है।** डिफ़ॉल्ट रूप से,
  AffineUI JavaScript नहीं चलाता — मौजूदा release के लिए यह एक जानबूझकर
  लिया गया निर्णय है ताकि कहानी सरल रहे और binary छोटी। JS + React
  support roadmap पर है और एक opt-in extension के रूप में आएगा, जिससे
  AffineUI उन teams के लिए एक first-class Electron विकल्प बन जाएगा जो
  अपने web-app codebase को बरकरार रखना चाहती हैं।

---

## स्थिति

**Alpha।** Core renderer, layout, cascade, और reconciler आज असली UIs के
लिए इस्तेमाल करने योग्य हैं — Bootstrap dashboards, custom DCC layouts,
और पूरे tool UIs सभी render होते हैं। Standards coverage व्यापक है
लेकिन अधूरी है, edge cases मौजूद हैं, और कुछ CSS features अभी भी आ रहे
हैं। Bugs की उम्मीद रखें। Bugs report करने की उम्मीद रखें। इसे अभी
customers को ship न करें।

---

## Install

अपनी language चुनें। चारों bindings एक ही C++ core पर बैठे हैं।

**Python** — Gradio जैसा, पर native:

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

**C++ (drop-in, कोई dependencies नहीं):** दो फ़ाइलें लें
[`dist/affineui.h`](../../dist/affineui.h) और [`dist/affineui.cpp`](../../dist/affineui.cpp),
उन्हें अपने project में जोड़ें, और `affineui.cpp` को एक बार C++20 के रूप में compile करें।
बस यही पूरा SDK है — कोई package manager नहीं, कोई submodule tree नहीं, कोई DLL नहीं।

समर्थित platforms: Windows, macOS, Linux, iOS, Android, WebGL। Platform-विशिष्ट notes और source से build करने के लिए आवश्यक चीज़ों के लिए
[docs/BUILDING.md](../BUILDING.md) देखें।

---

## Hello, world

हर language में दो प्रकार ship होते हैं:

- **Component API** — UI को typed widgets (`heading`, `button`, `slider`, …)
  के tree के रूप में वर्णित करें। Reconciler-चालित; state updates जगह पर
  होते हैं। यही API आप वास्तव में चाहते हैं।
- **Raw HTML** — engine को एक HTML string सौंप दें। यह existing apps के लिए
  drop-in रास्ता है, और उस हर चीज़ के लिए जिसे आप markup के रूप में लिखना
  पसंद करेंगे।

### Python

Component API:

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

Raw HTML:

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

Component API:

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

Raw HTML:

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

Component API:

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

Raw HTML:

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

Component API (amalgamated drop-in का उपयोग करते हुए):

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

Raw HTML:

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

## एक साधारण app

Hello world से थोड़ा अधिक — component API का उपयोग करके एक live-updating
counter। State host language में रहता है; reconciler आपके view की तुलना
पिछले frame से करता है और DOM को patch करता है। Updates के बीच CSS
hover/focus/animation चलता रहता है।

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

Rust, C#, और C++ के समकक्ष एक-से-एक हैं — वही method names, वही
reconciler व्यवहार। इसी API से बने बड़े end-to-end programs (एक पूरा
photo editor, एक game editor, एक modular synth, एक 3D-print slicer)
के लिए [`examples/`](../../examples/) देखें।

---

## HTML, CSS, और design systems

AffineUI एक असली HTML5 / CSS renderer है, कोई HTML-आकार का layout
solver नहीं। HTML tokenization और DOM [lexbor](https://github.com/lexbor/lexbor)
से आते हैं; flexbox गणित [Yoga](https://github.com/facebook/yoga) से;
paint [NanoVG](https://github.com/memononen/nanovg) के ज़रिए
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU) में जाता है।
Cascade, computed style, hit-testing, selector routing, और
reconciler हमारे हैं।

### Decius CSS डिफ़ॉल्ट है

Bundled डिफ़ॉल्ट [Decius CSS](https://deciuscss.com) है — एक आधुनिक
component framework जो AffineUI के साथ-साथ विकसित किया गया है और
जानबूझकर इस engine पर pixel-perfect render करने के लिए tuned है।
अगर आप component API का उपयोग stylesheet पास किए बिना करते हैं, तो
आपको Decius मिलता है, और यह बस काम करता है।

### अपना CSS लाएँ

Decius डिफ़ॉल्ट है, **आवश्यकता नहीं**। कोई भी CSS जिसके selectors AffineUI द्वारा emit किए गए class names से मेल खाते हों, built-in
components को style कर देगा, और raw path के ज़रिए load किया गया कोई भी
hand-authored HTML अपनी पसंद का कोई भी CSS ला सकता है। Bootstrap 4.6,
Tailwind-style utility classes, और Ant-style component markup — सब
बिना बदलाव के render होते हैं — देखें
[`examples/01_bootstrap`](../../examples/01_bootstrap) और
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard)।

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="Bootstrap CSS को render करता हुआ AffineUI">

*अनसंशोधित Bootstrap 4.6 CSS library natively render हो रही है — cards, navbars, buttons, और hover/active states, सीधे असली `.min.css` से।*

### Framework JavaScript

कोई JS engine नहीं है ([AffineUI क्या *नहीं* है](#affineui-क्या-नहीं-है) देखें)।
Framework interactivity — Bootstrap के dropdowns, Ant के modals, वगैरह —
script के रूप में चलने के बजाय native C++ व्यवहार पर mapped है।

---

## किसी existing app में embed करना

यदि आपके पास पहले से एक window और एक frame loop है, तो AffineUI एक
compile-time adapter के ज़रिए wire हो जाता है। दो built-in हैं — SDL2
और sokol_app — और बाकी सब के लिए एक manual रास्ता मौजूद है।

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

**Manual:** प्रत्येक input event के लिए `ui.dispatch(event)` को call करें
और प्रति frame एक बार `ui.render(width, height, dpi_scale)` को। पूरे API
के लिए [docs/EMBEDDING.md](../EMBEDDING.md) देखें।

दोनों adapters आपको बिना किसी glue code के HiDPI, cursor changes,
high-precision input, और CSS-selector click routing देते हैं।

---

## Demos चलाना

Repo को clone करें और CMake से build करें — `examples/` folder में
लगभग बीस end-to-end applications ship होती हैं, जो game tools, DCC
UIs, और framework compatibility को कवर करती हैं।

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

उल्लेखनीय demos:

| Demo | Path | क्या दिखाता है |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | सबसे छोटा काम करने वाला program |
| Bootstrap dashboard | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | असली Bootstrap 4.6 CSS, cards, navbars, tables |
| Game editor | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Docked panels, tree view, inspector |
| Dender (3D print slicer) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Viewport सहित पूरे-app का layout |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | Native window में embedded emulator UI |

टास्क रनर से कोई भी चलाएँ — यह ज़रूरी चीज़ें बिल्ड करके लॉन्च करता है (`./build.sh list` सभी दिखाता है)। Windows पर `build.ps1` इस्तेमाल करें।

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="Game editor demo">

*Decius Game Editor demo — AffineUI के डिफ़ॉल्ट Decius CSS look में docked panels, tree view, inspector, और toolbars।*

Python और Rust bindings अपने खुद के runnable examples ship करते हैं:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Docs

| Doc | इसमें क्या है |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Engine के आंतरिक — cascade, resolver, reconciler, paint |
| [docs/BUILDING.md](../BUILDING.md) | Platform-विशिष्ट build notes |
| [docs/EMBEDDING.md](../EMBEDDING.md) | AffineUI को किसी existing window / frame loop में wire करना |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Python / Rust / C# bindings C++ core को कैसे expose करते हैं |
| [docs/RELEASING.md](../RELEASING.md) | Release process, versioning, प्रति-registry install commands |
| [docs/ROADMAP.md](../ROADMAP.md) | आगे क्या ship हो रहा है |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | योगदान कैसे करें |

---

## Compile-time switches (C++ drop-in)

| Macro | उपयोग |
| --- | --- |
| `AFFINEUI_WITH_SDL` | SDL2 adapter सक्षम करें। |
| `AFFINEUI_WITH_SOKOL` | sokol_app adapter सक्षम करें। |
| `AFFINEUI_NO_IMM` | Immediate-mode layer छोड़ दें। |
| `AFFINEUI_NO_C_API` | C ABI छोड़ दें (language bindings के लिए ज़रूरी)। |
| `AFFINEUI_HTML_ENTITIES_FULL` | पूरी HTML5 named-entity table शामिल करें (डिफ़ॉल्ट: संक्षिप्त)। |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | sokol implementation symbols emit न करें। |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | NanoVG implementation symbols emit न करें। |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | stb_image implementation symbols emit न करें। |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | stb_truetype implementation symbols emit न करें। |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | fontstash implementation symbols emit न करें। |

डिफ़ॉल्ट GL backend defines: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`,
`AFFINEUI_BACKEND_GL`।

---

## Stack

| Layer | Library | License | क्यों |
| --- | --- | --- | --- |
| HTML5 + CSS parsing, DOM, selector matching | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Spec-कट्टर, अनुरक्षित |
| Flexbox गणित | [Yoga](https://github.com/facebook/yoga) | MIT | React Native के ज़रिए battle-tested |
| 2D vector painter | [NanoVG](https://github.com/memononen/nanovg) | zlib | Antialiased strokes / fills / gradients / text |
| Windowing + GPU abstraction | [sokol](https://github.com/floooh/sokol) | zlib | एक API के पीछे Metal / D3D11 / GL / WebGPU |
| Fonts | fontstash + stb_truetype | zlib / MIT | Atlas-आधारित glyph cache |
| Raster images | stb_image | MIT / public | PNG / JPG / GIF के लिए `<img>` decode |

**खुद का:** cascade, computed style, layout adapter, paint driver,
hit-test, click routing, reconciler, component API। हर वह चीज़ जहाँ
design का निर्णय मायने रखता है।

**सौंपा हुआ:** HTML5 tokenization, CSS3 tokenization, selector matching,
flexbox गणित, glyph rasterization, vector painting, window + input।
हर वह चीज़ जहाँ spec compliance और battle-testing मायने रखते हैं।

---

## License

[MIT](../../LICENSE)। Vendored third-party components अपने मूल licenses बरकरार रखते हैं — देखें [external/README.md](../../external/README.md)।
