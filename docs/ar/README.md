<p align="center">
  <sub>
    <b>اقرأ هذا بـ:</b>
    <a href="../../README.md">English</a> ·
    <a href="../zh-CN/README.md">中文</a> ·
    <a href="../es/README.md">Español</a> ·
    <a href="../hi/README.md">हिन्दी</a> ·
    <a href="README.md">العربية</a> ·
    <a href="../pt-BR/README.md">Português&nbsp;(BR)</a> ·
    <a href="../ru/README.md">Русский</a> ·
    <a href="../ja/README.md">日本語</a> ·
    <a href="../ko/README.md">한국어</a> ·
    <a href="../fr/README.md">Français</a> ·
    <a href="../de/README.md">Deutsch</a> ·
    <a href="../id/README.md">Indonesia</a>
  </sub>
</p>

<div dir="rtl" align="right">

# AffineUI

**عارض واجهات مستخدم صغير، متوافق مع HTML5، ومُسرَّع بواسطة GPU، مع إطار عمل مدمج للمكوّنات — بديل أصلي (native) لـ Electron و Qt.**

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI يشغّل مُشرِّح الطباعة ثلاثية الأبعاد Dender">

*Dender — مُشرِّح طباعة ثلاثية الأبعاد يُظهر المظهر الافتراضي لـ Decius CSS في AffineUI. لوحات مُثبَّتة (docked)، ومنفذ عرض مخصَّص، وتخطيط تطبيق كامل، كل ذلك مُقدَّم بشكل أصلي.*

يُقدِّم AffineUI محرّك تخطيط ورسم HTML/CSS حقيقيًا بأسلوب المتصفح، على هيئة ملفَّين قابلَين للإدراج المباشر بلغة C++، ومكتبة Python (بأسلوب Gradio)، وحزمة Rust crate، وحزمة C# NuGet. عارض واحد، وواجهة مكوّنات واحدة، وأربع لغات مضيفة. يعمل بمعدل 120 هرتز، ويُحرِّك بسلاسة، ولا يُضمِّن متصفحًا، ولا آلة افتراضية لـ JavaScript، ولا إطار عمل ضخمًا.

صُمِّم من أجل:

- **أدوات الألعاب وواجهات الاستخدام داخل الألعاب** — المُطلِقات، وشاشات HUD، وشاشات الإعدادات، ولوحات التصحيح، وطبقات المحرِّرات.
- **أدوات المحتوى بأسلوب DCC** — تطبيقات من فئة Maya / Blender / ZBrush / DaVinci Pro بواجهات أصلية كثيفة.
- **بديل لـ Qt / Electron** — عندما تريد نموذج التأليف بـ HTML/CSS دون المتصفح ودون تضخُّم زمن التشغيل.
- **أي شخص يشحن واجهة مستخدم أصلية صغيرة متعددة المنصات** يحتاج إلى أن تبدو مصمَّمة، لا مُلفَّقة على عجل.

**يندمج في مشروع قائم ويعمل مباشرة.** إذا كان لديك تطبيق يستخدم SDL2 أو sokol_app بالفعل، فإن إضافة AffineUI تعني ملفَّين ونداء توصيل واحد — لا انقسام في نظام البناء، ولا تنزيل لزمن تشغيل، ولا نافذة منفصلة. ضمِّن، حمِّل، اعرض، انتهيت.

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_skeuomorphic.png" width="720" alt="عرض توضيحي لمُركِّب صوتي معياري بأسلوب سكوومورفي مبني على AffineUI">

*عرض توضيحي لمُركِّب صوتي معياري بأسلوب سكوومورفي — كل ما على الشاشة هو HTML + CSS قياسي: مقابض، وكابلات، وخامات لوحات، ورسوم متحركة. لا مجموعة أدوات widget مخصَّصة، ولا إضافات.*

---

## ما ليس AffineUI

