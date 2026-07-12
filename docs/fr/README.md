<p align="center">
  <sub>
    <b>Lire en :</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**Un moteur de rendu d'interface petit, conforme à HTML5 et accéléré par GPU, doté d'un framework de composants intégré — un remplaçant natif pour Electron et Qt.**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI exécutant le slicer d'impression 3D Dender">

*Dender — un slicer d'impression 3D montrant l'apparence par défaut de Decius CSS dans AffineUI. Panneaux ancrés, viewport personnalisé et mise en page pleine application, le tout rendu nativement.*

AffineUI livre un vrai moteur de mise en page et de peinture HTML/CSS façon navigateur sous la forme d'un drop-in C++ en deux fichiers, d'une bibliothèque Python (style Gradio), d'un crate Rust et d'un paquet NuGet C#. Un seul moteur de rendu, une seule API de composants, quatre langages hôtes. Il tourne à 120 Hz, anime en douceur, et n'embarque ni navigateur, ni VM JavaScript, ni framework volumineux.

Il est conçu pour :

- **Les outils de jeu et l'interface en jeu** — lanceurs, HUD, écrans de paramètres, panneaux de debug, superpositions d'éditeur.
- **Les outils de contenu de type DCC** — applications de la classe Maya / Blender / ZBrush / DaVinci Pro avec des interfaces natives denses.
- **Le remplacement de Qt / Electron** — quand vous voulez le modèle d'écriture HTML/CSS mais pas le navigateur ni la surcharge du runtime.
- **Toute personne livrant une petite interface native multiplateforme** qui doit avoir l'air conçue, pas assemblée à la va-vite.

**Il s'intègre à un projet existant et fonctionne simplement.** Si vous avez déjà une application SDL2 ou sokol_app, ajouter AffineUI se résume à deux fichiers et à un appel de raccordement — pas de fork du système de build, pas de téléchargement au runtime, pas de fenêtre séparée. Inclure, charger, rendre, terminé.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="Démo de synthétiseur skeuomorphe construite avec AffineUI">

*Une démo de synthé modulaire skeuomorphe — tout ce qui est à l'écran est du HTML + CSS standard : boutons rotatifs, câbles, textures de panneau, animations. Pas de boîte à outils de widgets personnalisée, pas de greffons.*

---

## Ce que AffineUI *n'est pas*

- **Pas un navigateur web.** Pas de navigation, pas de cookies, pas de `fetch` / `XMLHttpRequest`, pas de gestion de fenêtres. L'objectif est d'être aussi petit et rapide qu'une interface native peut l'être — toute fonctionnalité qui le ferait grossir vers un « navigateur » est délibérément hors périmètre.
- **Pas un HTML5 amputé.** AffineUI vise une *vraie* couverture HTML5, pas un sous-ensemble minimal. Si une fonctionnalité HTML5 ou CSS non-ésotérique dont dépendent les frameworks de la classe Bootstrap, Tailwind ou Ant ne s'affiche pas correctement, considérez-le comme un bug et signalez-le. Les manques sont de l'incomplétude, pas de l'intention.
- **Pas un bac à sable de sécurité.** AffineUI est destiné à rendre *votre* HTML et *votre* CSS. **Ne** lui donnez **jamais** de balisage, de feuilles de style ou de scripts non fiables téléchargés depuis le web ouvert. Il n'y a pas de modèle d'origine, pas de CSP, pas d'isolation entre l'interface et le processus hôte.
- **Pas un framework tout-inclus.** AffineUI rend des interfaces. Il ne fait pas le décodage `<video>`, l'animation pilotée par GPU de scènes arbitraires, la 3D, l'audio, le réseau ou la gestion des ressources. C'est le travail de votre application — AffineUI est ce vers quoi vous les pointez.
- **Pas (encore) un runtime web natif basé sur JS.** Par défaut, AffineUI n'exécute pas de JavaScript — c'est un choix délibéré pour la version actuelle afin de garder l'histoire simple et le binaire petit. Le support JS + React est sur la feuille de route et arrivera comme une extension opt-in, faisant d'AffineUI un remplaçant Electron de premier ordre pour les équipes qui veulent conserver leur base de code d'application web.

---

## Statut

**Alpha.** Le cœur du moteur de rendu, la mise en page, la cascade et le reconciler sont utilisables pour de vraies interfaces aujourd'hui — les tableaux de bord Bootstrap, les dispositions DCC personnalisées et les interfaces d'outils complètes s'affichent tous. La couverture des standards est large mais incomplète, il existe des cas limites, et certaines fonctionnalités CSS sont encore en cours d'intégration. Attendez-vous à des bugs. Attendez-vous à en signaler. Ne l'expédiez pas encore à des clients.

La publication vers les registres est derrière le [pipeline de publication](../RELEASING.md) ; jusqu'à la première release taguée, vous pouvez compiler depuis les sources (voir ci-dessous).

