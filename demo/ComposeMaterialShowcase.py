"""Compose contact sheets, paired GIFs and an offline browser from real captures.

Requires Pillow. Input is ExportMaterialShowcase.ps1's completed manifest.
Original captures and source assets are never changed.
"""
from pathlib import Path
import argparse
import base64
import html
import io
import json
import os
import re
from urllib.parse import quote
from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat


def font(size):
    for path in [Path('C:/Windows/Fonts/segoeui.ttf'),
                 Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')]:
        if path.is_file():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def read_image(root, chapter, filename):
    path = (root / chapter / filename).resolve()
    if not path.is_relative_to(root):
        raise ValueError('Capture path escapes the showcase directory')
    with Image.open(path) as image:
        return image.convert('RGB')


def sheet(root, chapters, output, title, columns):
    tile_width, tile_height, caption = 640, 400, 32
    rows = (len(chapters) * 2 + columns - 1) // columns
    canvas = Image.new('RGB', (tile_width * columns, 76 + rows * (tile_height + caption)), '#0b111b')
    draw = ImageDraw.Draw(canvas)
    draw.text((20, 15), title, fill='#e4efff', font=font(28))
    draw.text((20, 49), 'Real native .mat content  /  identical captured time  /  Forward + Deferred',
              fill='#8da9c7', font=font(16))
    index = 0
    for chapter in chapters:
        for prefix in ['forward-', 'deferred-']:
            files = [name for name in chapter['images'] if name.startswith(prefix)]
            if len(files) != 1:
                raise ValueError('Contact sheets require one static capture per path')
            image = read_image(root, chapter['name'], files[0])
            image.thumbnail((tile_width, tile_height), Image.Resampling.LANCZOS)
            x, y = (index % columns) * tile_width, 76 + (index // columns) * (tile_height + caption)
            canvas.paste(image, (x, y))
            draw.text((x + 12, y + tile_height + 4),
                      f"{chapter.get('material') or chapter.get('scene')} / {chapter['variant']} / {prefix[:-1]}",
                      fill='#bcd2e9', font=font(18))
            index += 1
    canvas.save(output)


def motion(root, chapter, output):
    ordered = {}
    for prefix in ['forward-', 'deferred-']:
        files = [name for name in chapter['images'] if name.startswith(prefix)]
        ordered[prefix] = sorted(files, key=lambda name: int(re.search(r'-(\d+)\.png$', name).group(1)))
        if len(files) != 32:
            raise ValueError('Expected the complete 32-frame motion capture')
    frames, changes, previous = [], {'forward': [], 'deferred': []}, {}
    for index in range(32):
        canvas = Image.new('RGB', (960, 376), '#0b111b')
        draw = ImageDraw.Draw(canvas)
        draw.text((14, 8), chapter['name'].replace('05-', '').replace('-', ' ').title(),
                  font=font(22), fill='#e4efff')
        draw.text((14, 338), 'Forward', font=font(17), fill='#bcd2e9')
        draw.text((494, 338), 'Deferred', font=font(17), fill='#bcd2e9')
        for side, prefix in enumerate(['forward-', 'deferred-']):
            image = read_image(root, chapter['name'], ordered[prefix][index])
            # Exclude the static header. Record measured motion, not just file count.
            surface = image.crop((0, 100, image.width, image.height))
            key = prefix[:-1]
            if key in previous:
                delta = ImageChops.difference(surface, previous[key])
                changes[key].append(sum(ImageStat.Stat(delta).mean) / 3)
            previous[key] = surface
            image.thumbnail((480, 300), Image.Resampling.LANCZOS)
            canvas.paste(image, (side * 480, 36))
        frames.append(canvas.quantize(colors=128, method=Image.Quantize.MEDIANCUT))
    frames[0].save(output, save_all=True, append_images=frames[1:], duration=100,
                   loop=0, disposal=2, optimize=False)
    return dict(chapter=chapter['name'], frames=32, durationMs=3200,
                adjacentMeanByteChange=changes,
                changedPairs={key: sum(value > 0 for value in values) for key, values in changes.items()})


def gallery(root, chapters, destination, backend):
    """Offline browsing of actual captures; originals remain separate PNG files."""
    rows = []
    for chapter in chapters:
        row = {key: chapter.get(key) for key in ['name', 'scene', 'material', 'variant', 'lighting', 'motion']}
        row['title'] = (chapter.get('material') or chapter.get('scene') or chapter['name']).replace('-', ' ').title()
        row['images'] = []
        for prefix in ['forward-', 'deferred-']:
            files = sorted(name for name in chapter['images'] if name.startswith(prefix))
            if not files:
                raise ValueError('Each chapter must contain both render paths')
            path = (root / chapter['name'] / files[0]).resolve()
            image = read_image(root, chapter['name'], files[0])
            image.thumbnail((960, 600), Image.Resampling.LANCZOS)
            buffer = io.BytesIO()
            image.save(buffer, format='JPEG', quality=87)
            row['images'].append(dict(
                preview='data:image/jpeg;base64,' + base64.b64encode(buffer.getvalue()).decode('ascii'),
                original=quote(os.path.relpath(path, destination).replace('\\', '/'), safe='/.'),
                label=prefix[:-1].title()))
        if chapter['motion']:
            gif = destination / (chapter['name'] + '.gif')
            row['animation'] = 'data:image/gif;base64,' + base64.b64encode(gif.read_bytes()).decode('ascii')
        rows.append(row)
    # Data stays inside its JSON block, including names from an edited manifest.
    data = json.dumps(rows, ensure_ascii=True).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')
    document = r'''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Material Workshop</title>
<style>
:root{color-scheme:dark;font:16px/1.5 system-ui,sans-serif;background:#0b111b;color:#e4efff}
*{box-sizing:border-box}body{margin:0}header,main{max-width:1600px;margin:auto;padding:24px}
header{padding-bottom:12px}h1{margin:0;font-size:30px}p{color:#a9bed7;margin:6px 0}
nav{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin:16px 0}
label{display:grid;gap:4px;font-size:13px;color:#a9bed7}select,button{font:inherit;color:#e4efff;background:#152239;border:1px solid #365171;border-radius:6px;padding:9px 12px}
button{cursor:pointer}button:disabled{opacity:.4;cursor:default}select:focus-visible,button:focus-visible,a:focus-visible{outline:3px solid #7ed6f5;outline-offset:3px}
#chapter{min-width:260px;max-width:100%}.pair{display:grid;grid-template-columns:1fr 1fr;gap:12px}
figure{margin:0;background:#111d2e;border:1px solid #2a3c55;border-radius:8px;overflow:hidden}
figure img{width:100%;height:auto;display:block}figcaption{display:flex;justify-content:space-between;padding:8px 12px}a{color:#7ed6f5}
h2{font-size:21px;margin:0}#details{margin-bottom:16px}#animation{max-width:960px;width:100%;display:block;margin:auto}
[hidden]{display:none!important}footer{padding:20px 0;font-size:13px;color:#a9bed7}
@media(max-width:850px){.pair{grid-template-columns:1fr}header,main{padding:16px}h1{font-size:25px}}
</style>
<header><h1>Material Workshop</h1><p>@@BACKEND@@ · Real rendered surfaces, conditions and light studies</p></header>
<main>
<nav aria-label="Browse studies">
<label>Collection<select id="group"><option value="all">All studies</option><option value="exhibits">Exhibits</option><option value="heroes">Surface closeups</option><option value="conditions">Surface conditions</option><option value="lighting">Lighting</option><option value="motion">Motion</option></select></label>
<label>Study<select id="chapter"></select></label>
<button id="previous" aria-label="Previous study">← Previous</button><button id="next" aria-label="Next study">Next →</button>
<button id="play" hidden>Play motion</button><span id="position" aria-live="polite"></span>
</nav>
<h2 id="title"></h2><p id="details"></p>
<section class="pair" id="pair" aria-label="Forward and Deferred comparison">
<figure><img id="forward" alt="Forward render"><figcaption>Forward <a id="forward-original">Original PNG</a></figcaption></figure>
<figure><img id="deferred" alt="Deferred render"><figcaption>Deferred <a id="deferred-original">Original PNG</a></figcaption></figure>
</section>
<img id="animation" hidden alt="Animated Forward and Deferred comparison">
<footer>Previews are embedded for offline browsing. Original PNG links use the adjacent showcase directory.
Lighting studies use the existing StandardLit material model. Acrylic uses straight-alpha blending.</footer>
</main><script type="application/json" id="data">@@DATA@@</script>
<script>
'use strict';
const rows=JSON.parse(document.getElementById('data').textContent);
const el=id=>document.getElementById(id); let filtered=rows,playing=false;
function show(){
  const row=filtered[Number(el('chapter').value)];if(!row)return;
  playing=false;el('animation').hidden=true;el('animation').removeAttribute('src');el('pair').hidden=false;
  el('title').textContent=row.title;
  el('details').textContent=[row.variant,row.lighting,row.motion?'32 captured frames per path':'Same captured time'].join(' · ');
  row.images.forEach((image,index)=>{const id=index?'deferred':'forward';el(id).src=image.preview;el(id+'-original').href=image.original;});
  el('play').hidden=!row.motion;el('play').textContent='Play motion';
  el('position').textContent=(Number(el('chapter').value)+1)+' / '+filtered.length;
  el('previous').disabled=el('chapter').selectedIndex===0;
  el('next').disabled=el('chapter').selectedIndex===filtered.length-1;
}
function filter(){
 const group=el('group').value;
 filtered=rows.filter(row=>group==='all'||(group==='exhibits'&&row.name.startsWith('01-'))||(group==='heroes'&&row.name.startsWith('02-'))||(group==='conditions'&&row.name.startsWith('03-'))||(group==='lighting'&&row.name.startsWith('04-'))||(group==='motion'&&row.motion));
 el('chapter').replaceChildren(...filtered.map((row,index)=>{const option=document.createElement('option');option.value=index;option.textContent=row.title+' / '+row.variant+' / '+row.lighting;return option;}));show();
}
el('group').addEventListener('change',filter);el('chapter').addEventListener('change',show);
function step(delta){const select=el('chapter');select.selectedIndex=Math.max(0,Math.min(filtered.length-1,select.selectedIndex+delta));show();}
el('previous').addEventListener('click',()=>step(-1));el('next').addEventListener('click',()=>step(1));
el('play').addEventListener('click',()=>{const row=filtered[Number(el('chapter').value)];playing=!playing;el('animation').hidden=!playing;el('pair').hidden=playing;el('play').textContent=playing?'Pause motion':'Play motion';if(playing)el('animation').src=row.animation;else el('animation').removeAttribute('src');});
filter();
</script></html>'''
    document = document.replace('@@BACKEND@@', html.escape(backend)).replace('@@DATA@@', data)
    (destination / 'material-workshop.html').write_text(document, encoding='utf-8')


def compose(source, destination):
    root = source.resolve()
    if destination.exists():
        raise ValueError('Choose a fresh output directory')
    manifest = json.loads((root / 'showcase.json').read_text(encoding='utf-8-sig'))
    chapters = manifest['completedChapters']
    if manifest.get('schemaVersion') != 1 or manifest.get('backend') not in ['D3D11', 'D3D12'] or not chapters:
        raise ValueError('Expected a nonempty native showcase manifest')
    names = set()
    for chapter in chapters:
        name = chapter['name']
        if not isinstance(name, str) or not re.fullmatch(r'[a-z0-9][a-z0-9-]*', name) or name in names:
            raise ValueError('Chapter names must be unique portable folder names')
        names.add(name)
        files = chapter['images']
        if not files or len(files) != len(set(files)) or any(
                not re.fullmatch(r'(forward|deferred)-[a-z0-9-]+\.png', name) for name in files):
            raise ValueError('Expected unique native PNG capture names')
        for prefix in ['forward-', 'deferred-']:
            expected = 32 if chapter['motion'] else 1
            if sum(name.startswith(prefix) for name in files) != expected:
                raise ValueError('Each chapter must contain both complete render paths')
    destination.mkdir(parents=True)
    base = [c for c in chapters if c['name'].startswith('01-')]
    heroes = [c for c in chapters if c['name'].startswith('02-')]
    conditions = [c for c in chapters if not c['motion'] and c.get('scene') == 'gallery']
    if base:
        sheet(root, base, destination / 'material-workshop-overview.png', f'MATERIAL WORKSHOP / {len(base)} EXHIBITS', 2)
    if heroes:
        sheet(root, heroes, destination / 'material-hero-surfaces.png', f'{len(heroes)} PROCEDURAL SURFACES / CLOSE STUDIES', 4)
    if conditions:
        sheet(root, conditions, destination / 'material-conditions.png', 'BASE / CLEAN / WET / WORN', 2)
    measured = []
    for chapter in chapters:
        if chapter['motion']:
            measured.append(motion(root, chapter, destination / (chapter['name'] + '.gif')))
    gallery(root, chapters, destination, manifest['backend'])
    summary = dict(backend=manifest['backend'], sourceImages=sum(len(c['images']) for c in chapters), motion=measured)
    (destination / 'visual-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('showcase', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    compose(args.showcase, args.output)
