<p align="center">
  <sub>
    <b>Читать на:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**Небольшой, HTML5-совместимый, GPU-ускоренный UI-рендерер со встроенным
фреймворком компонентов — нативная замена Electron и Qt.**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI, запускающий слайсер для 3D-печати Dender">

*Dender — слайсер для 3D-печати, демонстрирующий стандартный внешний вид AffineUI на CSS Decius. Стыкуемые панели, кастомный вьюпорт и полноценный лейаут приложения — всё отрисовано нативно.*

AffineUI предоставляет настоящий браузероподобный движок HTML/CSS-разметки
и отрисовки в виде C++-drop-in из двух файлов, Python-библиотеки (в стиле
Gradio), Rust-крейта и C#-пакета NuGet. Один рендерер, один API
компонентов, четыре хост-языка. Работает на 120 Гц, плавно анимирует и не
встраивает ни браузер, ни JavaScript-виртуальную машину, ни громоздкий
фреймворк.

Создан для:

- **Игровых инструментов и внутриигрового UI** — лаунчеры, HUD-ы, экраны
  настроек, отладочные панели, оверлеи редакторов.
- **DCC-инструментов для работы с контентом** — приложений уровня
  Maya / Blender / ZBrush / DaVinci Pro с насыщенным нативным UI.
- **Замены Qt / Electron** — когда нужна модель разработки на HTML/CSS,
  но без браузера и раздутого рантайма.
- **Всех, кто выпускает небольшой кроссплатформенный нативный UI**, который
  должен выглядеть продуманно, а не собранным на скорую руку.

**Встраивается в существующий проект и просто работает.** Если у вас уже
есть приложение на SDL2 или sokol_app, добавление AffineUI — это два
файла и один вызов для связки: никакого форка системы сборки, никакой
загрузки рантайма, никакого отдельного окна. Подключил, загрузил,
отрисовал — готово.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="Скевоморфное демо синтезатора, собранное на AffineUI">

*Демо скевоморфного модульного синтезатора — всё на экране это стандартные HTML + CSS: ручки, кабели, текстуры панелей, анимации. Никакого кастомного тулкита виджетов, никаких плагинов.*

---

## Чем AffineUI *не является*

- **Не веб-браузер.** Нет навигации, нет cookies, нет
  `fetch` / `XMLHttpRequest`, нет управления окнами. Цель — быть настолько
  же маленьким и быстрым, как нативный UI. Всё, что тянуло бы его в
  сторону «браузера», намеренно вынесено за рамки.
- **Не урезанный HTML5.** AffineUI нацелен на *реальное* покрытие HTML5,
  а не на минимальное подмножество. Если некая неэзотерическая фича
  HTML5 или CSS, на которую опираются фреймворки уровня Bootstrap,
  Tailwind или Ant, отрисовывается неверно — считайте это багом и
  заводите тикет. Это пробелы из-за неполноты, а не по замыслу.
- **Не песочница безопасности.** AffineUI предназначен для отрисовки
  *вашего* HTML и *вашего* CSS. **Никогда** не скармливайте ему
  недоверенную разметку, стили или скрипты, скачанные из открытой сети.
  Здесь нет модели origin, нет CSP, нет изоляции между UI и хост-процессом.
- **Не «всё-в-одном»-фреймворк.** AffineUI рендерит UI. Он не занимается
  декодированием `<video>`, GPU-анимацией произвольных сцен, 3D, звуком,
  сетью или управлением ассетами. Это задачи вашего приложения — AffineUI
  это то, на что вы их *направляете*.
- **Пока не JS-совместимый нативный веб-рантайм.** По умолчанию AffineUI
  не выполняет JavaScript — это осознанный выбор для текущего релиза,
  чтобы упростить рассказ и уменьшить бинарник. Поддержка JS + React
  есть в дорожной карте и появится как опциональное расширение, что
  сделает AffineUI полноценной заменой Electron для команд, которые
  хотят сохранить свою кодовую базу веб-приложения.

---

## Статус

**Alpha.** Ядро рендерера, лейаута, каскада и реконсилера уже пригодны
для реального UI — рендерятся дашборды на Bootstrap, кастомные DCC-лейауты
и полноценные UI инструментов. Покрытие стандартов широкое, но неполное,
есть краевые случаи, часть CSS-фич ещё в процессе. Ожидайте багов.
Ожидайте, что будете их заводить. Пока не отправляйте это клиентам.

Публикация в реестрах пакетов ждёт [пайплайна релиза](../RELEASING.md);
до первого тегированного релиза вы можете собрать из исходников (см. ниже).

---

