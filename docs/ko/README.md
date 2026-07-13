<p align="center">
  <sub>
    <b>다른 언어로 보기:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**작고 HTML5 표준을 준수하며 GPU 가속을 지원하는 UI 렌더러 — 컴포넌트 프레임워크가 통합된, Electron 및 Qt의 네이티브 대체품.**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI로 실행 중인 Dender 3D 프린트 슬라이서">

*Dender — AffineUI의 기본 Decius CSS 룩을 보여주는 3D 프린트 슬라이서. 도킹된 패널, 커스텀 뷰포트, 그리고 전체 앱 레이아웃까지 모두 네이티브로 렌더링됩니다.*

AffineUI는 실제 브라우저 스타일의 HTML/CSS 레이아웃 및 페인트 엔진을 두 개 파일로 된 C++ 드롭인, Python 라이브러리(Gradio 스타일), Rust 크레이트, 그리고 C# NuGet 패키지로 제공합니다. 하나의 렌더러, 하나의 컴포넌트 API, 네 가지 호스트 언어. 120Hz로 동작하고 부드럽게 애니메이션되며, 브라우저나 JavaScript VM, 거대한 프레임워크를 임베드하지 않습니다.

용도:

- **게임 도구 및 인게임 UI** — 런처, HUD, 설정 화면, 디버그 패널, 에디터 오버레이.
- **DCC 스타일의 콘텐츠 도구** — Maya / Blender / ZBrush / DaVinci급 애플리케이션의 밀도 높은 네이티브 UI.
- **Qt / Electron 대체** — HTML/CSS 저작 모델은 원하지만 브라우저나 런타임의 무거움은 원치 않을 때.
- **작은 크로스 플랫폼 네이티브 UI를 배포하는 모든 사람** — 대충 짜맞춘 것이 아니라 디자인된 것처럼 보여야 할 때.

**기존 프로젝트에 그대로 넣기만 하면 동작합니다.** 이미 SDL2 또는 sokol_app 애플리케이션이 있다면, AffineUI를 추가하는 데 필요한 것은 파일 두 개와 연결 호출 한 번뿐입니다 — 빌드 시스템을 갈아엎을 필요도, 런타임을 다운로드할 필요도, 별도의 창을 띄울 필요도 없습니다. 포함하고, 로드하고, 렌더링하면 끝.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="AffineUI로 만든 스큐어모픽 신디사이저 데모">

*스큐어모픽 모듈러 신디사이저 데모 — 화면상의 모든 것이 표준 HTML + CSS입니다: 노브, 케이블, 패널 텍스처, 애니메이션. 커스텀 위젯 툴킷도, 플러그인도 사용하지 않았습니다.*

---

## AffineUI가 *아닌* 것

- **웹 브라우저가 아닙니다.** 내비게이션, 쿠키, `fetch` / `XMLHttpRequest`, 창 관리 기능이 없습니다. 목표는 네이티브 UI가 될 수 있는 만큼 작고 빠른 것이며 — "브라우저"에 가까워지게 만드는 기능은 모두 의도적으로 제외되었습니다.
- **HTML5의 축소판이 아닙니다.** AffineUI는 최소한의 하위 집합이 아니라 *진짜* HTML5 커버리지를 목표로 합니다. Bootstrap, Tailwind, Ant급 프레임워크가 의존하는 비-난해한 HTML5 또는 CSS 기능이 올바르게 렌더링되지 않는다면, 버그로 간주하고 신고해 주세요. 이런 부족함은 의도가 아니라 미완성일 뿐입니다.
- **보안 샌드박스가 아닙니다.** AffineUI는 *여러분의* HTML과 *여러분의* CSS를 렌더링하기 위한 것입니다. 공개된 웹에서 다운로드한 신뢰할 수 없는 마크업, 스타일시트, 스크립트를 **절대** 입력하지 마세요. 오리진 모델도, CSP도, UI와 호스트 프로세스 간의 격리도 없습니다.
- **모든 것이 포함된 프레임워크가 아닙니다.** AffineUI는 UI를 렌더링합니다. `<video>` 디코딩, 임의 씬에 대한 GPU 기반 애니메이션, 3D, 오디오, 네트워킹, 에셋 관리는 하지 않습니다. 이것들은 여러분 애플리케이션의 몫이며 — AffineUI는 그것들을 *가리키는* 대상입니다.
- **(아직은) JS 기반의 네이티브 웹 런타임이 아닙니다.** 기본적으로 AffineUI는 JavaScript를 실행하지 않습니다 — 이는 현재 릴리즈에서 스토리를 단순하게 유지하고 바이너리를 작게 만들기 위한 의도적인 선택입니다. JS + React 지원은 로드맵에 있으며 옵트인 확장 기능으로 제공될 예정입니다. 이는 웹 앱 코드베이스를 유지하고자 하는 팀에게 AffineUI를 일급 Electron 대체품으로 만들어 줄 것입니다.

