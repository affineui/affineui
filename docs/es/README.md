<p align="center">
  <sub>
    <b>Léelo en:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
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

**Un renderizador de UI pequeño, compatible con HTML5 y acelerado por GPU, con un framework de componentes integrado — un reemplazo nativo para Electron y Qt.**

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI ejecutando el laminador de impresión 3D Dender">

*Dender — un laminador de impresión 3D que muestra el aspecto por defecto de AffineUI con Decius CSS. Paneles acoplables, un viewport personalizado y un layout de aplicación completo, todo renderizado de forma nativa.*

AffineUI ofrece un auténtico motor de layout y pintado HTML/CSS estilo navegador, empaquetado como un drop-in de dos archivos en C++, una librería de Python (al estilo de Gradio), una crate de Rust y un paquete NuGet para C#. Un solo renderizador, una sola API de componentes, cuatro lenguajes anfitriones. Corre a 120 Hz, anima con fluidez y no incrusta un navegador, ni una VM de JavaScript, ni un framework pesado.

Está pensado para:

- **Herramientas de juegos y UI dentro del juego** — lanzadores, HUDs, pantallas de configuración, paneles de depuración, overlays de editor.
- **Herramientas de contenido estilo DCC** — aplicaciones de la clase Maya / Blender / ZBrush / DaVinci Pro con UIs nativas densas.
- **Reemplazo de Qt / Electron** — cuando quieres el modelo de autoría HTML/CSS pero sin el navegador ni el sobrepeso del runtime.
- **Cualquiera que envíe una UI nativa pequeña y multiplataforma** que necesite verse diseñada, no improvisada.

**Se integra en un proyecto existente y simplemente funciona.** Si ya tienes una aplicación con SDL2 o sokol_app, añadir AffineUI son dos archivos y una llamada de conexión — sin fork del sistema de build, sin descarga en tiempo de ejecución, sin ventana aparte. Incluir, cargar, renderizar, listo.

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="Demo de un sintetizador skeuomórfico construido con AffineUI">

*Una demo skeuomórfica de sintetizador modular — todo lo que aparece en pantalla es HTML + CSS estándar: perillas, cables, texturas de panel, animaciones. Sin toolkit de widgets a medida, sin plugins.*

---

## Lo que AffineUI *no* es

- **No es un navegador web.** No hay navegación, ni cookies, ni
  `fetch` / `XMLHttpRequest`, ni gestión de ventanas. El objetivo es ser
  tan pequeño y rápido como una UI nativa pueda ser — cualquier
  funcionalidad que lo empujara hacia "navegador" queda fuera de
  alcance a propósito.
- **No es un HTML5 recortado.** AffineUI apunta a una cobertura *real*
  de HTML5, no a un subconjunto mínimo. Si una funcionalidad de HTML5
  o CSS no esotérica de la que dependan frameworks al nivel de
  Bootstrap, Tailwind o Ant no renderiza correctamente, considéralo un
  bug y repórtalo. Las carencias son fruto de la incompletitud, no de
  la intención.
- **No es un sandbox de seguridad.** AffineUI está pensado para
  renderizar *tu* HTML y *tu* CSS. **Nunca** le des markup,
  hojas de estilo o scripts sin confianza descargados de la web
  abierta. No hay modelo de origen, ni CSP, ni aislamiento entre la UI
  y el proceso anfitrión.
- **No es un framework que lo incluye todo.** AffineUI renderiza UI.
  No hace decodificación de `<video>`, ni animación por GPU de escenas
  arbitrarias, ni 3D, ni audio, ni networking, ni gestión de assets.
  Esas son tareas de tu aplicación — AffineUI es aquello a lo que las
  *diriges*.
- **(Aún) no es un runtime web nativo basado en JS.** Por defecto,
  AffineUI no ejecuta JavaScript — es una decisión deliberada de la
  versión actual para mantener la historia simple y el binario
  pequeño. El soporte de JS + React está en el roadmap y llegará como
  una extensión opcional, convirtiendo a AffineUI en un reemplazo de
  primera clase para Electron en equipos que quieran conservar su base
  de código de aplicación web.

---

## Estado

**Alpha.** El núcleo del renderizador, el layout, la cascada y el reconciliador ya son útiles para UIs reales hoy — dashboards con Bootstrap, layouts personalizados estilo DCC y UIs completas de herramientas renderizan todos. La cobertura del estándar es amplia pero incompleta, hay casos límite y algunas funcionalidades de CSS aún están llegando. Espera bugs. Espera reportar bugs. No se lo entregues aún a clientes.

---

