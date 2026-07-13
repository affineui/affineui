# Drag the DENDER floating tool rail by its grip. The grip (drag handle)
# sits at the top of the rail; exact y varies with theme metrics, so try a
# few candidate press points — a miss just clicks and the trace shows no
# arm, a hit arms float-drag and logs everything.
foreach ($gy in 100, 106, 112, 96) {
    Write-Host "== try grip at (34,$gy) =="
    Drag 34 $gy 134 ($gy + 60) 10
    Start-Sleep -Milliseconds 400
}
