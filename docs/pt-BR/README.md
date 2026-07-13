<p align="center">
  <sub>
    <b>Leia em:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**Um renderizador de UI pequeno, compatível com HTML5 e acelerado por GPU, com um framework de componentes integrado — um substituto nativo para Electron e Qt.**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI executando o fatiador de impressão 3D Dender">

*Dender — um fatiador de impressão 3D exibindo a aparência CSS padrão Decius do AffineUI. Painéis encaixáveis, uma viewport personalizada e um layout completo de aplicação, tudo renderizado nativamente.*

O AffineUI oferece um motor real de layout e pintura HTML/CSS no estilo de navegador como um drop-in C++ de dois arquivos, uma biblioteca Python (no estilo Gradio), um crate Rust e um pacote NuGet C#. Um renderizador, uma API de componentes, quatro linguagens hospedeiras. Ele roda a 120 Hz, anima suavemente e não embute um navegador, uma VM JavaScript ou um framework grande.

Foi construído para:

- **Ferramentas de jogos e UI dentro do jogo** — launchers, HUDs, telas de configurações, painéis
  de depuração, overlays de editor.
- **Ferramentas de conteúdo estilo DCC** — aplicações da classe Maya / Blender / ZBrush / DaVinci Pro
  com UIs nativas densas.
- **Substituto para Qt / Electron** — quando você quer o modelo de autoria HTML/CSS
  mas não o navegador nem o inchaço do runtime.
- **Qualquer pessoa entregando uma UI nativa pequena e multiplataforma** que precise parecer
  bem projetada, não improvisada.

**Ele se integra a um projeto existente e simplesmente funciona.** Se você já tem uma aplicação SDL2 ou sokol_app, adicionar o AffineUI são dois arquivos e uma chamada de conexão — sem fork do sistema de build, sem download em runtime, sem janela separada. Inclua, carregue, renderize, pronto.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="Demo de sintetizador esqueumórfico construído com AffineUI">

*Um demo de sintetizador modular esqueumórfico — tudo na tela é HTML + CSS padrão: knobs, cabos, texturas de painel, animações. Sem toolkit de widgets personalizado, sem plugins.*

---

## O que o AffineUI *não* é

- **Não é um navegador web.** Sem navegação, sem cookies, sem
  `fetch` / `XMLHttpRequest`, sem gerenciamento de janelas. O objetivo é ser tão
  pequeno e rápido quanto uma UI nativa pode ser — cada recurso que o faria crescer
  em direção a "navegador" está fora de escopo de propósito.
- **Não é um HTML5 capado.** O AffineUI mira cobertura *real* de HTML5, não um
  subconjunto mínimo. Se um recurso não esotérico de HTML5 ou CSS do qual
  frameworks da classe Bootstrap, Tailwind ou Ant dependem não renderizar
  corretamente, trate como bug e reporte. As lacunas são
  incompletudes, não intenção.
- **Não é um sandbox de segurança.** O AffineUI serve para renderizar *seu* HTML
  e *seu* CSS. **Nunca** o alimente com marcação, folhas de estilo ou scripts
  não confiáveis baixados da web aberta. Não há modelo de origem, sem
  CSP, sem isolamento entre a UI e o processo hospedeiro.
- **Não é um framework tudo-incluído.** O AffineUI renderiza UI. Ele
  não faz decodificação de `<video>`, animação por GPU de cenas arbitrárias,
  3D, áudio, rede ou gerenciamento de assets. Isso é trabalho da sua
  aplicação — o AffineUI é aquilo para o qual você os aponta.
- **(Ainda) não é um runtime web nativo baseado em JS.** Por padrão, o AffineUI
  não executa JavaScript — esta é uma escolha deliberada para o release atual
  para manter a história simples e o binário pequeno. Suporte a JS + React
  está no roadmap e chegará como uma extensão opt-in,
  tornando o AffineUI um substituto para Electron de primeira classe para times que
  queiram manter sua base de código web.

---

## Status

**Alpha.** O renderizador principal, layout, cascade e reconciliador já são utilizáveis
para UIs reais hoje — dashboards Bootstrap, layouts DCC personalizados e UIs completas de
ferramentas renderizam. A cobertura de padrões é ampla, porém incompleta, existem casos
extremos e alguns recursos CSS ainda estão chegando. Espere bugs.
Espere reportar bugs. Não entregue para clientes ainda.

