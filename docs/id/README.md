<p align="center">
  <sub>
    <b>Baca dalam:</b>
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
    <a href="../de/README.md">Deutsch</a> ·
    <a href="README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**Renderer UI berukuran kecil, patuh terhadap HTML5, dengan akselerasi GPU
dan framework komponen terintegrasi — pengganti native untuk Electron dan Qt.**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI menjalankan slicer cetak 3D Dender">

*Dender — slicer cetak 3D yang menampilkan tampilan default Decius CSS pada AffineUI. Panel docked, viewport kustom, dan tata letak aplikasi penuh, semuanya di-render secara native.*

AffineUI menghadirkan engine layout dan paint HTML/CSS bergaya browser sungguhan
sebagai drop-in C++ dua file, sebuah library Python (bergaya Gradio), sebuah crate
Rust, dan sebuah paket NuGet C#. Satu renderer, satu API komponen, empat bahasa host.
Ia berjalan pada 120 Hz, menganimasikan dengan mulus, dan tidak menyematkan browser,
VM JavaScript, maupun framework besar.

Ia dibangun untuk:

- **Alat game dan UI dalam game** — launcher, HUD, layar pengaturan, panel
  debug, overlay editor.
- **Alat konten bergaya DCC** — aplikasi kelas Maya / Blender / ZBrush / DaVinci Pro
  dengan UI native yang padat.
- **Pengganti Qt / Electron** — ketika Anda menginginkan model authoring HTML/CSS
  tetapi bukan browser atau bloat runtime-nya.
- **Siapa saja yang merilis UI native lintas-platform berukuran kecil** yang harus
  terlihat didesain, bukan asal jadi.

**Ia langsung masuk ke proyek yang sudah ada dan bekerja begitu saja.** Jika Anda
sudah memiliki aplikasi SDL2 atau sokol_app, menambahkan AffineUI hanya butuh dua
file dan satu pemanggilan wire-up — tanpa fork sistem build, tanpa unduh runtime,
tanpa jendela terpisah. Include, load, render, selesai.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="Demo synth skeuomorfik dibangun dengan AffineUI">

*Demo synth modular skeuomorfik — semua yang tampil di layar adalah HTML + CSS standar: kenop, kabel, tekstur panel, animasi. Tanpa toolkit widget kustom, tanpa plugin.*

---

## Apa yang *bukan* AffineUI

- **Bukan web browser.** Tidak ada navigasi, tidak ada cookie, tidak ada
  `fetch` / `XMLHttpRequest`, tidak ada manajemen jendela. Tujuannya adalah menjadi
  sekecil dan secepat mungkin seperti UI native — setiap fitur yang akan
  mengembangkannya ke arah "browser" secara sengaja berada di luar scope.
- **Bukan HTML5 yang dilemahkan.** AffineUI mengincar cakupan HTML5 yang *sebenarnya*,
  bukan subset minimal. Jika sebuah fitur HTML5 atau CSS yang tidak esoteris yang
  diandalkan oleh framework kelas Bootstrap, Tailwind, atau Ant tidak ter-render
  dengan benar, anggap itu sebagai bug dan laporkan. Kesenjangan itu adalah
  ketidaklengkapan, bukan disengaja.
- **Bukan sandbox keamanan.** AffineUI dimaksudkan untuk me-render HTML *Anda*
  dan CSS *Anda*. **Jangan pernah** memberinya markup, stylesheet, atau skrip
  tidak tepercaya yang diunduh dari web terbuka. Tidak ada model origin, tidak ada
  CSP, tidak ada isolasi antara UI dan proses host.
- **Bukan framework yang menyertakan segalanya.** AffineUI me-render UI. Ia
  tidak melakukan dekode `<video>`, animasi digerakkan GPU untuk scene sembarang,
  3D, audio, jaringan, atau manajemen aset. Itu adalah tugas aplikasi Anda —
  AffineUI adalah tempat Anda mengarahkannya.
- **Belum menjadi runtime web native berbasis JS.** Secara default, AffineUI
  tidak menjalankan JavaScript — ini adalah pilihan sengaja untuk rilis saat ini
  demi menjaga alur cerita tetap sederhana dan binary tetap kecil. Dukungan JS + React
  ada di roadmap dan akan hadir sebagai ekstensi opt-in, menjadikan AffineUI
  pengganti Electron kelas satu bagi tim yang ingin mempertahankan basis kode
  aplikasi web mereka.

---

## Status

**Alpha.** Renderer inti, layout, cascade, dan reconciler sudah dapat digunakan
untuk UI sungguhan hari ini — dashboard Bootstrap, tata letak DCC kustom, dan UI
alat penuh semuanya ter-render. Cakupan standar sudah luas namun belum lengkap,
kasus tepi masih ada, dan beberapa fitur CSS masih dalam proses masuk. Harap
bersiap menghadapi bug. Harap bersiap melaporkan bug. Jangan rilis ke pelanggan
dulu.

