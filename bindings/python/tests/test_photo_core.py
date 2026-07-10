"""Headless tests for the raster photo core (photo_core.cpp).

Exercises the document model the way the app drives it: strokes,
selections, click tools, adjustments, history, and geometry ops. No
window is opened — everything runs against the CPU raster engine.
"""

import math

import photo_core


def make_doc(w=320, h=200):
    return photo_core.PhotoDocument(w, h)


def hex_at(doc, x, y):
    return doc.pick_color(x, y)


# ── document / layers ────────────────────────────────────────────────────


def test_new_document_has_background_layer():
    doc = make_doc()
    layers = doc.layers()
    assert len(layers) == 1
    assert layers[0].name == "Background"
    assert doc.width() == 320 and doc.height() == 200


def test_new_document_background_color():
    doc = make_doc()
    doc.new_document(100, 80, "#ff0000")
    assert hex_at(doc, 50, 40) == "#ff0000"
    assert doc.width() == 100 and doc.height() == 80


def test_sample_scene_layers_match_web():
    doc = photo_core.PhotoDocument(1280, 800)
    doc.load_sample_scene()
    names = [layer.name for layer in doc.layers()]
    assert names == ["Background", "Sun", "Mountains · Far",
                     "Mountains · Near", "Water", "“DECIUS”", "Paint"]
    # paint layer (top) is active, background is locked
    assert doc.active_index() == len(names) - 1
    assert doc.layers()[0].locked
    # history restarted at "Open"
    assert [h.name for h in doc.history_entries()] == ["Open"]
    # sky gradient: the top of the doc is the dark navy stop
    assert hex_at(doc, 640, 2) != "#000000"


def test_add_duplicate_delete_layer():
    doc = make_doc()
    lid = doc.add_layer("Paint")
    assert lid > 0
    assert [layer.name for layer in doc.layers()] == ["Background", "Paint"]
    assert doc.active_layer().name == "Paint"
    assert doc.duplicate_active()
    assert [layer.name for layer in doc.layers()] == [
        "Background", "Paint", "Paint copy"]
    assert doc.delete_active()
    assert [layer.name for layer in doc.layers()] == ["Background", "Paint"]
    # never delete the last layer
    assert doc.delete_active()
    assert not doc.delete_active()


def test_layer_reorder_and_move():
    doc = make_doc()
    doc.add_layer("A")
    doc.add_layer("B")
    assert [layer.name for layer in doc.layers()] == ["Background", "A", "B"]
    assert doc.set_active_index(1)  # A
    assert doc.move_active(1)
    assert [layer.name for layer in doc.layers()] == ["Background", "B", "A"]
    ids = {layer.name: layer.id for layer in doc.layers()}
    assert doc.reorder_layer(ids["A"], 0)
    assert [layer.name for layer in doc.layers()] == ["A", "Background", "B"]


def test_locked_layer_blocks_painting():
    doc = make_doc()
    doc.set_active_locked(True)
    assert not doc.begin_stroke("brush", 10, 10, color="#ff0000")
    assert not doc.fill_at(10, 10, "#ff0000")
    doc.set_active_locked(False)
    assert doc.begin_stroke("brush", 10, 10, color="#ff0000")
    doc.end_stroke()


# ── brush engine ─────────────────────────────────────────────────────────


def test_brush_stroke_paints_foreground():
    doc = make_doc()
    assert doc.begin_stroke("brush", 60, 60, size=30, hardness=1.0,
                            opacity=1.0, flow=1.0, color="#ff0000")
    doc.stroke_to(120, 60)
    doc.end_stroke()
    assert hex_at(doc, 90, 60) == "#ff0000"
    assert [h.name for h in doc.history_entries()][-1] == "Brush"


def test_eraser_erases():
    doc = make_doc()
    doc.new_document(100, 100, "#00ff00")
    doc.set_active_locked(False) if doc.active_layer().locked else None
    # background layer from new_document is locked; paint on a new layer
    doc.add_layer("Paint")
    doc.begin_stroke("brush", 50, 50, size=40, hardness=1.0,
                     color="#ff0000")
    doc.end_stroke()
    assert hex_at(doc, 50, 50) == "#ff0000"
    doc.begin_stroke("eraser", 50, 50, size=40, hardness=1.0)
    doc.end_stroke()
    assert hex_at(doc, 50, 50) == "#00ff00"  # backdrop shows through


