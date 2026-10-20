"""Opt-in real-device content acceptance; preserves every case and its log.

Run: python TestMaterialDemo.py TOOL.exe VIEWER.exe FRESH_OUTPUT [--d3d12]
Requires Pillow for independent screenshot decoding/comparison. All mutations
are confined to a copied library. No source asset or binary format is rewritten
by Python: the alpha reference uses the real MaterialTool encoder.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
from PIL import Image, ImageChops


def accept(tool, viewer, output, d3d12=False):
    output = output.resolve()
    if output.exists():
        raise ValueError('Choose a fresh output directory')
    output.mkdir(parents=True)
    source = Path(__file__).resolve().parent / 'MaterialWorkshop/Assets/materials'
    library = output / 'library'
    shutil.copytree(source, library)
    environment = os.environ.copy()
    for key in ['AY_SHADOW_USE_SC', 'AY_SHADOW_USE_PHOSKIA']:
        environment.pop(key, None)
    receipts = []

    def run(name, scene='transparency', refused=False):
        destination = output / name
        arguments = [str(viewer.resolve()), '--gallery', '--hidden', '--materials', str(library),
                     '--scene', scene, '--output', str(destination)]
        if d3d12:
            arguments.append('--d3d12')
        with (output / (name + '.log')).open('wb') as log:
            result = subprocess.run(arguments, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=90)
        images = sorted(destination.glob('*.png'))
        row = dict(name=name, returnCode=result.returncode, images=[p.name for p in images])
        receipts.append(row)
        (output / 'acceptance.json').write_text(json.dumps(receipts, indent=2)+'\n', encoding='utf-8')
        if refused:
            if result.returncode == 0 or images:
                raise AssertionError(f'{name} must refuse corrupt input without delivering captures: {row}')
        elif result.returncode or len(images) != 2:
            raise AssertionError(f'{name} did not complete both real render paths: {row}')
        return destination

    # A valid fallback is deliberately present. The viewer must not turn a
    # broken authored shader into a successful showcase through bridge fallback.
    shader = library / 'pbr.phoskia'
    original_shader = shader.read_bytes()
    (library / 'simple_lit_shadow.phoskia').write_bytes(original_shader)
    shader.write_text('material Broken { fragment { return ??? } }', encoding='utf-8')
    try:
        run('01-invalid-shader-with-valid-fallback', refused=True)
    finally:
        shader.write_bytes(original_shader)

    for filename, name, replacement in [
        ('metal.mat', '02-invalid-material', b'not a material'),
        ('textures/metal_n.png', '03-invalid-texture', b'not an image'),
        ('textures/metal_n.png', '04-missing-texture', None),
    ]:
        path = library / filename
        original = path.read_bytes()
        if replacement is None:
            path.unlink()
        else:
            path.write_bytes(replacement)
        try:
            run(name, refused=True)
        finally:
            path.write_bytes(original)

    baseline = run('05-transparent-acrylic')
    recipe_path = library / 'library.json'
    recipe = json.loads(recipe_path.read_text(encoding='utf-8'))
    for material in recipe['materials']:
        if material['id'].startswith('acrylic'):
            material['baseColor'][3] = 0
    recipe_path.write_text(json.dumps(recipe, indent=2)+'\n', encoding='utf-8')
    with (output / 'alpha-reference-build.log').open('wb') as log:
        result = subprocess.run([str(tool.resolve()), 'build', str(library)], stdout=log,
                                stderr=subprocess.STDOUT, timeout=30)
    if result.returncode:
        raise AssertionError('The real encoder refused the alpha-zero reference')
    reference = run('06-alpha-zero-reference')
    for path in ['forward-transparency.png', 'deferred-transparency.png']:
        with Image.open(baseline/path) as a, Image.open(reference/path) as b:
            if a.size != (1280, 800) or b.size != a.size:
                raise AssertionError('Unexpected real capture dimensions')
            difference = ImageChops.difference(a.convert('RGB'), b.convert('RGB')).crop((0, 110, 1280, 780))
            red, green, blue = difference.split()
            maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
            changed = maximum.point(lambda value: 255 if value > 3 else 0).histogram()[255]
            if changed < 1000:
                raise AssertionError(f'Transparent acrylic did not contribute real blended pixels: {path}, {changed}')
            receipts.append(dict(path=path, acrylicChangedPixels=changed))
    (output / 'acceptance.json').write_text(json.dumps(receipts, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(receipts, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', type=Path)
    parser.add_argument('viewer', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--d3d12', action='store_true')
    arguments = parser.parse_args()
    accept(arguments.tool, arguments.viewer, arguments.output, arguments.d3d12)
