# DENDER workload for drive-input.ps1 — exercises the gestures that have
# been perf-sensitive: hover sweep, viewport orbit drag, picks, panel
# clicks, and the vertical/horizontal splitter drags (the splitter drag is
# the one hand-driving can't reproduce consistently). Coordinates assume the
# default 1440x900 DENDER layout. Dot-sourced by drive-input.ps1, so the
# MoveTo/Down/Up/Click/Drag helpers are in scope.

Write-Host "== hover sweep =="
for ($i = 0; $i -lt 30; $i++) { MoveTo (100 + $i * 40) (80 + $i * 20); Start-Sleep -Milliseconds 30 }
Start-Sleep -Milliseconds 500

Write-Host "== viewport orbit drag =="
Drag 600 400 720 440 20
Start-Sleep -Milliseconds 500

Write-Host "== picks + panel clicks =="
Click 600 400           # viewport pick
Click 1240 140          # outliner row
Click 60 300            # tool rail

Write-Host "== vertical splitter drags (viewport | right panels) =="
foreach ($sx in 1070, 1078, 1086) {
    Drag $sx 400 ($sx - 60) 400 12
    Drag ($sx - 60) 400 $sx 400 12
}

Write-Host "== horizontal splitter drags (workarea | timeline) =="
foreach ($sy in 640, 660, 680) {
    Drag 600 $sy 600 ($sy - 40) 10
}