---

## 상태

**알파.** 코어 렌더러, 레이아웃, 캐스케이드, 리컨실러는 이미 실제 UI에 사용 가능한 수준입니다 — Bootstrap 대시보드, 커스텀 DCC 레이아웃, 전체 도구 UI가 모두 렌더링됩니다. 표준 커버리지는 광범위하지만 완전하지는 않고, 엣지 케이스가 존재하며, 일부 CSS 기능은 아직 추가되는 중입니다. 버그가 있을 수 있습니다. 버그를 신고해야 할 수도 있습니다. 아직 고객에게 배포하지는 마세요.

---

## 설치

원하는 언어를 선택하세요. 네 개의 바인딩 모두 동일한 C++ 코어 위에 있습니다.

**Python** — Gradio와 비슷하지만 네이티브:

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

**C++ (드롭인, 의존성 없음):** [`dist/affineui.h`](../../dist/affineui.h)와 [`dist/affineui.cpp`](../../dist/affineui.cpp) 두 파일을 받아서 프로젝트에 추가하고, `affineui.cpp`를 C++20으로 한 번 컴파일하세요. 그것이 SDK 전부입니다 — 패키지 매니저도, 서브모듈 트리도, DLL도 없습니다.

지원 플랫폼: Windows, macOS, Linux, iOS, Android, WebGL. 플랫폼별 참고 사항과 소스로부터 빌드하기 위한 사전 요구 사항은 [docs/BUILDING.md](../BUILDING.md)를 참조하세요.

---

## Hello, world

모든 언어에는 두 가지 방식이 제공됩니다:

- **컴포넌트 API** — UI를 타입이 지정된 위젯(`heading`, `button`, `slider`, …)의 트리로 기술합니다. 리컨실러 기반이며, 상태가 제자리에서 업데이트됩니다. 이것이 여러분이 실제로 원하는 API입니다.
- **원시 HTML** — 엔진에 HTML 문자열을 넘깁니다. 기존 앱을 위한 드롭인 경로이며, 마크업으로 저작하는 편이 나은 것들을 위한 것입니다.

### Python

컴포넌트 API:

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

원시 HTML:

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

컴포넌트 API:

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

원시 HTML:

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

컴포넌트 API:

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

원시 HTML:

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

컴포넌트 API (통합 드롭인 사용):

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

원시 HTML:

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

## 소박한 앱

hello world보다 조금 더 나아간 예제 — 컴포넌트 API를 사용한 실시간 업데이트 카운터입니다. 상태는 호스트 언어 안에 있으며, 리컨실러가 여러분의 뷰를 이전 프레임과 비교하여 DOM에 패치를 적용합니다. CSS 호버/포커스/애니메이션은 업데이트 사이에도 계속 동작합니다.

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

Rust, C#, C++ 버전도 일대일로 동일합니다 — 같은 메서드 이름, 같은 리컨실러 동작. 동일한 API로 만들어진 더 큰 엔드투엔드 프로그램(완전한 사진 편집기, 게임 에디터, 모듈러 신디사이저, 3D 프린트 슬라이서)에 대해서는 [`examples/`](../../examples/)를 참조하세요.

---

## HTML, CSS, 그리고 디자인 시스템