## Instalación

Elige tu lenguaje. Los cuatro bindings se apoyan sobre el mismo núcleo de C++.

**Python** — como Gradio, pero nativo:

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

**C++ (drop-in, cero dependencias):** toma los dos archivos
[`dist/affineui.h`](../../dist/affineui.h) y [`dist/affineui.cpp`](../../dist/affineui.cpp),
añádelos a tu proyecto y compila `affineui.cpp` una vez como C++20.
Ese es el SDK completo — sin gestor de paquetes, sin árbol de submódulos, sin DLL.

Plataformas soportadas: Windows, macOS, Linux, iOS, Android, WebGL. Consulta
[docs/BUILDING.md](../BUILDING.md) para notas específicas de cada plataforma y
los requisitos previos para compilar desde el código fuente.

---

## Hola, mundo

Cada lenguaje incluye dos sabores:

- **API de componentes** — describe la UI como un árbol de widgets tipados
  (`heading`, `button`, `slider`, …). Guiada por un reconciliador; el estado
  se actualiza in situ. Esta es la API que en realidad quieres.
- **HTML crudo** — le pasas al motor una cadena de HTML. Este es el camino
  drop-in para aplicaciones existentes y para cualquier cosa que prefieras
  escribir como markup.

### Python

API de componentes:

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

HTML crudo:

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

API de componentes:

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

HTML crudo:

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

API de componentes:

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

HTML crudo:

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

API de componentes (usando el drop-in amalgamado):

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

HTML crudo:

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

## Una aplicación modesta

Un poco más que hola mundo — un contador que se actualiza en vivo usando la
API de componentes. El estado vive en el lenguaje anfitrión; el reconciliador
compara tu vista con la del último frame y parchea el DOM. Los efectos
hover/focus/animación de CSS siguen corriendo entre actualizaciones.

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

Los equivalentes en Rust, C# y C++ son uno a uno — mismos nombres de método,
mismo comportamiento del reconciliador. Consulta [`examples/`](../../examples/)
para programas más grandes de extremo a extremo (un editor de fotos completo,
un editor de juegos, un sintetizador modular, un laminador de impresión 3D)
construidos con esta misma API.

---

## HTML, CSS y sistemas de diseño