- **ليس متصفح ويب.** لا تنقل بين الصفحات، ولا ملفات ارتباط، ولا `fetch` / `XMLHttpRequest`، ولا إدارة نوافذ. الهدف أن يكون صغيرًا وسريعًا بقدر ما تكون عليه واجهات المستخدم الأصلية — وكل ميزة قد تدفعه في اتجاه "المتصفح" فهي خارج النطاق عن قصد.
- **ليس HTML5 منزوع الأنياب.** يستهدف AffineUI تغطية *حقيقية* لـ HTML5، لا مجموعة فرعية بسيطة. إذا لم يُعرض بشكل صحيح ميزة غير غامضة من HTML5 أو CSS تعتمد عليها أُطر من فئة Bootstrap أو Tailwind أو Ant، فاعتبرها خطأً وأبلغ عنها. الفجوات ناجمة عن نقص لا عن قصد.
- **ليس صندوق حماية أمني.** الغرض من AffineUI أن يعرض HTML الخاص *بك* و CSS الخاص *بك*. **لا** تُطعمه علامات أو أنماطًا أو نصوصًا برمجية غير موثوقة نُزِّلت من الويب المفتوح. لا يوجد نموذج مصدر (origin)، ولا CSP، ولا عزل بين واجهة الاستخدام والعملية المضيفة.
- **ليس إطار عمل شامل يحوي كل شيء.** يعرض AffineUI واجهات الاستخدام فحسب. لا يقوم بفك ترميز `<video>`، ولا بتحريك مشاهد اعتباطية عبر GPU، ولا 3D، ولا صوت، ولا اتصالات شبكية، ولا إدارة أصول. هذه من مهام تطبيقك — و AffineUI هو ما توجِّهها *إليه*.
- **ليس (بعد) زمن تشغيل ويب أصلي مبني على JS.** بشكل افتراضي، لا يُشغِّل AffineUI JavaScript — وهذا خيار مقصود في الإصدار الحالي لإبقاء القصة بسيطة والملف الثنائي صغيرًا. دعم JS + React مُدرج على خارطة الطريق وسيصل كامتداد اختياري، مما يجعل AffineUI بديلًا من الدرجة الأولى لـ Electron للفرق التي تريد الاحتفاظ بقاعدة كود تطبيق الويب لديها.

---

## الحالة

**ألفا.** العارض الأساسي، والتخطيط، وسلسلة الأنماط (cascade)، والـ reconciler صالحة للاستخدام في واجهات حقيقية اليوم — لوحات Bootstrap، وتخطيطات DCC مخصَّصة، وواجهات أدوات كاملة، جميعها تُعرض. تغطية المعايير واسعة لكنها غير مكتملة، وهناك حالات حافة، وبعض ميزات CSS لا تزال تنزل. توقع أخطاء. توقع أن تُبلِّغ عن أخطاء. لا تُطلقه للعملاء بعد.

---

## التثبيت

اختر لغتك. جميع الروابط الأربعة تستند إلى نواة C++ نفسها.

**Python** — كـ Gradio لكن أصلي:

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

**C++ (إسقاط مباشر، بلا اعتماديات):** خذ الملفَّين
[`dist/affineui.h`](../../dist/affineui.h) و [`dist/affineui.cpp`](../../dist/affineui.cpp)،
وأضفهما إلى مشروعك، وتَرْجَم `affineui.cpp` مرة واحدة كـ C++20.
هذا هو كامل الـ SDK — لا مدير حزم، ولا شجرة وحدات فرعية، ولا DLL.

المنصات المدعومة: Windows، و macOS، و Linux، و iOS، و Android، و WebGL. راجع
[docs/BUILDING.md](../BUILDING.md) للاطلاع على ملاحظات خاصة بكل منصة ومتطلبات البناء من المصدر.

---

## مرحبًا يا عالم

توجد نكهتان في كل لغة:

