<p align="center">
  <sub>
    <b>他の言語で読む:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="../ar/README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

# AffineUI

**小さく、HTML5準拠で、GPUアクセラレーションを備えたUIレンダラー。コンポーネントフレームワークを統合しており、ElectronやQtのネイティブな代替となります。**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="Dender 3Dプリントスライサーを動作させているAffineUI">

*Dender — AffineUIのデフォルトであるDecius CSSの見た目を示す3Dプリントスライサー。ドッキング可能なパネル、カスタムビューポート、そしてアプリ全体のレイアウトが、すべてネイティブでレンダリングされています。*

AffineUIは、本物のブラウザ相当のHTML/CSSレイアウトおよびペイントエンジンを、2ファイル構成のC++ドロップインとして、またPythonライブラリ（Gradioスタイル）、Rustクレート、C# NuGetパッケージとして提供します。1つのレンダラー、1つのコンポーネントAPI、4つのホスト言語です。120Hzで動作し、滑らかにアニメーションし、ブラウザもJavaScript VMも大きなフレームワークも埋め込みません。

主な用途は次のとおりです。

- **ゲームツールとゲーム内UI** — ランチャー、HUD、設定画面、デバッグパネル、エディタのオーバーレイなど。
- **DCC風のコンテンツ制作ツール** — Maya / Blender / ZBrush / DaVinci Proクラスの、密度の高いネイティブUIを備えたアプリケーション。
- **Qt / Electronの代替** — HTML/CSSによるオーサリングモデルは欲しいが、ブラウザやランタイムの肥大化は避けたい場合。
- **小さくクロスプラットフォームなネイティブUIを届けたいすべての人** — 寄せ集めではなく、きちんとデザインされて見える必要がある場合。

**既存のプロジェクトにそのまま組み込むだけで動作します。** すでにSDL2やsokol_appを使ったアプリケーションがあるなら、AffineUIの追加は2ファイルと配線用の呼び出し1つで済みます。ビルドシステムをフォークする必要も、ランタイムをダウンロードする必要も、別ウィンドウを用意する必要もありません。インクルードしてロードしてレンダリングすれば完了です。

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="AffineUIで構築されたスキューモーフィックなシンセデモ">

*スキューモーフィックなモジュラーシンセのデモ — 画面上のすべては標準のHTML + CSSです。ノブ、ケーブル、パネルのテクスチャ、アニメーションのすべてがそうです。カスタムウィジェットツールキットもプラグインもありません。*

---

## AffineUIが *ではない* もの

- **ウェブブラウザではありません。** ナビゲーション、Cookie、`fetch` / `XMLHttpRequest`、ウィンドウ管理はありません。ネイティブUIとして可能な限り小さく速いことを目指しており、「ブラウザ」の方向に肥大化させる機能はすべて意図的にスコープ外です。
- **骨抜きにしたHTML5ではありません。** AffineUIは最小サブセットではなく、*本物の* HTML5カバレッジを目指しています。Bootstrap、Tailwind、Antクラスのフレームワークが依存するような、特殊でないHTML5やCSSの機能が正しくレンダリングされない場合は、バグとして扱い、報告してください。ギャップは意図ではなく、未完成にすぎません。
- **セキュリティサンドボックスではありません。** AffineUIは *あなたの* HTMLと *あなたの* CSSをレンダリングするためのものです。オープンなウェブからダウンロードした信頼できないマークアップ、スタイルシート、スクリプトを **絶対に** 与えないでください。オリジンモデルも、CSPも、UIとホストプロセスの分離もありません。
- **何でも入りのフレームワークではありません。** AffineUIはUIをレンダリングします。`<video>`のデコード、任意シーンのGPU駆動アニメーション、3D、オーディオ、ネットワーキング、アセット管理は行いません。それらはあなたのアプリケーションの仕事であり、AffineUIはそれらを *向ける* 先です。
- **（今のところ）JSベースのネイティブなウェブランタイムではありません。** AffineUIはデフォルトではJavaScriptを実行しません。これは、話をシンプルにし、バイナリを小さく保つための現行リリースにおける意図的な選択です。JS + Reactサポートはロードマップに載っており、オプトインの拡張として提供される予定です。これにより、ウェブアプリのコードベースをそのまま維持したいチームにとって、AffineUIは第一級のElectron代替となります。

---

## ステータス

**アルファ。** コアレンダラー、レイアウト、カスケード、リコンサイラは、実際のUIで今日から利用可能です — Bootstrapダッシュボード、カスタムDCCレイアウト、フルツールUIまですべてレンダリングできます。標準のカバレッジは広範ですが未完成で、エッジケースが存在し、一部のCSS機能は現在も対応中です。バグに遭遇することを覚悟してください。バグを報告することを覚悟してください。まだ顧客に出荷しないでください。

レジストリへの公開は[リリースパイプライン](../RELEASING.md)の背後にあります。最初のタグ付きリリースが到着するまでは、ソースからビルドできます（後述）。

