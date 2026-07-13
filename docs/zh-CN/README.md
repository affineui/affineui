<p align="center">
  <sub>
    <b>阅读其他语言版本：</b>
    <a href="../../README.md">English</a> ·
    <a href="README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
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

**一个小巧、兼容 HTML5、GPU 加速的 UI 渲染器，内置组件框架——用于原生取代 Electron 和 Qt。**

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI 运行 Dender 3D 打印切片器">

*Dender——一个 3D 打印切片器，展示了 AffineUI 默认的 Decius CSS 外观。停靠面板、自定义视口以及完整的应用布局，全部原生渲染。*

AffineUI 以两文件 C++ 即插即用库、Python 库（Gradio 风格）、Rust crate 以及 C# NuGet 包的形式，提供了一个真正的浏览器风格 HTML/CSS 布局与绘制引擎。一个渲染器、一套组件 API、四种宿主语言。它以 120 Hz 运行，动画流畅，并且不嵌入浏览器、JavaScript 虚拟机或庞大框架。

它面向以下场景构建：

- **游戏工具和游戏内 UI**——启动器、HUD、设置界面、调试面板、编辑器叠加层。
- **DCC 风格的内容创作工具**——像 Maya / Blender / ZBrush / DaVinci 那样的专业级应用，具有密集的原生 UI。
- **Qt / Electron 替代方案**——当你想要 HTML/CSS 的创作模型，但不想要浏览器或运行时的臃肿时。
- **任何需要交付小巧、跨平台原生 UI 的人**，且希望它看起来是精心设计的，而不是随便拼凑的。

**它可以直接接入现有项目并即刻工作。**如果你已经有一个 SDL2 或 sokol_app 应用程序，接入 AffineUI 只需两个文件加一次接线调用——无需分叉构建系统，无需运行时下载，无需独立窗口。包含、加载、渲染，搞定。

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="使用 AffineUI 构建的拟物化合成器演示">

*一个拟物化的模块化合成器演示——屏幕上的一切都是标准 HTML + CSS：旋钮、连接线、面板纹理、动画。没有自定义的控件工具包，没有插件。*

---

## AffineUI *不是*什么

- **不是一个网页浏览器。**没有导航，没有 cookie，没有 `fetch` / `XMLHttpRequest`，没有窗口管理。目标是让它在原生 UI 能达到的范围内保持尽可能小巧和快速——任何会让它朝"浏览器"方向膨胀的功能都被有意排除在范围之外。
- **不是被阉割的 HTML5。**AffineUI 追求*真正的* HTML5 覆盖率，而非最小子集。如果某个 Bootstrap、Tailwind 或 Ant 级别框架所依赖的非冷门 HTML5 或 CSS 特性没有正确渲染，请把它当作 bug 报告。这些缺口是尚未完成，而不是故意为之。
- **不是安全沙箱。**AffineUI 用于渲染*你的* HTML 和*你的* CSS。**永远不要**把从开放网络下载的、不受信任的标记、样式表或脚本喂给它。它没有源模型，没有 CSP，也没有 UI 与宿主进程之间的隔离。
- **不是包罗万象的框架。**AffineUI 负责渲染 UI。它不做 `<video>` 解码、任意场景的 GPU 驱动动画、3D、音频、网络或资产管理。那些是你的应用要做的事——AffineUI 是你把它们指向的对象。
- **（目前）不是基于 JS 的原生 Web 运行时。**默认情况下，AffineUI 不运行 JavaScript——在当前版本中这是一项刻意的取舍，为了让整体架构简单、二进制体积小。JS + React 支持在路线图上，会作为可选扩展落地，使 AffineUI 成为一流的 Electron 替代方案，让希望保留 Web 应用代码库的团队受益。

---

## 状态

**Alpha 阶段。**核心渲染器、布局、层叠和协调器（reconciler）今天已可用于真实 UI——Bootstrap 仪表盘、自定义 DCC 布局以及完整的工具 UI 都能渲染。标准覆盖广泛但尚未完整，存在边界情况，一些 CSS 特性仍在陆续落地。请预期会有 bug。请预期你会提交 bug。请暂时不要交付给客户。

---

## 安装

选择你的语言。四种绑定都构建在同一个 C++ 核心之上。

**Python**——像 Gradio 一样，但原生：

```bash
pip install affineui
```

**Rust：**