- **واجهة المكوّنات** — صِف واجهة الاستخدام كشجرة من widgets مُصنَّفة (`heading`، و `button`، و `slider`، …). مدفوعة بـ reconciler؛ تحديثات الحالة تُجرى في مكانها. هذه هي الواجهة التي تريدها فعلًا.
- **HTML خام** — ناوِل المحرِّك سلسلة HTML. هذا هو المسار القابل للإسقاط المباشر للتطبيقات القائمة ولأي شيء تفضِّل تأليفه كعلامات.

### Python

واجهة المكوّنات:

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

HTML خام:

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

واجهة المكوّنات:

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

HTML خام:

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

واجهة المكوّنات:

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

HTML خام:

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

واجهة المكوّنات (باستخدام الإسقاط المباشر المدمج):

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

HTML خام:

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

## تطبيق متواضع

أكثر بقليل من "hello world" — عدَّاد يُحدَّث حيًّا باستخدام واجهة المكوّنات. الحالة تعيش في اللغة المضيفة؛ يُقارن الـ reconciler عرضك مع الإطار السابق ويُرقِّع الـ DOM. يستمر تشغيل تأثيرات CSS من hover و focus وحركات بين التحديثات.

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

المكافئات في Rust و C# و C++ هي واحد-لواحد — نفس أسماء الدوال ونفس سلوك الـ reconciler. راجع [`examples/`](../../examples/) للاطلاع على برامج أكبر متكاملة من طرف إلى طرف (محرِّر صور كامل، ومحرِّر ألعاب، ومُركِّب صوتي معياري، ومُشرِّح طباعة ثلاثية الأبعاد) مبنية بالواجهة نفسها.

---

## HTML و CSS وأنظمة التصميم