AffineUI es un renderizador real de HTML5 / CSS, no un resolvedor de layout
con forma de HTML. La tokenización de HTML y el DOM provienen de
[lexbor](https://github.com/lexbor/lexbor); las matemáticas de flexbox vienen
de [Yoga](https://github.com/facebook/yoga); el pintado pasa por
[NanoVG](https://github.com/memononen/nanovg) hacia
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU).
La cascada, el estilo computado, el hit-testing, el enrutamiento de selectores
y el reconciliador son nuestros.

### Decius CSS es el predeterminado

El predeterminado incluido es [Decius CSS](https://deciuscss.com) — un framework
moderno de componentes desarrollado en paralelo a AffineUI y afinado
deliberadamente para renderizar píxel-perfecto en este motor. Si usas la API
de componentes sin pasar una hoja de estilos, obtienes Decius, y simplemente
funciona.

### Trae tu propio CSS

Decius es el predeterminado, **no un requisito**. Cualquier CSS cuyos
selectores coincidan con los nombres de clase que emite AffineUI aplicará
estilo a los componentes integrados, y cualquier HTML escrito a mano que
cargues por la vía cruda puede traer el CSS que quiera. Bootstrap 4.6, clases
utilitarias al estilo Tailwind y markup de componentes al estilo Ant renderizan
todos sin configuración adicional — mira
[`examples/01_bootstrap`](../../examples/01_bootstrap) y
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI renderizando CSS de Bootstrap">

*La librería CSS Bootstrap 4.6 sin modificar renderizando de forma nativa — tarjetas, navbars, botones y estados hover/active, directamente del `.min.css` real.*

### JavaScript de frameworks

No hay motor de JS (ver [Lo que AffineUI *no* es](#lo-que-affineui-no-es)).
La interactividad de los frameworks — los dropdowns de Bootstrap, los modales
de Ant, etc. — se mapea a comportamiento nativo en C++ en lugar de ejecutarse
como script.

---

## Integración en una aplicación existente

Si ya tienes una ventana y un bucle de frames, AffineUI se conecta mediante un
adaptador en tiempo de compilación. Vienen dos incorporados — SDL2 y sokol_app
— y existe un camino manual para todo lo demás.

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

**Manual:** llama a `ui.dispatch(event)` por cada evento de entrada y a
`ui.render(width, height, dpi_scale)` una vez por frame. Consulta
[docs/EMBEDDING.md](../EMBEDDING.md) para la API completa.

Ambos adaptadores te dan HiDPI, cambios de cursor, entrada de alta precisión y
enrutamiento de clicks por selector CSS sin ningún código de pegamento.

---

## Ejecutando las demos

Clona el repo y compílalo con CMake — la carpeta `examples/` incluye
aproximadamente veinte aplicaciones de extremo a extremo que cubren
herramientas de juegos, UIs de tipo DCC y compatibilidad con frameworks.

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

Algunas destacadas:

| Demo | Ruta | Qué muestra |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | El programa funcional más pequeño |
| Dashboard de Bootstrap | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | CSS real de Bootstrap 4.6, tarjetas, navbars, tablas |
| Editor de juegos | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Paneles acoplables, vista de árbol, inspector |
| Dender (laminador de impresión 3D) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Layout de aplicación completo con viewport |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | UI de emulador incrustada en una ventana nativa |

Ejecuta cualquiera con el task runner: compila lo necesario y la lanza (`./build.sh list` las muestra todas). En Windows usa `build.ps1`.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_game_editor.png" width="720" alt="Demo del editor de juegos">

*Demo del Decius Game Editor — paneles acoplables, vista de árbol, inspector y barras de herramientas con el aspecto por defecto de AffineUI con Decius CSS.*

Los bindings de Python y Rust incluyen sus propios ejemplos ejecutables:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Documentación

| Documento | Qué contiene |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Interior del motor — cascada, resolvedor, reconciliador, pintado |
| [docs/BUILDING.md](../BUILDING.md) | Notas de compilación específicas de cada plataforma |
| [docs/EMBEDDING.md](../EMBEDDING.md) | Cómo conectar AffineUI a una ventana / bucle de frames existente |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Cómo los bindings de Python / Rust / C# exponen el núcleo de C++ |
| [docs/RELEASING.md](../RELEASING.md) | Proceso de release, versionado, comandos de instalación por registro |
| [docs/ROADMAP.md](../ROADMAP.md) | Qué llega a continuación |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | Cómo contribuir |

---

## Interruptores en tiempo de compilación (drop-in de C++)

| Macro | Uso |
| --- | --- |
| `AFFINEUI_WITH_SDL` | Habilita el adaptador de SDL2. |
| `AFFINEUI_WITH_SOKOL` | Habilita el adaptador de sokol_app. |
| `AFFINEUI_NO_IMM` | Omite la capa de modo inmediato. |
| `AFFINEUI_NO_C_API` | Omite la ABI de C (necesaria para los bindings de lenguajes). |
| `AFFINEUI_HTML_ENTITIES_FULL` | Incluye la tabla completa de entidades con nombre de HTML5 (predeterminado: compacta). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | No emite los símbolos de implementación de sokol. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | No emite los símbolos de implementación de NanoVG. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | No emite los símbolos de implementación de stb_image. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | No emite los símbolos de implementación de stb_truetype. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | No emite los símbolos de implementación de fontstash. |

Defines predeterminados del backend GL: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`,
`AFFINEUI_BACKEND_GL`.

---

## Stack

| Capa | Librería | Licencia | Por qué |
| --- | --- | --- | --- |
| Parsing de HTML5 + CSS, DOM, coincidencia de selectores | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Riguroso con la spec, mantenido |
| Matemáticas de flexbox | [Yoga](https://github.com/facebook/yoga) | MIT | Probado en batalla vía React Native |
| Pintor vectorial 2D | [NanoVG](https://github.com/memononen/nanovg) | zlib | Trazos / rellenos / degradados / texto con antialiasing |
| Ventanas + abstracción de GPU | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU tras una sola API |
| Fuentes | fontstash + stb_truetype | zlib / MIT | Caché de glifos basado en atlas |
| Imágenes rasterizadas | stb_image | MIT / público | Decodificación de `<img>` para PNG / JPG / GIF |

**Propio:** cascada, estilo computado, adaptador de layout, driver de pintado,
hit-test, enrutamiento de clicks, reconciliador, API de componentes. Todo
aquello donde importa el criterio de diseño.

**Delegado:** tokenización de HTML5, tokenización de CSS3, coincidencia de
selectores, matemáticas de flexbox, rasterización de glifos, pintado vectorial,
ventanas + entrada. Todo aquello donde importa el cumplimiento de la spec y
haber sido probado en batalla.

---

## Licencia

[MIT](../../LICENSE). Los componentes de terceros incluidos conservan sus
licencias originales — ver [external/README.md](../../external/README.md).
