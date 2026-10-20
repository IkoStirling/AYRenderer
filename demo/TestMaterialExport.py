"""Opt-in process test of showcase preservation and changing-input rejection.

Run: python TestMaterialExport.py VIEWER.exe FRESH_OUTPUT
Uses PowerShell 7 and the real device viewer. All mutation stays in a copied
library; completed/failing captures and logs remain available for inspection.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import time


def accept(viewer, output):
    output = output.resolve()
    if output.exists():
        raise ValueError('Choose a fresh output directory')
    output.mkdir(parents=True)
    demo = Path(__file__).resolve().parent
    library = output / 'library'
    shutil.copytree(demo / 'MaterialWorkshop/Assets/materials', library)
    environment = os.environ.copy()
    for key in ['AY_SHADOW_USE_SC', 'AY_SHADOW_USE_PHOSKIA']:
        environment.pop(key, None)

    def command(destination):
        return ['pwsh', '-NoProfile', '-File', str(demo / 'ExportMaterialShowcase.ps1'),
                '-Executable', str(viewer.resolve()), '-MaterialDirectory', str(library),
                '-OutputDirectory', str(destination)]

    existing = output / 'existing'
    existing.mkdir()
    sentinel = existing / 'showcase.json'
    sentinel.write_bytes(b'preserve this previous delivery\n')
    with (output / 'existing-output.log').open('wb') as log:
        result = subprocess.run(command(existing), env=environment, stdout=log,
                                stderr=subprocess.STDOUT, timeout=30)
    if result.returncode == 0 or sentinel.read_bytes() != b'preserve this previous delivery\n':
        raise AssertionError('Existing output must be refused without modifying its manifest')

    changing = output / 'changing-source'
    recipe = library / 'library.json'
    original = recipe.read_bytes()
    mutated = False
    with (output / 'changing-source.log').open('wb') as log:
        process = subprocess.Popen(command(changing), env=environment, stdout=log,
                                   stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 120
        try:
            while process.poll() is None:
                # The first path has completed GPU capture. Change an input
                # without breaking any engine file so rejection must come from
                # the chapter's identity check, rather than a loader failure.
                if not mutated and list((changing / '01-gallery').glob('forward-*.png')):
                    recipe.write_bytes(original + b'\n')
                    mutated = True
                if time.monotonic() >= deadline:
                    raise TimeoutError('Exporter did not finish the bounded acceptance case')
                time.sleep(.02)
            code = process.wait()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            recipe.write_bytes(original)
    diagnostic = (output / 'changing-source.log').read_text(encoding='utf-8-sig', errors='replace')
    captures = sorted((changing / '01-gallery').glob('*.png'))
    if not mutated or code == 0 or 'Source materials changed during 01-gallery' not in diagnostic:
        raise AssertionError('Changing inputs must be explicitly refused after the real chapter')
    if len(captures) != 2 or (changing / 'showcase.json').exists():
        raise AssertionError('Unaccepted captures must remain, without publishing a completed chapter')
    summary = dict(existingOutputPreserved=True, changedInputRejected=True,
                   refusedChapterCapturesPreserved=len(captures))
    (output / 'acceptance.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('viewer', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    accept(args.viewer, args.output)
