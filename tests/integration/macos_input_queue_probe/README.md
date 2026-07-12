# macOS input queue probe

This opt-in integration probe posts 300 tagged native mouse-drag events at 1 kHz
while each UI frame deliberately occupies the main thread for 20 ms. It runs
once with normal frame scheduling and once while a frame pumps a nested AppKit
run loop.

Acceptance requires every input callback in order, bounded event and mouse-up
age, one frame transaction at a time, no input callback during an active frame,
and far fewer frames than input events.

```sh
cmake -S tests/integration/macos_input_queue_probe \
  -B build-input-probe -G Ninja \
  -DAFFINEUI_CHECKOUT="$PWD" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-input-probe --target macos_input_queue_probe
python3 tests/integration/macos_input_queue_probe/run_probe.py \
  build-input-probe/macos_input_queue_probe
```