AffineUI عارض HTML5 / CSS حقيقي، لا حلَّال تخطيط بشكل HTML. تأتي تجزئة (tokenization) HTML و DOM من [lexbor](https://github.com/lexbor/lexbor)؛
وحسابات flexbox تأتي من [Yoga](https://github.com/facebook/yoga)؛
ويمر الرسم عبر [NanoVG](https://github.com/memononen/nanovg) إلى
[sokol_gfx](https://github.com/floooh/sokol) (Metal / D3D11 / GL / WebGPU).
أما cascade، و computed style، و hit-testing، وتوجيه المحدِّدات (selectors)، والـ reconciler فمن صنعنا.

### Decius CSS هو الافتراضي

الافتراضي المُضمَّن هو [Decius CSS](https://deciuscss.com) — إطار عمل مكوّنات حديث طُوِّر بالتوازي مع AffineUI ومضبوط بعناية ليُقدَّم بدقة بكسل على هذا المحرِّك. إذا استخدمت واجهة المكوّنات دون تمرير ورقة أنماط، فستحصل على Decius، وسيعمل ببساطة.

### أحضر CSS الخاص بك

Decius هو الافتراضي، **وليس اشتراطًا**. أي CSS تتطابق محدِّداته مع أسماء الأصناف (class names) التي يُصدرها AffineUI سيصمم المكوّنات المدمجة، وأي HTML مكتوب يدويًا تحمِّله عبر المسار الخام يمكنه إحضار أي CSS يشاء. Bootstrap 4.6، والأصناف الأدواتية بأسلوب Tailwind، والعلامات على غرار مكوّنات Ant، جميعها تُعرض جاهزة — راجع
[`examples/01_bootstrap`](../../examples/01_bootstrap) و
[`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard).

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI يعرض Bootstrap CSS">

*مكتبة Bootstrap 4.6 CSS غير المُعدَّلة تُعرض بشكل أصلي — بطاقات، وأشرطة تنقُّل، وأزرار، وحالات hover و active، مباشرة من ملف `.min.css` الحقيقي.*

### JavaScript الخاص بأُطر العمل

لا يوجد محرك JS (راجع [ما ليس AffineUI](#ما-ليس-affineui)).
التفاعلية الخاصة بالأُطر — قوائم Bootstrap المنسدلة، ونوافذ Ant الحوارية، وما شابه — تُخطَّط إلى سلوك C++ أصلي بدلًا من تشغيلها كنص برمجي.

---

## التضمين في تطبيق قائم

إذا كان لديك بالفعل نافذة وحلقة إطارات، فإن AffineUI يتصل بها عبر مُهيِّئ (adapter) وقت الترجمة. اثنان مدمجان — SDL2 و sokol_app — ويوجد مسار يدوي لكل شيء آخر.

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

**يدوي:** استدعِ `ui.dispatch(event)` لكل حدث إدخال و
`ui.render(width, height, dpi_scale)` مرة في كل إطار. راجع
[docs/EMBEDDING.md](../EMBEDDING.md) للاطلاع على الواجهة البرمجية الكاملة.

كلا المُهيِّئَين يمنحك HiDPI، وتغييرات المؤشر، وإدخالًا عالي الدقة، وتوجيه نقر عبر محدِّدات CSS دون أي كود لاصق.

---

## تشغيل العروض التوضيحية

استنسخ المستودع وابنِ باستخدام CMake — يشحن مجلد `examples/` نحو عشرين تطبيقًا متكاملًا من طرف إلى طرف يغطي أدوات الألعاب، وواجهات DCC، وتوافق الأُطر.

```bash
git clone https://github.com/benjcooley/affineui.git
cd affineui
cmake -S . -B build -G Ninja
cmake --build build
```

من أبرزها:

| العرض التوضيحي | المسار | ما يُظهره |
| --- | --- | --- |
| Hello | [`examples/00_hello`](../../examples/00_hello) | أصغر برنامج عامل |
| لوحة Bootstrap | [`examples/10_bootstrap_dashboard`](../../examples/10_bootstrap_dashboard) | Bootstrap 4.6 CSS حقيقي، وبطاقات، وأشرطة تنقُّل، وجداول |
| محرِّر ألعاب | [`examples/11_decius_game_editor`](../../examples/11_decius_game_editor) | لوحات مثبَّتة، وعرض شجري، ومُفتِّش |
| Dender (مُشرِّح طباعة ثلاثية الأبعاد) | [`examples/16_decius_dender`](../../examples/16_decius_dender) | تخطيط تطبيق كامل مع منفذ عرض |
| Atari 2600 | [`examples/17_affine_2600`](../../examples/17_affine_2600) | واجهة محاكٍ مضمَّنة في نافذة أصلية |

شغّل أيًّا منها عبر مشغّل المهام — يبني ما تحتاجه الديمو ثم يشغّلها (`./build.sh list` يعرضها كلها). على Windows استخدم `build.ps1`.

```bash
./build.sh run hello
./build.sh run decius_game_editor
./build.sh run decius_dender
./build.sh list
```

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="عرض توضيحي لمحرِّر الألعاب">

*عرض توضيحي لـ Decius Game Editor — لوحات مثبَّتة، وعرض شجري، ومُفتِّش، وأشرطة أدوات في المظهر الافتراضي لـ Decius CSS في AffineUI.*

روابط Python و Rust تشحن أمثلتها القابلة للتشغيل:

```bash
./build.sh run py_hello
./build.sh run py_component_gallery

cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example hello
cargo run --manifest-path bindings/rust/affineui/Cargo.toml --example component_gallery
```

---

## الوثائق

| الوثيقة | ما تحتويه |
| --- | --- |
| [docs/ARCHITECTURE.md](../ARCHITECTURE.md) | داخليات المحرِّك — cascade، والمُحلِّل، والـ reconciler، والرسم |
| [docs/BUILDING.md](../BUILDING.md) | ملاحظات بناء خاصة بكل منصة |
| [docs/EMBEDDING.md](../EMBEDDING.md) | توصيل AffineUI بنافذة / حلقة إطارات قائمة |
| [docs/LANGUAGE_BINDINGS.md](../LANGUAGE_BINDINGS.md) | كيف تكشف روابط Python / Rust / C# نواة C++ |
| [docs/RELEASING.md](../RELEASING.md) | عملية الإصدار، والإصدارات، وأوامر التثبيت لكل سجل (registry) |
| [docs/ROADMAP.md](../ROADMAP.md) | ما القادم |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | كيفية المساهمة |

---

## مفاتيح وقت الترجمة (إسقاط مباشر C++)

| الماكرو | الاستخدام |
| --- | --- |
| `AFFINEUI_WITH_SDL` | تفعيل مُهيِّئ SDL2. |
| `AFFINEUI_WITH_SOKOL` | تفعيل مُهيِّئ sokol_app. |
| `AFFINEUI_NO_IMM` | حذف طبقة الوضع الفوري (immediate-mode). |
| `AFFINEUI_NO_C_API` | حذف واجهة C ABI (مطلوبة لروابط اللغات). |
| `AFFINEUI_HTML_ENTITIES_FULL` | تضمين جدول الكيانات المسمَّاة الكامل لـ HTML5 (الافتراضي: مضغوط). |
| `AFFINEUI_HOST_PROVIDES_SOKOL` | عدم إصدار رموز تنفيذ sokol. |
| `AFFINEUI_HOST_PROVIDES_NANOVG` | عدم إصدار رموز تنفيذ NanoVG. |
| `AFFINEUI_HOST_PROVIDES_STB_IMAGE` | عدم إصدار رموز تنفيذ stb_image. |
| `AFFINEUI_HOST_PROVIDES_STB_TRUETYPE` | عدم إصدار رموز تنفيذ stb_truetype. |
| `AFFINEUI_HOST_PROVIDES_FONTSTASH` | عدم إصدار رموز تنفيذ fontstash. |

تعريفات النهاية الخلفية الافتراضية لـ GL: `SOKOL_GLCORE`، و `SOKOL_NO_ENTRY`،
و `AFFINEUI_BACKEND_GL`.

---

## الحزمة التقنية (Stack)

| الطبقة | المكتبة | الرخصة | لماذا |
| --- | --- | --- | --- |
| تحليل HTML5 + CSS، و DOM، ومطابقة المحدِّدات | [lexbor](https://github.com/lexbor/lexbor) | Apache-2 | صارم مع المواصفات، ومُصان |
| حسابات flexbox | [Yoga](https://github.com/facebook/yoga) | MIT | مُختبَر ميدانيًا عبر React Native |
| رسَّام متجهات ثنائي الأبعاد | [NanoVG](https://github.com/memononen/nanovg) | zlib | حدود / تعبئات / تدرُّجات / نصوص مضادة للتسنُّن |
| النوافذ + تجريد GPU | [sokol](https://github.com/floooh/sokol) | zlib | Metal / D3D11 / GL / WebGPU خلف واجهة واحدة |
| الخطوط | fontstash + stb_truetype | zlib / MIT | ذاكرة تخزين مؤقت للجليفات مبنية على أطلس |
| صور نقطية | stb_image | MIT / public | فك ترميز `<img>` لـ PNG / JPG / GIF |

**مملوك:** cascade، و computed style، ومُهيِّئ التخطيط، ومحرِّك الرسم، و hit-test، وتوجيه النقر، والـ reconciler، وواجهة المكوّنات. كل ما يهم فيه الحكم التصميمي.

**مُفوَّض:** تجزئة HTML5، وتجزئة CSS3، ومطابقة المحدِّدات، وحسابات flexbox، وتنقيط الجليفات، والرسم المتجهي، والنوافذ + الإدخال. كل ما يهم فيه الالتزام بالمواصفات والاختبار الميداني.

---

## الرخصة

[MIT](../../LICENSE). تحتفظ المكوّنات الخارجية المُضمَّنة برخصها الأصلية — راجع [external/README.md](../../external/README.md).

</div>