---

## Install

Pilih bahasa Anda. Keempat binding berada di atas core C++ yang sama.

**Python** — seperti Gradio, tetapi native:

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

**C++ (drop-in, tanpa dependensi):** ambil kedua file
[`dist/affineui.h`](../../dist/affineui.h) dan [`dist/affineui.cpp`](../../dist/affineui.cpp),
tambahkan ke proyek Anda, dan kompilasi `affineui.cpp` sekali sebagai C++20.
Itulah keseluruhan SDK-nya — tanpa package manager, tanpa pohon submodule, tanpa DLL.

Platform yang didukung: Windows, macOS, Linux, iOS, Android, WebGL. Lihat
[docs/BUILDING.md](../BUILDING.md) untuk catatan spesifik-platform dan
prasyarat untuk build dari source.

---

## Hello, world

Dua varian tersedia di setiap bahasa:

- **API Komponen** — mendeskripsikan UI sebagai pohon widget bertipe
  (`heading`, `button`, `slider`, …). Digerakkan oleh reconciler; pembaruan state
  terjadi di tempat. Ini adalah API yang sebenarnya Anda inginkan.
- **HTML mentah** — serahkan string HTML kepada engine. Ini adalah jalur drop-in
  untuk aplikasi yang sudah ada dan untuk apa pun yang lebih Anda tulis sebagai markup.

### Python

API Komponen:

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

HTML mentah:

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

API Komponen:

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

HTML mentah:

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

API Komponen:

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

HTML mentah:

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

API Komponen (menggunakan drop-in yang sudah digabung):

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

HTML mentah:

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

## Sebuah aplikasi sederhana

Sedikit lebih dari hello world — sebuah counter yang memperbarui secara langsung
menggunakan API komponen. State tinggal di bahasa host; reconciler melakukan diff
view Anda terhadap frame terakhir dan mem-patch DOM. Hover/focus/animasi CSS terus
berjalan di antara pembaruan.

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

Padanan Rust, C#, dan C++ satu-satu — nama method yang sama, perilaku reconciler
yang sama. Lihat [`examples/`](../../examples/) untuk program end-to-end yang
lebih besar (editor foto lengkap, editor game, synth modular, slicer cetak 3D)
yang dibangun dengan API yang sama.

---

## HTML, CSS, dan design system