---

## Installation

Choisissez votre langage. Les quatre bindings reposent sur le même cœur C++.

**Python** — comme Gradio, mais natif :

```bash
pip install affineui
```

**Rust :**

```bash
cargo add affineui
```

**C# :**

```bash
dotnet add package AffineUI
```

**C++ (drop-in, zéro dépendance) :** récupérez les deux fichiers [`dist/affineui.h`](../../dist/affineui.h) et [`dist/affineui.cpp`](../../dist/affineui.cpp), ajoutez-les à votre projet et compilez `affineui.cpp` une seule fois en C++20. C'est l'intégralité du SDK — pas de gestionnaire de paquets, pas d'arbre de sous-modules, pas de DLL.

Plateformes supportées : Windows, macOS, Linux, iOS, Android, WebGL. Voir [docs/BUILDING.md](../BUILDING.md) pour les notes spécifiques à chaque plateforme et les prérequis pour compiler depuis les sources.

---

## Hello, world

Deux variantes sont livrées dans chaque langage :

- **API de composants** — décrivez l'interface comme un arbre de widgets typés (`heading`, `button`, `slider`, …). Piloté par un reconciler ; les mises à jour d'état se font en place. C'est l'API que vous voulez réellement.
- **HTML brut** — passez au moteur une chaîne HTML. C'est la voie d'intégration pour les applications existantes et pour tout ce que vous préférez écrire en balisage.

### Python

API de composants :

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

HTML brut :

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

API de composants :

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

HTML brut :

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

API de composants :

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

HTML brut :

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

API de composants (en utilisant le drop-in amalgamé) :

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

HTML brut :

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

## Une application modeste

Un peu plus qu'un hello world — un compteur mis à jour en direct via l'API de composants. L'état vit dans le langage hôte ; le reconciler compare votre vue à la dernière frame et applique les correctifs au DOM. Le survol, le focus et les animations CSS continuent de s'exécuter entre les mises à jour.

Python :

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

Les équivalents en Rust, C# et C++ sont un pour un — mêmes noms de méthodes, même comportement du reconciler. Voir [`examples/`](../../examples/) pour des programmes de bout en bout plus étoffés (un éditeur photo complet, un éditeur de jeu, un synthé modulaire, un slicer d'impression 3D) construits avec cette même API.

---

## HTML, CSS et systèmes de conception

AffineUI est un vrai moteur de rendu HTML5 / CSS, pas un résolveur de mise en page à l'apparence de HTML. La tokenisation HTML et le DOM viennent de [lexbor](https://github.com/lexbor/lexbor) ; les calculs de flexbox viennent de [Yoga](https://github.com/facebook/yoga) ; la peinture passe par [NanoVG](https://github.com/memononen/nanovg) puis [sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU). La cascade, le style calculé, le hit-testing, le routage de sélecteurs et le reconciler sont les nôtres.

### Decius CSS est la valeur par défaut

L'ensemble par défaut fourni est [Decius CSS](https://deciuscss.com) — un framework de composants moderne développé aux côtés d'AffineUI et délibérément accordé pour un rendu pixel-perfect sur ce moteur. Si vous utilisez l'API de composants sans passer de feuille de style, vous obtenez Decius, et cela fonctionne simplement.

### Apportez votre propre CSS

Decius est la valeur par défaut, **pas une obligation**. Tout CSS dont les sélecteurs correspondent aux noms de classe qu'AffineUI émet stylera les composants intégrés, et tout HTML écrit à la main que vous chargez via la voie brute peut apporter le CSS qu'il souhaite. Bootstrap 4.6, les classes utilitaires façon Tailwind et le balisage de composants façon Ant s'affichent tous dès la sortie de la boîte — voir [`examples/01_bootstrap`](../../examples/01_bootstrap) et [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI rendant du CSS Bootstrap">

*La bibliothèque CSS Bootstrap 4.6 non modifiée rendue nativement — cartes, barres de navigation, boutons, et états survol/actif, directement depuis le vrai `.min.css`.*

### Le JavaScript des frameworks