---

## インストール

言語を選んでください。4つのバインディングはすべて同じC++コアの上に載っています。

**Python** — Gradioのようですが、ネイティブです。

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

**C++（ドロップイン、依存関係ゼロ）:** 2つのファイル [`dist/affineui.h`](../../dist/affineui.h) と [`dist/affineui.cpp`](../../dist/affineui.cpp) を取得し、プロジェクトに追加して、`affineui.cpp` をC++20として一度コンパイルするだけです。これがSDK全体です — パッケージマネージャも、サブモジュールツリーも、DLLもありません。

サポート対象プラットフォーム: Windows、macOS、Linux、iOS、Android、WebGL。プラットフォーム固有の注意事項やソースからビルドするための前提条件については、[docs/BUILDING.md](../BUILDING.md) を参照してください。

---

## Hello, world

すべての言語で2つの流儀を提供しています。

- **コンポーネントAPI** — UIを型付きウィジェット（`heading`、`button`、`slider` など）のツリーとして記述します。リコンサイラで駆動され、状態はインプレースで更新されます。実際に使いたくなるのはこちらのAPIです。
- **生のHTML** — エンジンにHTMLの文字列を渡します。既存アプリのドロップイン用の経路であり、マークアップとして書きたいものすべてに向いています。

### Python

コンポーネントAPI:

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

生のHTML:

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

コンポーネントAPI:

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

生のHTML:

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

コンポーネントAPI:

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

生のHTML:

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

コンポーネントAPI（アマルガメートされたドロップインを使用）:

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

生のHTML:

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

## ささやかなアプリ

hello worldからもう少し進んだ例として、コンポーネントAPIを使ったライブ更新のカウンターです。状態はホスト言語側で保持され、リコンサイラが前フレームのビューとの差分を取ってDOMにパッチを当てます。CSSのホバー/フォーカス/アニメーションは更新の合間も動き続けます。

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

Rust、C#、C++での等価な実装は1対1で対応します — 同じメソッド名、同じリコンサイラの挙動です。同じAPIで構築された、より大規模なエンドツーエンドのプログラム（フル機能のフォトエディタ、ゲームエディタ、モジュラーシンセ、3Dプリントスライサー）は [`examples/`](../../examples/) を参照してください。

---

## HTML、CSS、デザインシステム