def test_stroke_opacity_ceiling():
    # opacity acts as a stroke ceiling: overlapping dabs at 50% never
    # exceed 50% (web commitLive semantics).
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    doc.begin_stroke("brush", 50, 50, size=30, hardness=1.0, opacity=0.5,
                     flow=1.0, color="#000000")
    for i in range(10):
        doc.stroke_to(50 + i, 50)
    doc.end_stroke()
    # composite of 50% black over white ≈ #808080 (allow rounding)
    color = hex_at(doc, 50, 50)
    value = int(color[1:3], 16)
    assert 120 <= value <= 136, color


def test_clone_requires_source():
    doc = make_doc()
    assert not doc.begin_stroke("clone", 10, 10)
    doc.set_clone_source(20, 20)
    assert doc.begin_stroke("clone", 40, 40)
    doc.end_stroke()


def test_dodge_lightens_burn_darkens():
    doc = make_doc()
    doc.new_document(100, 100, "")
    doc.add_layer("Gray")
    doc.fill_layer("#808080", 1.0)
    doc.begin_stroke("dodge", 30, 50, size=20, hardness=1.0, opacity=0.4,
                     flow=1.0)
    doc.end_stroke()
    doc.begin_stroke("burn", 70, 50, size=20, hardness=1.0, opacity=0.4,
                     flow=1.0)
    doc.end_stroke()
    dodge = int(hex_at(doc, 30, 50)[1:3], 16)
    burn = int(hex_at(doc, 70, 50)[1:3], 16)
    assert dodge > 0x80 > burn


# ── selection + click tools ──────────────────────────────────────────────


def test_selection_clips_painting():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    doc.set_selection(0, 0, 50, 100)
    doc.begin_stroke("brush", 50, 50, size=60, hardness=1.0,
                     color="#ff0000")
    doc.end_stroke()
    assert hex_at(doc, 40, 50) == "#ff0000"   # inside selection
    assert hex_at(doc, 60, 50) == "#ffffff"   # clipped outside
    doc.clear_selection()
    assert not doc.has_selection()


def test_select_all_and_delete():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#123456", 1.0)
    assert hex_at(doc, 10, 10) == "#123456"
    doc.set_selection(0, 0, 320, 100)
    assert doc.delete_selection_pixels()
    assert hex_at(doc, 10, 10) != "#123456"
    assert hex_at(doc, 10, 150) == "#123456"


def test_fill_at_flood_fills_similar_region():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    # paint a hard red square, then bucket the outside blue
    doc.draw_shape(20, 20, 60, 60, "#ff0000", 0)
    assert doc.fill_at(80, 80, "#0000ff", tolerance=32, contiguous=True)
    assert hex_at(doc, 80, 80) == "#0000ff"
    assert hex_at(doc, 40, 40) == "#ff0000"  # inside square untouched


def test_wand_select_returns_bbox():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    doc.draw_shape(30, 30, 70, 70, "#ff0000", 0)
    rect = doc.wand_select(50, 50, tolerance=32, contiguous=True)
    assert rect is not None
    x, y, w, h = rect
    assert abs(x - 30) <= 1 and abs(y - 30) <= 1
    assert abs(w - 40) <= 2 and abs(h - 40) <= 2
    assert doc.selection() == rect


def test_eyedropper_picks_composite():
    doc = make_doc()
    doc.new_document(100, 100, "#336699")
    assert doc.pick_color(50, 50) == "#336699"


def test_gradient_fades_foreground():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    assert doc.apply_gradient(0, 0, 0, 100, "#000000", 1.0)
    top = int(hex_at(doc, 50, 2)[1:3], 16)
    bottom = int(hex_at(doc, 50, 98)[1:3], 16)
    assert top < 30 and bottom > 220


def test_type_tool_places_text_layer():
    doc = make_doc()
    lid = doc.place_type(10, 10, "Hi", size=48, color="#000000")
    assert lid > 0
    layer = doc.active_layer()
    assert layer.kind == "text"
    assert layer.name == "T Hi"
    assert [h.name for h in doc.history_entries()][-1] == "Type: Hi"


