"""Rebuild authoring-only compound looks from the versioned canonical fixture.

No engine state or resource references are created. The production decoder and
native demo remain the consumers of these ordinary complete RSL snapshots.
"""
from pathlib import Path
import copy
import json

root = Path(__file__).resolve().parent / 'looks'
base = json.loads((root / 'cel-paint.rsl').read_bytes())

paper = copy.deepcopy(base)
paper.update(colorMapping=1, posterizationEnabled=False, colorLevels=6,
             ditherStrength=0.12, shadowColor=[0.96, 0.91, 0.80],
             highlightColor=[0.12, 0.20, 0.35], printPattern=1,
             printSpacing=6.0, printAngle=28.0, printGamma=0.65,
             printStrength=0.16, inkColor=[0.12, 0.18, 0.34],
             paperColor=[0.97, 0.89, 0.75], outlineMode=2,
             outlineStrength=0.65, outlineColor=[0.12, 0.16, 0.29])
paper['temporal'].update(enabled=True, mode=0, historyWeight=0.60,
                         historyOffsetX=0.35, historyOffsetY=0.12)
paper['display'].update(enabled=True, scanlineStrength=0.0, maskStrength=0.0,
                        chromaticOffset=0.0, curvature=0.0, vignetteStrength=0.12,
                        grainStrength=0.008, noiseSeed=73)

phosphor = copy.deepcopy(base)
colors = [[0.008, 0.024, 0.028], [0.015, 0.10, 0.11],
          [0.025, 0.28, 0.24], [0.08, 0.48, 0.34],
          [0.28, 0.72, 0.46], [0.66, 0.91, 0.65], [0.91, 1.0, 0.84]]
phosphor.update(colorMapping=2, pixelationEnabled=True, pixelSize=3,
                posterizationEnabled=False, ditherStrength=0.16,
                paletteSize=len(colors), palette=colors+[colors[-1]]*(16-len(colors)),
                printPattern=0, outlineMode=0)
phosphor['temporal'].update(enabled=True, mode=1, holdFrames=4,
                            historyOffsetX=0.0, historyOffsetY=0.0)
phosphor['display'].update(enabled=True, scanlineStrength=0.18, maskStrength=0.08,
                           chromaticOffset=0.2, curvature=0.025,
                           vignetteStrength=0.18, grainStrength=0.012, noiseSeed=151)

for name, look in [('paper-echo', paper), ('phosphor-stop', phosphor)]:
    (root / (name + '.rsl')).write_text(json.dumps(look, indent=2)+'\n', encoding='utf-8')
    print(name)