---

## Instalação

Escolha sua linguagem. Todos os quatro bindings ficam sobre o mesmo núcleo C++.

**Python** — como Gradio, mas nativo:

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

**C++ (drop-in, zero dependências):** pegue os dois arquivos
[`dist/affineui.h`](../../dist/affineui.h) e [`dist/affineui.cpp`](../../dist/affineui.cpp),
adicione-os ao seu projeto e compile `affineui.cpp` uma vez como C++20.
Esse é o SDK inteiro — sem gerenciador de pacotes, sem árvore de submódulos, sem DLL.

Plataformas suportadas: Windows, macOS, Linux, iOS, Android, WebGL. Veja
[docs/BUILDING.md](../BUILDING.md) para notas específicas de plataforma e
pré-requisitos para compilar a partir do código-fonte.

---

## Olá, mundo

Duas variantes estão presentes em cada linguagem:

- **API de componentes** — descreva a UI como uma árvore de widgets tipados
  (`heading`, `button`, `slider`, …). Guiada pelo reconciliador; atualizações de estado
  acontecem in place. Esta é a API que você realmente quer.
- **HTML puro** — entregue ao motor uma string HTML. Este é o caminho drop-in
  para apps existentes e para qualquer coisa que você prefira criar como marcação.

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

HTML puro:

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

HTML puro:

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

HTML puro:

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

API de componentes (usando o drop-in amalgamado):

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

HTML puro:

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

## Um app modesto

Um pouco além do hello world — um contador com atualização ao vivo usando a
API de componentes. O estado vive na linguagem hospedeira; o reconciliador compara
sua view com o último frame e aplica patches na DOM. hover/focus/animation do CSS
continuam funcionando entre as atualizações.

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

Os equivalentes em Rust, C# e C++ são um-para-um — mesmos nomes de métodos,
mesmo comportamento do reconciliador. Veja [`examples/`](../../examples/) para
programas end-to-end maiores (um editor de fotos completo, um editor de jogos,
um sintetizador modular, um fatiador de impressão 3D) construídos com essa mesma API.

---

## HTML, CSS e design systems