def test_shape_tool_draws_rect():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    assert doc.draw_shape(10, 10, 90, 90, "#00ff00", 8)
    assert hex_at(doc, 50, 50) == "#00ff00"
    assert hex_at(doc, 11, 11) == "#ffffff"  # rounded corner clipped


# ── adjustments & filters ────────────────────────────────────────────────


def test_invert():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#102030", 1.0)
    assert doc.apply_adjust("invert", 0, "Invert", "mirror")
    assert hex_at(doc, 10, 10) == "#efdfcf"


def test_desaturate():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#ff0000", 1.0)
    doc.apply_adjust("desat")
    color = hex_at(doc, 10, 10)
    assert color[1:3] == color[3:5] == color[5:7]


def test_threshold():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#c0c0c0", 1.0)
    doc.apply_adjust("threshold")
    assert hex_at(doc, 10, 10) == "#ffffff"


def test_preview_commit_and_cancel():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#808080", 1.0)
    before = hex_at(doc, 10, 10)
    assert doc.begin_preview()
    doc.preview_adjust("bc", 60, 0, 0)  # brightness +60
    lightened = hex_at(doc, 10, 10)
    assert int(lightened[1:3], 16) > int(before[1:3], 16)
    doc.cancel_preview()
    assert hex_at(doc, 10, 10) == before
    # now commit one
    doc.begin_preview()
    doc.preview_adjust("bc", 60, 0, 0)
    doc.commit_preview("Brightness/Contrast", "light")
    assert hex_at(doc, 10, 10) == lightened
    assert [h.name for h in doc.history_entries()][-1] == \
        "Brightness/Contrast"


def test_blur_softens_edge():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    doc.draw_shape(0, 0, 50, 100, "#000000", 0)
    doc.begin_preview()
    doc.preview_adjust("blur", 8, 0, 0)
    doc.commit_preview("Gaussian Blur", "blur")
    edge = int(hex_at(doc, 50, 50)[1:3], 16)
    assert 40 < edge < 215  # neither black nor white at the edge


def test_pixelate_quantizes():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    doc.apply_gradient(0, 0, 100, 0, "#000000", 1.0)
    doc.apply_adjust("pixelate", 10)
    # inside one 10px block every pixel matches
    assert hex_at(doc, 41, 41) == hex_at(doc, 48, 48)


# ── history ──────────────────────────────────────────────────────────────


def test_undo_redo_restore_pixels():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#ff0000", 1.0)
    doc.fill_layer("#00ff00", 1.0)
    assert hex_at(doc, 10, 10) == "#00ff00"
    doc.undo()
    assert hex_at(doc, 10, 10) == "#ff0000"
    doc.redo()
    assert hex_at(doc, 10, 10) == "#00ff00"


def test_history_jump_and_truncate():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.fill_layer("#111111", 1.0, "source-over", "Fill A")
    doc.fill_layer("#222222", 1.0, "source-over", "Fill B")
    names = [h.name for h in doc.history_entries()]
    assert names[-2:] == ["Fill A", "Fill B"]
    assert doc.jump_to(len(names) - 2)
    assert hex_at(doc, 10, 10) == "#111111"
    # a new edit truncates the redo tail (web parity)
    doc.fill_layer("#333333", 1.0, "source-over", "Fill C")
    names = [h.name for h in doc.history_entries()]
    assert names[-2:] == ["Fill A", "Fill C"]


def test_history_brush_restores_source():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Paint")
    doc.fill_layer("#ff0000", 1.0)         # state used as history source
    doc.set_history_source(doc.history_index())
    doc.fill_layer("#0000ff", 1.0)          # newer state
    assert hex_at(doc, 50, 50) == "#0000ff"
    assert doc.begin_stroke("history", 50, 50, size=30, hardness=1.0)
    doc.end_stroke()
    assert hex_at(doc, 50, 50) == "#ff0000"  # restored from source


def test_layer_ops_snapshot_history():
    doc = make_doc()
    doc.add_layer("A")
    doc.toggle_visible(doc.active_id())
    names = [h.name for h in doc.history_entries()]
    assert names[-1] == "Hide Layer"
    assert names[-2] == "New Layer"


# ── blend modes ──────────────────────────────────────────────────────────