AffineUI는 HTML 모양의 레이아웃 솔버가 아니라 실제 HTML5 / CSS 렌더러입니다. HTML 토큰화와 DOM은 [lexbor](https://github.com/lexbor/lexbor)에서 오고, 플렉스박스 계산은 [Yoga](https://github.com/facebook/yoga)에서 오며, 페인트는 [NanoVG](https://github.com/memononen/nanovg)를 거쳐 [sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU)로 이어집니다. 캐스케이드, 계산된 스타일, 히트 테스트, 셀렉터 라우팅, 그리고 리컨실러는 우리가 직접 만든 것입니다.

### Decius CSS가 기본입니다

기본으로 번들되는 것은 [Decius CSS](https://deciuscss.com)입니다 — AffineUI와 함께 개발된 현대적인 컴포넌트 프레임워크로, 이 엔진에서 픽셀 단위로 완벽하게 렌더링되도록 의도적으로 튜닝되었습니다. 스타일시트를 넘기지 않고 컴포넌트 API를 사용하면 Decius가 적용되며, 그대로 잘 동작합니다.

### 원하는 CSS 가져오기

Decius는 기본값일 뿐 **필수 요구 사항은 아닙니다**. AffineUI가 방출하는 클래스 이름에 셀렉터가 매칭되는 어떤 CSS라도 내장 컴포넌트에 스타일을 적용할 수 있고, 원시 경로로 로드하는 직접 작성한 HTML은 원하는 어떤 CSS든 가져올 수 있습니다. Bootstrap 4.6, Tailwind 스타일의 유틸리티 클래스, Ant 스타일의 컴포넌트 마크업이 모두 그대로 렌더링됩니다 — [`examples/01_bootstrap`](../../examples/01_bootstrap)과 [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard)를 참조하세요.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="Bootstrap CSS를 렌더링하는 AffineUI">

*수정 없는 Bootstrap 4.6 CSS 라이브러리가 네이티브로 렌더링되는 모습 — 카드, 내비바, 버튼, 그리고 호버/액티브 상태까지 실제 `.min.css`에서 곧바로 나온 것입니다.*

### 프레임워크 JavaScript

JS 엔진은 없습니다([AffineUI가 *아닌* 것](#affineui가-아닌-것) 참조). 프레임워크의 상호작용 — Bootstrap의 드롭다운, Ant의 모달 등 — 은 스크립트로 실행되는 대신 네이티브 C++ 동작으로 매핑됩니다.

---

## 기존 앱에 임베드하기

이미 창과 프레임 루프가 있다면, AffineUI는 컴파일 타임 어댑터를 통해 연결됩니다. 두 가지가 기본 제공됩니다 — SDL2와 sokol_app — 그리고 그 외의 모든 것을 위한 수동 경로도 존재합니다.

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

**수동:** 각 입력 이벤트마다 `ui.dispatch(event)`를 호출하고, 프레임마다 한 번씩 `ui.render(width, height, dpi_scale)`을 호출하세요. 전체 API는 [docs/EMBEDDING.md](../EMBEDDING.md)를 참조하세요.

두 어댑터 모두 접착 코드 없이 HiDPI, 커서 변경, 고정밀 입력, 그리고 CSS 셀렉터 클릭 라우팅을 제공합니다.

---

## 데모 실행하기

레포지토리를 클론하고 CMake로 빌드하세요 — `examples/` 폴더에는 게임 도구, DCC UI, 프레임워크 호환성을 다루는 약 스무 개 정도의 엔드투엔드 애플리케이션이 포함되어 있습니다.

```bash
git clone https://github.com/affineui/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

주목할 만한 것들:

| 데모 | 경로 | 무엇을 보여주는가 |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | 가장 작은 동작하는 프로그램 |
| Bootstrap 대시보드 | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | 실제 Bootstrap 4.6 CSS, 카드, 내비바, 테이블 |
| 게임 에디터 | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | 도킹된 패널, 트리 뷰, 인스펙터 |
| Dender (3D 프린트 슬라이서) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | 뷰포트가 있는 전체 앱 레이아웃 |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | 네이티브 창에 임베드된 에뮬레이터 UI |

태스크 러너로 실행합니다. 필요한 것을 빌드한 뒤 실행합니다(`./build.sh list` 로 전체 목록 확인). Windows 에서는 `build.ps1` 을 사용하세요.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="게임 에디터 데모">

*Decius Game Editor 데모 — AffineUI의 기본 Decius CSS 룩으로 만든 도킹된 패널, 트리 뷰, 인스펙터, 툴바.*

Python과 Rust 바인딩은 각자의 실행 가능한 예제를 함께 제공합니다:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## 문서

| 문서 | 내용 |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | 엔진 내부 — 캐스케이드, 리졸버, 리컨실러, 페인트 |
| [docs/BUILDING.md](../BUILDING.md) | 플랫폼별 빌드 참고 사항 |
| [docs/EMBEDDING.md](../EMBEDDING.md) | 기존 창 / 프레임 루프에 AffineUI 연결하기 |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Python / Rust / C# 바인딩이 C++ 코어를 노출하는 방식 |
| [docs/RELEASING.md](../RELEASING.md) | 릴리즈 프로세스, 버저닝, 레지스트리별 설치 명령 |
| [docs/ROADMAP.md](../ROADMAP.md) | 다음에 배포될 내용 |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | 기여 방법 |

---

## 컴파일 타임 스위치 (C++ 드롭인)

| 매크로 | 용도 |
| --- | --- |
| `AFFINEUI_WITH_SDL` | SDL2 어댑터 활성화. |
| `AFFINEUI_WITH_SOKOL` | sokol_app 어댑터 활성화. |
| `AFFINEUI_NO_IMM` | 이미디어트 모드 레이어 생략. |
| `AFFINEUI_NO_C_API` | C ABI 생략(언어 바인딩에 필요함). |
| `AFFINEUI_HTML_ENTITIES_FULL` | 전체 HTML5 명명 엔티티 테이블 포함(기본값: 간소 버전). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | sokol 구현 심볼을 방출하지 않음. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | NanoVG 구현 심볼을 방출하지 않음. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | stb_image 구현 심볼을 방출하지 않음. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | stb_truetype 구현 심볼을 방출하지 않음. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | fontstash 구현 심볼을 방출하지 않음. |

기본 GL 백엔드 정의: `SOKOL_GLCORE`, `SOKOL_NO_ENTRY`, `AFFINEUI_BACKEND_GL`.

---

## 스택

| 레이어 | 라이브러리 | 라이선스 | 이유 |
| --- | --- | --- | --- |
| HTML5 + CSS 파싱, DOM, 셀렉터 매칭 | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | 스펙에 엄격하고 유지보수됨 |
| 플렉스박스 계산 | [Yoga](https://github.com/facebook/yoga) | MIT | React Native를 통해 검증됨 |
| 2D 벡터 페인터 | [NanoVG](https://github.com/memononen/nanovg) | zlib | 안티에일리어싱된 스트로크 / 필 / 그라디언트 / 텍스트 |
| 윈도우 + GPU 추상화 | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU를 하나의 API 뒤에 둠 |
| 폰트 | fontstash + stb_truetype | zlib / MIT | 아틀라스 기반 글리프 캐시 |
| 래스터 이미지 | stb_image | MIT / public | PNG / JPG / GIF용 `<img>` 디코드 |

**자체 소유:** 캐스케이드, 계산된 스타일, 레이아웃 어댑터, 페인트 드라이버, 히트 테스트, 클릭 라우팅, 리컨실러, 컴포넌트 API. 디자인적 판단이 중요한 모든 것.

**위임:** HTML5 토큰화, CSS3 토큰화, 셀렉터 매칭, 플렉스박스 계산, 글리프 래스터화, 벡터 페인팅, 창 + 입력. 스펙 준수와 실전 검증이 중요한 모든 것.

---

## 라이선스

[MIT](../../LICENSE). 벤더된 서드파티 컴포넌트는 원래의 라이선스를 유지합니다 — [external/README.md](../../external/README.md)를 참조하세요.