Il n'y a pas de moteur JS (voir [Ce que AffineUI *n'est pas*](#ce-que-affineui-nest-pas)). L'interactivité des frameworks — les dropdowns de Bootstrap, les modales d'Ant, et ainsi de suite — est mappée sur un comportement natif C++ au lieu de s'exécuter comme un script.

---

## Intégration dans une application existante

Si vous avez déjà une fenêtre et une boucle de frame, AffineUI s'y raccorde via un adaptateur à la compilation. Deux sont intégrés — SDL2 et sokol_app — et une voie manuelle existe pour tout le reste.

**SDL2 :**

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

**sokol_app :**

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

**Manuel :** appelez `ui.dispatch(event)` pour chaque événement d'entrée et `ui.render(width, height, dpi_scale)` une fois par frame. Voir [docs/EMBEDDING.md](../EMBEDDING.md) pour l'API complète.

Les deux adaptateurs vous fournissent le HiDPI, les changements de curseur, l'entrée haute précision et le routage de clics par sélecteur CSS sans aucun code de liaison.

---

## Exécution des démos

Clonez le dépôt et construisez avec CMake — le dossier `examples/` embarque une vingtaine d'applications de bout en bout couvrant les outils de jeu, les interfaces DCC et la compatibilité avec les frameworks.

```bash
git clone https://github.com/benjcooley/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

Les plus notables :

| Démo | Chemin | Ce qu'elle montre |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | Le plus petit programme qui fonctionne |
| Tableau de bord Bootstrap | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | CSS Bootstrap 4.6 réel, cartes, barres de navigation, tableaux |
| Éditeur de jeu | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Panneaux ancrés, vue arborescente, inspecteur |
| Dender (slicer d'impression 3D) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Mise en page pleine application avec viewport |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | Interface d'émulateur intégrée dans une fenêtre native |

Lancez n'importe laquelle avec le task runner — il compile ce qu'il faut et l'exécute (`./build.sh list` les affiche toutes). Sous Windows, utilisez `build.ps1`.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="Démo de l'éditeur de jeu">

*Démo de l'éditeur de jeu Decius — panneaux ancrés, vue arborescente, inspecteur et barres d'outils dans l'apparence par défaut Decius CSS d'AffineUI.*

Les bindings Python et Rust livrent leurs propres exemples exécutables :

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Documentation

| Document | Contenu |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Internes du moteur — cascade, résolveur, reconciler, peinture |
| [docs/BUILDING.md](../BUILDING.md) | Notes de compilation spécifiques à chaque plateforme |
| [docs/EMBEDDING.md](../EMBEDDING.md) | Intégration d'AffineUI dans une fenêtre / boucle de frame existante |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Comment les bindings Python / Rust / C# exposent le cœur C++ |
| [docs/RELEASING.md](../RELEASING.md) | Processus de release, versionnage, commandes d'installation par registre |
| [docs/ROADMAP.md](../ROADMAP.md) | Ce qui arrive ensuite |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | Comment contribuer |

---

## Interrupteurs à la compilation (drop-in C++)

| Macro | Utilisation |
| --- | --- |
| `AFFINEUI_WITH_SDL` | Active l'adaptateur SDL2. |
| `AFFINEUI_WITH_SOKOL` | Active l'adaptateur sokol_app. |
| `AFFINEUI_NO_IMM` | Omet la couche immediate-mode. |
| `AFFINEUI_NO_C_API` | Omet l'ABI C (nécessaire pour les bindings de langages). |
| `AFFINEUI_HTML_ENTITIES_FULL` | Inclut la table complète des entités nommées HTML5 (par défaut : compacte). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | N'émet pas les symboles d'implémentation de sokol. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | N'émet pas les symboles d'implémentation de NanoVG. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | N'émet pas les symboles d'implémentation de stb_image. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | N'émet pas les symboles d'implémentation de stb_truetype. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | N'émet pas les symboles d'implémentation de fontstash. |

Défauts du backend GL : `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`, `AFFINEUI_BACKEND_GL`.

---

## Pile technique

| Couche | Bibliothèque | Licence | Pourquoi |
| --- | --- | --- | --- |
| Parsing HTML5 + CSS, DOM, correspondance des sélecteurs | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Pédant sur la spec, maintenu |
| Calculs de flexbox | [Yoga](https://github.com/facebook/yoga) | MIT | Éprouvé via React Native |
| Peintre vectoriel 2D | [NanoVG](https://github.com/memononen/nanovg) | zlib | Traits / remplissages / dégradés / texte anti-crénelés |
| Fenêtrage + abstraction GPU | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU derrière une seule API |
| Polices | fontstash + stb_truetype | zlib / MIT | Cache de glyphes basé sur un atlas |
| Images matricielles | stb_image | MIT / domaine public | Décodage `<img>` pour PNG / JPG / GIF |

**Ce que nous possédons :** cascade, style calculé, adaptateur de mise en page, pilote de peinture, hit-test, routage des clics, reconciler, API de composants. Tout ce où le jugement de conception compte.

**Ce qui est délégué :** tokenisation HTML5, tokenisation CSS3, correspondance des sélecteurs, calculs de flexbox, rastérisation des glyphes, peinture vectorielle, fenêtre + entrées. Tout ce où la conformité aux specs et l'éprouvage au combat comptent.

---

## Licence

[MIT](../../LICENSE). Les composants tiers intégrés (vendored) conservent leurs licences d'origine — voir [external/README.md](../../external/README.md).