def test_multiply_blend():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Gray")
    doc.fill_layer("#808080", 1.0)
    doc.add_layer("Mult")
    doc.fill_layer("#808080", 1.0)
    doc.set_active_blend("multiply")
    # 0x80 * 0x80 / 255 ≈ 0x40
    value = int(hex_at(doc, 50, 50)[1:3], 16)
    assert abs(value - 0x40) <= 2


def test_screen_blend():
    doc = make_doc()
    doc.new_document(100, 100, "")
    doc.add_layer("Base")
    doc.fill_layer("#808080", 1.0)
    doc.add_layer("Scr")
    doc.fill_layer("#808080", 1.0)
    doc.set_active_blend("screen")
    # 255 - (255-128)² / 255 ≈ 0xc0
    value = int(hex_at(doc, 50, 50)[1:3], 16)
    assert abs(value - 0xC0) <= 2


def test_layer_opacity_scales():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Half")
    doc.fill_layer("#000000", 1.0)
    doc.set_active_opacity(0.5)
    value = int(hex_at(doc, 50, 50)[1:3], 16)
    assert 120 <= value <= 136


def test_merge_down_and_flatten():
    doc = make_doc()
    doc.new_document(100, 100, "#ffffff")
    doc.add_layer("Red")
    doc.fill_layer("#ff0000", 1.0)
    doc.add_layer("HalfBlack")
    doc.fill_layer("#000000", 0.5)
    assert doc.merge_down()
    assert len(doc.layers()) == 2
    dark_red = hex_at(doc, 50, 50)
    assert int(dark_red[1:3], 16) < 0xFF  # black merged into red
    assert doc.flatten()
    assert len(doc.layers()) == 1
    assert hex_at(doc, 50, 50) == dark_red  # visual result preserved


# ── geometry ─────────────────────────────────────────────────────────────


def test_crop_to_selection():
    doc = make_doc()
    doc.add_layer("Paint")
    doc.draw_shape(100, 50, 200, 150, "#ff0000", 0)
    doc.set_selection(100, 50, 100, 100)
    assert doc.apply_crop()
    assert doc.width() == 100 and doc.height() == 100
    assert hex_at(doc, 50, 50) == "#ff0000"
    assert not doc.has_selection()


def test_resize_image_scales_content():
    doc = make_doc()
    doc.new_document(100, 100, "#ff0000")
    assert doc.resize_image(50, 50)
    assert doc.width() == 50
    assert hex_at(doc, 25, 25) == "#ff0000"


def test_resize_canvas_anchors():
    doc = make_doc()
    doc.new_document(100, 100, "#ff0000")
    assert doc.resize_canvas(200, 100, "tl")  # doc content stays top-left
    assert doc.width() == 200
    assert hex_at(doc, 50, 50) == "#ff0000"
    picked = doc.pick_color(150, 50)  # extended area is transparent
    assert picked != "#ff0000"


# ── assets / export ──────────────────────────────────────────────────────


def test_place_assets():
    doc = make_doc()
    for asset, blend in (("Sun flare", "screen"), ("Vignette", "multiply"),
                         ("Noise texture", "overlay"),
                         ("Gradient map", "soft-light")):
        doc.place_asset(asset)
        layer = doc.active_layer()
        assert layer.name == asset
        assert layer.blend == blend


def test_export_png(tmp_path):
    doc = make_doc(64, 48)
    doc.add_layer("Paint")
    doc.fill_layer("#ff8800", 1.0)
    path = tmp_path / "out.png"
    assert doc.export_png(str(path), 1.0, True)
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    # verify decodability if PIL is around (optional)
    try:
        from PIL import Image
    except ImportError:
        return
    img = Image.open(path)
    assert img.size == (64, 48)
    assert img.convert("RGB").getpixel((32, 24)) == (0xFF, 0x88, 0x00)


# ── view transform ───────────────────────────────────────────────────────


def test_view_math_roundtrip():
    doc = make_doc()
    doc.set_zoom(2.0)
    doc.set_pan(15, -22)
    # without a stage rect, origin math still holds relative to (0,0)
    ox, oy = doc.doc_origin()
    dx, dy = doc.screen_to_doc(ox + 40, oy + 60)
    assert math.isclose(dx, 20.0, abs_tol=1e-6)
    assert math.isclose(dy, 30.0, abs_tol=1e-6)