```bash
cargo add affineui
```

**C#：**

```bash
dotnet add package AffineUI
```

**C++（即插即用，零依赖）：**获取两个文件
[`dist/affineui.h`](../../dist/affineui.h) 与 [`dist/affineui.cpp`](../../dist/affineui.cpp)，
加入你的项目，并以 C++20 一次性编译 `affineui.cpp`。
这就是整个 SDK——没有包管理器，没有子模块树，没有 DLL。

支持的平台：Windows、macOS、Linux、iOS、Android、WebGL。请参阅
[docs/BUILDING.md](../BUILDING.md) 获取平台特定说明以及从源码构建的前置条件。

---

## Hello, world

每种语言都提供两种风格：

- **组件 API**——将 UI 描述为类型化控件的树（`heading`、`button`、`slider` 等）。协调器驱动；状态就地更新。这是你实际会想用的 API。
- **原始 HTML**——把 HTML 字符串直接交给引擎。这是现有应用的即插即用路径，也适用于任何你更愿意以标记形式编写的内容。

### Python

组件 API：

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

原始 HTML：

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

组件 API：

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

原始 HTML：

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

组件 API：

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

原始 HTML：

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

组件 API（使用合并后的即插即用文件）：

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

原始 HTML：

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

## 一个稍微复杂点的应用

比 hello world 多一点——一个使用组件 API 的实时更新计数器。状态存在于宿主语言中；协调器会将你的视图与上一帧进行 diff，并对 DOM 打补丁。CSS 的 hover / focus / 动画在更新之间持续运行。

Python：

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

Rust、C# 和 C++ 的等价实现是一对一的——相同的方法名、相同的协调器行为。请参阅
[`examples/`](../../examples/) 中使用同一 API 构建的更大型端到端程序（一个完整的照片编辑器、一个游戏编辑器、一个模块化合成器、一个 3D 打印切片器）。

---

## HTML、CSS 与设计系统