## Установка

Выберите свой язык. Все четыре биндинга сидят на одном C++-ядре.

**Python** — как Gradio, но нативно:

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

**C++ (drop-in, ноль зависимостей):** возьмите два файла
[`dist/affineui.h`](../../dist/affineui.h) и [`dist/affineui.cpp`](../../dist/affineui.cpp),
добавьте их в проект и один раз скомпилируйте `affineui.cpp` как C++20.
Это весь SDK — никакого пакетного менеджера, никакого дерева
сабмодулей, никакой DLL.

Поддерживаемые платформы: Windows, macOS, Linux, iOS, Android, WebGL. См.
[docs/BUILDING.md](../BUILDING.md) для платформозависимых замечаний и
предварительных условий для сборки из исходников.

---

## Hello, world

В каждом языке идут две вариации:

- **Component API** — описываете UI как дерево типизированных виджетов
  (`heading`, `button`, `slider`, …). Реконсилер обновляет состояние на
  месте. Это тот API, которым вы на самом деле хотите пользоваться.
- **Сырой HTML** — передаёте движку HTML-строку. Это drop-in-путь для
  существующих приложений и для всего, что удобнее описывать разметкой.

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

Сырой HTML:

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

Сырой HTML:

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

Сырой HTML:

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

Component API (используется amalgamated drop-in):

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

Сырой HTML:

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

## Скромное приложение

Чуть больше, чем hello world — счётчик с живым обновлением на компонентном
API. Состояние живёт в хост-языке; реконсилер сравнивает ваше представление
с предыдущим кадром и патчит DOM. CSS-эффекты hover/focus/анимации
продолжают работать между обновлениями.

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

Эквиваленты на Rust, C# и C++ — один к одному: те же имена методов,
то же поведение реконсилера. См. [`examples/`](../../examples/) для более
крупных end-to-end-программ (полноценный фоторедактор, редактор игр,
модульный синтезатор, слайсер для 3D-печати), собранных на этом же API.

---

## HTML, CSS и дизайн-системы