AffineUI adalah renderer HTML5 / CSS sungguhan, bukan solver layout yang hanya
berbentuk seperti HTML. Tokenisasi HTML dan DOM berasal dari [lexbor](https://github.com/lexbor/lexbor);
matematika flexbox berasal dari [Yoga](https://github.com/facebook/yoga);
paint melewati [NanoVG](https://github.com/memononen/nanovg) menuju
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU).
Cascade, computed style, hit-testing, routing selector, dan reconciler adalah milik kami.

### Decius CSS adalah default-nya

Default yang sudah disertakan adalah [Decius CSS](https://deciuscss.com) — sebuah
framework komponen modern yang dikembangkan bersama AffineUI dan sengaja disetel
agar ter-render pixel-perfect pada engine ini. Jika Anda menggunakan API komponen
tanpa memberikan stylesheet, Anda mendapatkan Decius, dan ia bekerja begitu saja.

### Bawa CSS Anda sendiri

Decius adalah default, **bukan keharusan**. CSS apa pun yang selector-nya cocok
dengan nama kelas yang dikeluarkan AffineUI akan men-style komponen bawaan, dan
HTML tulisan-tangan apa pun yang Anda muat melalui jalur mentah dapat membawa CSS
apa pun yang diinginkannya. Bootstrap 4.6, kelas utility bergaya Tailwind, dan
markup komponen bergaya Ant semuanya ter-render out of the box — lihat
[`examples/01_bootstrap`](../../examples/01_bootstrap) dan
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI me-render Bootstrap CSS">

*Library CSS Bootstrap 4.6 tanpa modifikasi ter-render secara native — cards, navbar, tombol, dan state hover/active, langsung dari file `.min.css` asli.*

### Framework JavaScript

Tidak ada JS engine (lihat [Apa yang *bukan* AffineUI](#apa-yang-bukan-affineui)).
Interaktivitas framework — dropdown Bootstrap, modal Ant, dan seterusnya —
dipetakan ke perilaku C++ native alih-alih dijalankan sebagai skrip.

---

## Menyematkan ke aplikasi yang sudah ada

Jika Anda sudah memiliki jendela dan frame loop, AffineUI terhubung melalui
adapter compile-time. Dua sudah dibangun ke dalam — SDL2 dan sokol_app — dan
jalur manual tersedia untuk yang lainnya.

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

**Manual:** panggil `ui.dispatch(event)` untuk setiap event input dan
`ui.render(width, height, dpi_scale)` sekali per frame. Lihat
[docs/EMBEDDING.md](../EMBEDDING.md) untuk API lengkapnya.

Kedua adapter memberi Anda HiDPI, perubahan kursor, input presisi tinggi, dan
routing klik berbasis selector CSS tanpa kode glue apa pun.

---

## Menjalankan demo

Clone repo dan build dengan CMake — folder `examples/` menyertakan sekitar dua
puluh aplikasi end-to-end yang mencakup alat game, UI DCC, dan kompatibilitas
framework.

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

Beberapa yang menonjol:

| Demo | Path | Yang ditampilkan |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | Program berjalan terkecil |
| Bootstrap dashboard | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | Bootstrap 4.6 CSS sungguhan, cards, navbar, tabel |
| Editor game | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Panel docked, tree view, inspector |
| Dender (slicer cetak 3D) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Tata letak aplikasi penuh dengan viewport |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | UI emulator disematkan dalam jendela native |

Jalankan salah satunya dengan task runner — ia membangun yang diperlukan lalu menjalankannya (`./build.sh list` menampilkan semuanya). Di Windows gunakan `build.ps1`.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="Demo editor game">

*Demo Decius Game Editor — panel docked, tree view, inspector, dan toolbar dalam tampilan default Decius CSS pada AffineUI.*

Binding Python dan Rust menyertakan contoh yang dapat dijalankan sendiri:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Dokumentasi

| Dok | Isinya |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Internal engine — cascade, resolver, reconciler, paint |
| [docs/BUILDING.md](../BUILDING.md) | Catatan build spesifik-platform |
| [docs/EMBEDDING.md](../EMBEDDING.md) | Menyambungkan AffineUI ke jendela / frame loop yang sudah ada |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Bagaimana binding Python / Rust / C# mengekspos core C++ |
| [docs/RELEASING.md](../RELEASING.md) | Proses rilis, versioning, perintah install per-registry |
| [docs/ROADMAP.md](../ROADMAP.md) | Apa yang akan dirilis berikutnya |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | Cara berkontribusi |

---

## Sakelar compile-time (drop-in C++)

| Makro | Kegunaan |
| --- | --- |
| `AFFINEUI_WITH_SDL` | Mengaktifkan adapter SDL2. |
| `AFFINEUI_WITH_SOKOL` | Mengaktifkan adapter sokol_app. |
| `AFFINEUI_NO_IMM` | Menghilangkan layer immediate-mode. |
| `AFFINEUI_NO_C_API` | Menghilangkan C ABI (dibutuhkan untuk binding bahasa). |
| `AFFINEUI_HTML_ENTITIES_FULL` | Menyertakan tabel named-entity HTML5 lengkap (default: ringkas). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | Jangan keluarkan simbol implementasi sokol. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | Jangan keluarkan simbol implementasi NanoVG. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | Jangan keluarkan simbol implementasi stb_image. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | Jangan keluarkan simbol implementasi stb_truetype. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | Jangan keluarkan simbol implementasi fontstash. |

Defines backend GL default: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`,
`AFFINEUI_BACKEND_GL`.

---

## Stack

| Layer | Library | Lisensi | Alasan |
| --- | --- | --- | --- |
| Parsing HTML5 + CSS, DOM, pencocokan selector | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Patuh spesifikasi, terawat |
| Matematika flexbox | [Yoga](https://github.com/facebook/yoga) | MIT | Teruji lewat React Native |
| Painter vektor 2D | [NanoVG](https://github.com/memononen/nanovg) | zlib | Stroke / fill / gradien / teks antialias |
| Windowing + abstraksi GPU | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU di balik satu API |
| Font | fontstash + stb_truetype | zlib / MIT | Cache glyph berbasis atlas |
| Gambar raster | stb_image | MIT / public | Dekode `<img>` untuk PNG / JPG / GIF |

**Dimiliki:** cascade, computed style, adapter layout, driver paint,
hit-test, routing klik, reconciler, API komponen. Semua tempat di mana
pertimbangan desain penting.

**Didelegasikan:** tokenisasi HTML5, tokenisasi CSS3, pencocokan selector,
matematika flexbox, rasterisasi glyph, painting vektor, jendela + input.
Semua tempat di mana kepatuhan spesifikasi dan teruji-pertempuran penting.

---

## Lisensi

[MIT](../../LICENSE). Komponen pihak ketiga yang di-vendor mempertahankan lisensi
aslinya — lihat [external/README.md](../../external/README.md).
