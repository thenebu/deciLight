# deciLight – 3D-Druckdateien

Dieser Ordner enthält die 3D-Modelle für das deciLight-Gehäuse.

- `deciLight.f3d` – Fusion 360 Quelldatei (alle Teile)
- `deciLight-main.3mf` – Gehäuse: Base, Cover, Visor, LED-Mount, ESP-Mount
- `deciLight-feet.3mf` – 4x Standfüße
- `deciLight-lens.3mf` – Diffusor-Linse
- `deciLight-reflector.3mf` – Reflektor

Die `.3mf`-Dateien enthalten bereits fertige Bambu-Studio-Projekte inkl.
Druckeinstellungen (Slicer: Bambu Studio, Drucker: Bambu Lab X1 Carbon,
0.4 mm Düse).

## Druckeinstellungen

Basisprofil: **0.20mm Standard @BBL X1C**

| Teil | Material | Layer Height | Wandlinien | Infill | Top/Bottom Layers | Düsentemp. | Bett-temp. | Brim |
|---|---|---|---|---|---|---|---|---|
| main (Base/Cover/Visor/LED-Mount/ESP-Mount) | PETG (Bambu PETG Basic), Farbe `#363636` | 0.20 mm | 2 | 15 % (Gyroid) | 5 / 3 | 255 °C | 70 °C | Auto, 5 mm |
| feet | TPU (Bambu TPU 95A) | 0.20 mm | 2 | 15 % (Grid) | Standard | 230 °C | 35 °C | Auto, 5 mm |
| lens | PETG (Bambu PETG Basic), Farbe `#FFFFFF`, für den Diffusor-Effekt ggf. transparentes/natur PETG verwenden | 0.20 mm | 2 | 15 % (Grid) | 5 / 3 | 255 °C | 70 °C | Auto, 5 mm |
| reflector | PETG (Bambu PETG Basic), Farbe `#363636` | 0.20 mm | 1 | 0 % (kein Infill) | 0 / 3 | 255 °C | 70 °C | Auto, 5 mm |

Weitere Einstellungen (für alle Teile gleich):
- Düsendurchmesser: 0.4 mm
- Supports: aus (`enable_support = 0`), Support-Winkel-Schwelle 30° falls doch benötigt
- Bett-Typ: Hot Plate (Textured/Engineering Plate empfohlen für PETG/TPU)

## Hinweise

- **feet** aus TPU drucken für rutschfeste, leicht flexible Standfüße.
- **reflector** hat bewusst keinen Top-Layer und keinen Infill (dünne
  Schale) – bei Bedarf innen mit reflektierender Folie/Farbe nachbehandeln.
- **lens** wurde als weißes PETG geslict; für einen echten Diffusor-Effekt
  transparentes oder natur-farbenes PETG (oder PETG-Diffusor-Filament)
  verwenden.
- Alle `.3mf`-Dateien lassen sich direkt in Bambu Studio (oder kompatiblen
  Slicern wie OrcaSlicer) öffnen – die Einstellungen sind bereits enthalten.