AffineUI — это настоящий HTML5 / CSS-рендерер, а не просто HTML-образный
солвер лейаута. HTML-токенизация и DOM взяты из
[lexbor](https://github.com/lexbor/lexbor);
математика flexbox — из [Yoga](https://github.com/facebook/yoga);
отрисовка идёт через [NanoVG](https://github.com/memononen/nanovg) в
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU).
Каскад, вычисленный стиль, хит-тестинг, маршрутизация селекторов и
реконсилер — наши.

### Decius CSS по умолчанию

Идущий в комплекте дефолт — [Decius CSS](https://deciuscss.com) —
современный компонентный фреймворк, разрабатываемый параллельно с
AffineUI и целенаправленно настроенный на pixel-perfect-отрисовку на
этом движке. Если вы используете компонентный API без своего таблицы
стилей, вы получаете Decius, и он просто работает.

### Приносите свой CSS

Decius — это дефолт, **а не требование**. Любой CSS, чьи селекторы
совпадают с именами классов, которые эмитирует AffineUI, будет
стилизовать встроенные компоненты, а любой написанный вручную HTML,
загруженный через сырой путь, может приносить любой CSS, какой захочет.
Bootstrap 4.6, утилитарные классы в стиле Tailwind и разметка компонентов
в стиле Ant отрисовываются «из коробки» — см.
[`examples/01_bootstrap`](../../examples/01_bootstrap) и
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI отрисовывает Bootstrap CSS">

*Немодифицированная библиотека Bootstrap 4.6, рендерящаяся нативно — карточки, навбары, кнопки и состояния hover/active, прямо из настоящего `.min.css`.*

### JavaScript-фреймворков

JS-движка здесь нет (см. [Чем AffineUI *не является*](#чем-affineui-не-является)).
Интерактивность фреймворков — выпадающие меню Bootstrap, модалки Ant и
так далее — маппится на нативное C++-поведение вместо выполнения как
скрипт.

---

## Встраивание в существующее приложение

Если у вас уже есть окно и цикл кадров, AffineUI встраивается через
compile-time-адаптер. Два уже встроены — SDL2 и sokol_app — и есть
ручной путь для всего остального.

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

**Вручную:** вызывайте `ui.dispatch(event)` для каждого входного события и
`ui.render(width, height, dpi_scale)` один раз за кадр. См.
[docs/EMBEDDING.md](../EMBEDDING.md) для полного API.

Оба адаптера дают вам HiDPI, смену курсора, ввод высокой точности и
маршрутизацию кликов по CSS-селекторам без единой строчки glue-кода.

---

## Запуск демо

Склонируйте репозиторий и соберите через CMake — папка `examples/`
содержит примерно двадцать end-to-end-приложений, покрывающих игровые
инструменты, DCC UI и совместимость с фреймворками.

```bash
git clone https://github.com/benjcooley/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

Заметные из них:

| Демо | Путь | Что показывает |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | Минимальная работающая программа |
| Дашборд на Bootstrap | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | Настоящий Bootstrap 4.6 CSS, карточки, навбары, таблицы |
| Редактор игр | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | Стыкуемые панели, дерево, инспектор |
| Скевоморфный синтезатор | [`examples/14_decius_synth_skeuo`](../../examples/14_decius_synth_skeuo) | Кастомный скин с реалистичными текстурами + анимациями |
| Dender (слайсер для 3D-печати) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | Полноценный лейаут приложения с вьюпортом |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | UI эмулятора, встроенный в нативное окно |

Запустите любое из них прямо из директории сборки:

```bash
./build/examples/00_hello/hello
./build/examples/11_decius_game_editor/decius_game_editor
./build/examples/14_decius_synth_skeuo/decius_synth_skeuo
./build/examples/16_decius_dender/dender
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="Демо редактора игр">

*Демо Decius Game Editor — стыкуемые панели, дерево, инспектор и тулбары в стандартном виде AffineUI на CSS Decius.*

Python- и Rust-биндинги поставляются со своими собственными запускаемыми
примерами:

```bash
python bindings/python/examples/hello.py
python bindings/python/examples/component_gallery.py

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## Документация

| Документ | Что внутри |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | Внутреннее устройство движка — каскад, резолвер, реконсилер, отрисовка |
| [docs/BUILDING.md](../BUILDING.md) | Платформозависимые заметки по сборке |
| [docs/EMBEDDING.md](../EMBEDDING.md) | Встраивание AffineUI в существующее окно / цикл кадров |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Как биндинги Python / Rust / C# экспонируют C++-ядро |
| [docs/RELEASING.md](../RELEASING.md) | Процесс релиза, версионирование, команды установки по реестрам |
| [docs/ROADMAP.md](../ROADMAP.md) | Что выходит следующим |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | Как участвовать |

---

## Compile-time-переключатели (C++ drop-in)

| Макрос | Назначение |
| --- | --- |
| `AFFINEUI_WITH_SDL` | Включить адаптер SDL2. |
| `AFFINEUI_WITH_SOKOL` | Включить адаптер sokol_app. |
| `AFFINEUI_NO_IMM` | Убрать immediate-mode-слой. |
| `AFFINEUI_NO_C_API` | Убрать C ABI (нужен для языковых биндингов). |
| `AFFINEUI_HTML_ENTITIES_FULL` | Включить полную HTML5-таблицу именованных сущностей (по умолчанию: компактная). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | Не эмитировать символы реализации sokol. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | Не эмитировать символы реализации NanoVG. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | Не эмитировать символы реализации stb_image. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | Не эмитировать символы реализации stb_truetype. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | Не эмитировать символы реализации fontstash. |

Дефайны для GL-бэкенда по умолчанию: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`,
`AFFINEUI_BACKEND_GL`.

---

## Стек

| Слой | Библиотека | Лицензия | Зачем |
| --- | --- | --- | --- |
| Парсинг HTML5 + CSS, DOM, сопоставление селекторов | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | Педантично по спецификации, поддерживается |
| Математика flexbox | [Yoga](https://github.com/facebook/yoga) | MIT | Обкатан в бою через React Native |
| 2D-векторный отрисовщик | [NanoVG](https://github.com/memononen/nanovg) | zlib | Сглаженные обводки / заливки / градиенты / текст |
| Окна + абстракция GPU | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU за одним API |
| Шрифты | fontstash + stb_truetype | zlib / MIT | Кэш глифов на основе атласа |
| Растровые изображения | stb_image | MIT / public | Декодирование `<img>` для PNG / JPG / GIF |

**Своё:** каскад, вычисленный стиль, адаптер лейаута, драйвер отрисовки,
хит-тест, маршрутизация кликов, реконсилер, компонентный API. Всё, где
важно дизайн-суждение.

**Делегировано:** HTML5-токенизация, CSS3-токенизация, сопоставление
селекторов, математика flexbox, растеризация глифов, векторная
отрисовка, окна + ввод. Всё, где важны соответствие спецификации и
обкатка боем.

---

## Лицензия

[MIT](../../LICENSE). Заимствованные сторонние компоненты сохраняют свои
исходные лицензии — см. [external/README.md](../../external/README.md).