AffineUIはHTML風のレイアウトソルバではなく、本物のHTML5 / CSSレンダラーです。HTMLトークン化とDOMは [lexbor](https://github.com/lexbor/lexbor) から、flexboxの計算は [Yoga](https://github.com/facebook/yoga) から取得しています。ペイントは [NanoVG](https://github.com/memononen/nanovg) を経て [sokol_gfx](https://github.com/floooh/sokol)（Metal / D3D11 / GL / WebGPU）へと流れます。カスケード、算出スタイル、ヒットテスト、セレクタルーティング、そしてリコンサイラは自前です。

### デフォルトはDecius CSS

同梱のデフォルトは [Decius CSS](https://deciuscss.com) です。AffineUIと並行して開発された最新のコンポーネントフレームワークで、このエンジン上でピクセル単位で完璧にレンダリングされるよう意図的に調整されています。スタイルシートを渡さずにコンポーネントAPIを使えば、Deciusが得られ、そのまま動作します。

### 独自のCSSを持ち込む

Deciusはデフォルトですが、**必須ではありません**。AffineUIが出力するクラス名にセレクタが一致するCSSはすべて組み込みコンポーネントをスタイリングでき、生のパスからロードした手書きのHTMLは、好きなCSSをいくらでも持ち込むことができます。Bootstrap 4.6、Tailwind風のユーティリティクラス、Ant風のコンポーネントマークアップはすべて、そのままレンダリングできます — [`examples/01_bootstrap`](../../examples/01_bootstrap) と [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) を参照してください。

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="Bootstrap CSSをレンダリングするAffineUI">

*無改造のBootstrap 4.6 CSSライブラリがネイティブにレンダリングされています — カード、ナビゲーションバー、ボタン、ホバー/アクティブ状態のいずれも、本物の `.min.css` からそのまま動作しています。*

### フレームワークのJavaScript

JSエンジンはありません（[AffineUIが *ではない* もの](#affineuiが-ではない-もの)を参照）。フレームワークのインタラクティビティ — Bootstrapのドロップダウン、Antのモーダルなど — は、スクリプトとして実行される代わりに、ネイティブなC++の振る舞いにマッピングされます。

---

## 既存アプリへの組み込み

すでにウィンドウとフレームループを持っている場合、AffineUIはコンパイル時のアダプタ経由で配線できます。組み込み済みのものが2つ（SDL2とsokol_app）あり、それ以外のすべてに対しては手動の経路が存在します。

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

**手動:** 各入力イベントに対して `ui.dispatch(event)` を呼び、フレームごとに1回 `ui.render(width, height, dpi_scale)` を呼びます。完全なAPIは [docs/EMBEDDING.md](../EMBEDDING.md) を参照してください。

どちらのアダプタも、グルーコードなしでHiDPI、カーソルの変更、高精度入力、そしてCSSセレクタによるクリックルーティングを提供します。

---

## デモの実行

リポジトリをクローンしてCMakeでビルドしてください — `examples/` フォルダには、ゲームツール、DCC UI、フレームワーク互換性をカバーするおよそ20本のエンドツーエンドアプリケーションが同梱されています。

```bash
git clone https://github.com/benjcooley/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

注目すべきもの:

| デモ | パス | 見どころ |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | 最小の動作プログラム |
| Bootstrap ダッシュボード | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | 本物のBootstrap 4.6 CSS、カード、ナビゲーションバー、テーブル |
| ゲームエディタ | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | ドッキング可能なパネル、ツリービュー、インスペクタ |
| Dender (3Dプリントスライサー) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | ビューポート付きのフルアプリレイアウト |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | ネイティブウィンドウに埋め込まれたエミュレータUI |

タスクランナーで実行できます。必要なものをビルドして起動します（`./build.sh list` で一覧表示）。Windows では `build.ps1` を使います。

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="ゲームエディタのデモ">

*Decius Game Editorデモ — AffineUIのデフォルトであるDecius CSSの見た目における、ドッキング可能なパネル、ツリービュー、インスペクタ、ツールバー。*

PythonとRustのバインディングにも、独自の実行可能なサンプルが同梱されています。

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## ドキュメント

| ドキュメント | 内容 |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | エンジン内部 — カスケード、リゾルバ、リコンサイラ、ペイント |
| [docs/BUILDING.md](../BUILDING.md) | プラットフォーム固有のビルド手順 |
| [docs/EMBEDDING.md](../EMBEDDING.md) | 既存のウィンドウ / フレームループへのAffineUIの配線 |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | Python / Rust / C#のバインディングがC++コアをどのように公開するか |
| [docs/RELEASING.md](../RELEASING.md) | リリースプロセス、バージョニング、レジストリごとのインストールコマンド |
| [docs/ROADMAP.md](../ROADMAP.md) | 次に投入予定の内容 |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | コントリビュートの方法 |

---

## コンパイル時スイッチ（C++ドロップイン）

| マクロ | 用途 |
| --- | --- |
| `AFFINEUI_WITH_SDL` | SDL2アダプタを有効化。 |
| `AFFINEUI_WITH_SOKOL` | sokol_appアダプタを有効化。 |
| `AFFINEUI_NO_IMM` | 即時モードレイヤーを省略。 |
| `AFFINEUI_NO_C_API` | C ABIを省略（言語バインディングでは必要）。 |
| `AFFINEUI_HTML_ENTITIES_FULL` | 完全なHTML5名前付きエンティティテーブルを含める（デフォルト: コンパクト）。 |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | sokolの実装シンボルを出力しない。 |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | NanoVGの実装シンボルを出力しない。 |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | stb_imageの実装シンボルを出力しない。 |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | stb_truetypeの実装シンボルを出力しない。 |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | fontstashの実装シンボルを出力しない。 |

デフォルトのGLバックエンドの定義: `SOKOL_GLCORE`、`SOKOL_NO_ENTRY`、`AFFINEUI_BACKEND_GL`。

---

## 技術スタック

| レイヤー | ライブラリ | ライセンス | 採用理由 |
| --- | --- | --- | --- |
| HTML5 + CSSのパース、DOM、セレクタマッチング | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | 仕様に厳密で、メンテナンスされている |
| flexboxの計算 | [Yoga](https://github.com/facebook/yoga) | MIT | React Native経由で実戦検証済み |
| 2Dベクタペインタ | [NanoVG](https://github.com/memononen/nanovg) | zlib | アンチエイリアシング済みのストローク / 塗りつぶし / グラデーション / テキスト |
| ウィンドウ管理 + GPU抽象化 | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPUを単一のAPIで |
| フォント | fontstash + stb_truetype | zlib / MIT | アトラスベースのグリフキャッシュ |
| ラスター画像 | stb_image | MIT / public | `<img>` のPNG / JPG / GIFデコード |

**自前:** カスケード、算出スタイル、レイアウトアダプタ、ペイントドライバ、ヒットテスト、クリックルーティング、リコンサイラ、コンポーネントAPI。デザインの判断が重要になる箇所はすべて自前で持っています。

**委譲:** HTML5トークン化、CSS3トークン化、セレクタマッチング、flexboxの計算、グリフラスタライズ、ベクタペイント、ウィンドウ + 入力。仕様準拠と実戦検証が重要になる箇所はすべて委譲しています。

---

## ライセンス

[MIT](../../LICENSE)。同梱されたサードパーティコンポーネントは、それぞれ元のライセンスを保持します — [external/README.md](../../external/README.md) を参照してください。