O AffineUI é um renderizador HTML5 / CSS real, não um solver de layout com
formato de HTML. Tokenização de HTML e DOM vêm do [lexbor](https://github.com/lexbor/lexbor);
a matemática de flexbox vem do [Yoga](https://github.com/facebook/yoga);
a pintura passa pelo [NanoVG](https://github.com/memononen/nanovg) até o
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU).
O cascade, computed style, hit-testing, roteamento de seletores e o
reconciliador são nossos.

### Decius CSS é o padrão

O padrão embutido é o [Decius CSS](https://deciuscss.com) — um framework moderno
de componentes desenvolvido junto com o AffineUI e deliberadamente ajustado
para renderizar pixel-perfect neste motor. Se você usa a API de componentes
sem passar uma folha de estilos, você recebe o Decius, e ele simplesmente funciona.

### Traga seu próprio CSS

Decius é o padrão, **não uma exigência**. Qualquer CSS cujos seletores
correspondam aos nomes de classe emitidos pelo AffineUI vai estilizar os componentes embutidos,
e qualquer HTML feito à mão que você carregue via o caminho puro pode trazer qualquer CSS
que quiser. Bootstrap 4.6, classes utilitárias no estilo Tailwind e marcação de componentes
no estilo Ant renderizam prontos — veja
[`examples/01_bootstrap`](../../examples/01_bootstrap) e
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI renderizando CSS do Bootstrap">

*A biblioteca CSS Bootstrap 4.6 sem modificações renderizando nativamente — cards, navbars, botões e estados de hover/active, direto do `.min.css` real.*

### JavaScript de framework

Não há motor JS (veja [O que o AffineUI *não* é](#o-que-o-affineui-não-é)).
Interatividade de framework — dropdowns do Bootstrap, modais do Ant, e por
aí vai — é mapeada para comportamento nativo em C++ em vez de rodar como script.

---

## Embutindo em um app existente

Se você já tem uma janela e um frame loop, o AffineUI se conecta via um
adaptador em tempo de compilação. Dois já vêm embutidos — SDL2 e sokol_app — e um
caminho manual existe para todo o resto.

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

**Manual:** chame `ui.dispatch(event)` para cada evento de entrada e
`ui.render(width, height, dpi_scale)` uma vez por frame. Veja
[docs/EMBEDDING.md](../EMBEDDING.md) para a API completa.

Ambos os adaptadores dão a você HiDPI, mudanças de cursor, entrada de alta precisão e
roteamento de clique por seletor CSS sem nenhum código de cola.

---

## Executando os demos

Clone o repositório e compile com CMake — a pasta `examples/` traz
cerca de vinte aplicações end-to-end cobrindo ferramentas de jogos, UIs DCC
e compatibilidade com frameworks.

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

Alguns notáveis:

| Demo | Caminho | O que mostra |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | Menor programa funcional |
| Bootstrap dashboard | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | CSS real do Bootstrap 4.6, cards, navbars, tabelas |
| Editor de jogos | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Painéis encaixáveis, tree view, inspector |
| Dender (fatiador de impressão 3D) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Layout completo de aplicação com viewport |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | UI de emulador embutida em uma janela nativa |

Execute qualquer uma com o task runner — ele compila o necessário e a inicia (`./build.sh list` mostra todas). No Windows use `build.ps1`.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="Demo do editor de jogos">

*Demo do Decius Game Editor — painéis encaixáveis, tree view, inspector e barras de ferramentas na aparência CSS padrão Decius do AffineUI.*

Os bindings Python e Rust trazem seus próprios exemplos executáveis:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Docs

| Doc | O que tem nela |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Interior do motor — cascade, resolver, reconciliador, paint |
| [docs/BUILDING.md](../BUILDING.md) | Notas de build específicas por plataforma |
| [docs/EMBEDDING.md](../EMBEDDING.md) | Conectando o AffineUI a uma janela / frame loop existente |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Como os bindings Python / Rust / C# expõem o núcleo C++ |
| [docs/RELEASING.md](../RELEASING.md) | Processo de release, versionamento, comandos de instalação por registry |
| [docs/ROADMAP.md](../ROADMAP.md) | O que vem a seguir |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | Como contribuir |

---

## Chaves em tempo de compilação (drop-in C++)

| Macro | Uso |
| --- | --- |
| `AFFINEUI_WITH_SDL` | Habilita o adaptador SDL2. |
| `AFFINEUI_WITH_SOKOL` | Habilita o adaptador sokol_app. |
| `AFFINEUI_NO_IMM` | Omite a camada de modo imediato. |
| `AFFINEUI_NO_C_API` | Omite o C ABI (necessário para os bindings de linguagem). |
| `AFFINEUI_HTML_ENTITIES_FULL` | Inclui a tabela completa de entidades nomeadas HTML5 (padrão: compacta). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | Não emitir símbolos de implementação de sokol. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | Não emitir símbolos de implementação de NanoVG. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | Não emitir símbolos de implementação de stb_image. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | Não emitir símbolos de implementação de stb_truetype. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | Não emitir símbolos de implementação de fontstash. |

Defines do backend GL padrão: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`,
`AFFINEUI_BACKEND_GL`.

---

## Stack

| Camada | Biblioteca | Licença | Por quê |
| --- | --- | --- | --- |
| Parsing de HTML5 + CSS, DOM, matching de seletores | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Pedante quanto à spec, mantida |
| Matemática de flexbox | [Yoga](https://github.com/facebook/yoga) | MIT | Testada em batalha via React Native |
| Pintor vetorial 2D | [NanoVG](https://github.com/memononen/nanovg) | zlib | Traços / preenchimentos / gradientes / texto com antialiasing |
| Janelamento + abstração de GPU | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU por trás de uma API |
| Fontes | fontstash + stb_truetype | zlib / MIT | Cache de glifos baseado em atlas |
| Imagens raster | stb_image | MIT / público | Decodificação de `<img>` para PNG / JPG / GIF |

**Próprio:** cascade, computed style, adaptador de layout, driver de pintura,
hit-test, roteamento de clique, reconciliador, API de componentes. Tudo onde o
julgamento de design importa.

**Delegado:** tokenização de HTML5, tokenização de CSS3, matching de seletores,
matemática de flexbox, rasterização de glifos, pintura vetorial, janela + input.
Tudo onde conformidade com a spec e maturidade em batalha importam.

---

## Licença

[MIT](../../LICENSE). Componentes de terceiros vendidos junto mantêm suas licenças
originais — veja [external/README.md](../../external/README.md).