AffineUI 是一个真正的 HTML5 / CSS 渲染器，而非仅具备 HTML 外形的布局求解器。HTML 词法分析和 DOM 来自 [lexbor](https://github.com/lexbor/lexbor)；
flexbox 数学来自 [Yoga](https://github.com/facebook/yoga)；
绘制通过 [NanoVG](https://github.com/memononen/nanovg) 进入
[sokol_gfx](https://github.com/floooh/sokol)（Metal / D3D11 / GL / WebGPU）。
层叠、计算样式、命中测试、选择器路由以及协调器都是我们自己实现的。

### Decius CSS 是默认选择

内置的默认样式是 [Decius CSS](https://deciuscss.com)——一个与 AffineUI 并行开发的现代组件框架，经过刻意调优以便在本引擎上像素级完美渲染。如果你使用组件 API 而不传入样式表，你会得到 Decius，且它开箱即用。

### 使用你自己的 CSS

Decius 是默认，**但不是强制要求**。任何选择器与 AffineUI 发出的类名匹配的 CSS 都会为内置组件应用样式，而任何你通过原始路径加载的手写 HTML 都可以自带任意 CSS。Bootstrap 4.6、Tailwind 风格的实用类以及 Ant 风格的组件标记开箱即可渲染——见
[`examples/01_bootstrap`](../../examples/01_bootstrap) 与
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard)。

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI 渲染 Bootstrap CSS">

*未经修改的 Bootstrap 4.6 CSS 库正在原生渲染——卡片、导航栏、按钮，以及 hover / active 状态，全部直接来自真实的 `.min.css`。*

### 框架 JavaScript

没有 JS 引擎（参见 [AffineUI *不是*什么](#affineui-不是什么)）。
框架级的交互性——Bootstrap 的下拉菜单、Ant 的模态框等——被映射到原生 C++ 行为，而不是以脚本形式运行。

---

## 嵌入现有应用

如果你已经有一个窗口和一个帧循环，AffineUI 可通过一个编译期适配器接入。内置了两个适配器——SDL2 和 sokol_app——其他一切则可通过手动路径接入。

**SDL2：**

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

**sokol_app：**

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

**手动接入：**对每个输入事件调用 `ui.dispatch(event)`，每帧调用一次
`ui.render(width, height, dpi_scale)`。完整 API 请参阅
[docs/EMBEDDING.md](../EMBEDDING.md)。

两个适配器都自动提供 HiDPI、光标切换、高精度输入以及基于 CSS 选择器的点击路由，无需任何胶水代码。

---

## 运行演示

克隆仓库并使用 CMake 构建——`examples/` 文件夹中提供了大约 20 个端到端应用，覆盖游戏工具、DCC UI 以及框架兼容性。

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

值得关注的几个：

| 演示 | 路径 | 展示内容 |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | 最小可运行程序 |
| Bootstrap 仪表盘 | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | 真实的 Bootstrap 4.6 CSS、卡片、导航栏、表格 |
| 游戏编辑器 | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | 停靠面板、树视图、检查器 |
| Dender（3D 打印切片器） | [`examples/16_decius_dender`](../../examples/16_decius_dender) | 带视口的完整应用布局 |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | 嵌入原生窗口中的模拟器 UI |

用任务运行器运行任意示例 —— 它会构建所需内容并启动（`./build.sh list` 列出全部）。Windows 上请使用 `build.ps1`。

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_game_editor.png" width="720" alt="游戏编辑器演示">

*Decius Game Editor 演示——以 AffineUI 默认的 Decius CSS 外观呈现的停靠面板、树视图、检查器和工具栏。*

Python 与 Rust 绑定各自附带可运行示例：

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | 引擎内部——层叠、解析器、协调器、绘制 |
| [docs/BUILDING.md](../BUILDING.md) | 平台特定的构建说明 |
| [docs/EMBEDDING.md](../EMBEDDING.md) | 将 AffineUI 接入现有窗口 / 帧循环 |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Python / Rust / C# 绑定如何暴露 C++ 核心 |
| [docs/RELEASING.md](../RELEASING.md) | 发布流程、版本管理、各注册表安装命令 |
| [docs/ROADMAP.md](../ROADMAP.md) | 下一步要发布的内容 |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | 如何贡献 |

---

## 编译期开关（C++ 即插即用）

| 宏 | 用途 |
| --- | --- |
| `AFFINEUI_WITH_SDL` | 启用 SDL2 适配器。 |
| `AFFINEUI_WITH_SOKOL` | 启用 sokol_app 适配器。 |
| `AFFINEUI_NO_IMM` | 省略立即模式层。 |
| `AFFINEUI_NO_C_API` | 省略 C ABI（语言绑定需要）。 |
| `AFFINEUI_HTML_ENTITIES_FULL` | 包含完整的 HTML5 具名实体表（默认：紧凑版）。 |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | 不发出 sokol 的实现符号。 |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | 不发出 NanoVG 的实现符号。 |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | 不发出 stb_image 的实现符号。 |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | 不发出 stb_truetype 的实现符号。 |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | 不发出 fontstash 的实现符号。 |

默认 GL 后端定义：`SOKOL_GLCORE`、`SOKOL_NO_ENTRY`、
`AFFINEUI_BACKEND_GL`。

---

## 技术栈

| 层 | 库 | 许可证 | 为何选它 |
| --- | --- | --- | --- |
| HTML5 + CSS 解析、DOM、选择器匹配 | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | 严格遵循规范，持续维护 |
| Flexbox 数学 | [Yoga](https://github.com/facebook/yoga) | MIT | 经 React Native 久经考验 |
| 2D 矢量绘制器 | [NanoVG](https://github.com/memononen/nanovg) | zlib | 抗锯齿描边 / 填充 / 渐变 / 文本 |
| 窗口 + GPU 抽象 | [sokol](https://github.com/floooh/sokol) | zlib | 在一套 API 之下同时覆盖 Metal / D3D11 / GL / WebGPU |
| 字体 | fontstash + stb_truetype | zlib / MIT | 基于图集的字形缓存 |
| 光栅图像 | stb_image | MIT / 公有领域 | 用于 `<img>` 解码 PNG / JPG / GIF |

**自有：**层叠、计算样式、布局适配器、绘制驱动、命中测试、点击路由、协调器、组件 API。所有需要设计判断力的地方。

**委托：**HTML5 词法分析、CSS3 词法分析、选择器匹配、flexbox 数学、字形栅格化、矢量绘制、窗口 + 输入。所有需要规范符合性和久经考验的地方。

---

## 许可证

[MIT](../../LICENSE)。内置的第三方组件保留其原始许可证——参见 [external/README.md](../../external/README.md)。
