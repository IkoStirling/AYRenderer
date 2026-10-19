"""Generate the editable 32-swatch reference and PNG using only Python's stdlib."""
from pathlib import Path
import struct,zlib

folder=Path(__file__).parent/'looks'
colors=[(28,38,61),(47,121,119),(131,169,112),(214,164,84),
        (206,100,77),(111,68,117),(63,111,157),(239,225,184)]
tile,gap=48,4
width,height=8*tile+9*gap,4*tile+5*gap
pixels=bytearray(width*height*4)
shapes=[]
for row in range(4):
    for col,base in enumerate(colors):
        color=tuple(round(v*(.6,.85,1,1)[row]+(255-v)*(.25 if row==3 else 0)) for v in base)
        x0,y0=gap+col*(tile+gap),gap+row*(tile+gap)
        for y in range(y0,y0+tile):
            for x in range(x0,x0+tile):pixels[(y*width+x)*4:(y*width+x)*4+4]=bytes((*color,255))
        shapes.append(f'<rect x="{x0}" y="{y0}" width="{tile}" height="{tile}" fill="#{color[0]:02x}{color[1]:02x}{color[2]:02x}"/>')
def chunk(kind,data):
    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
rows=b''.join(b'\0'+pixels[y*width*4:(y+1)*width*4] for y in range(height))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b'')
(folder/'palette-reference.png').write_bytes(png)
(folder/'palette-reference.svg').write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">\n'+ '\n'.join(shapes)+'\n</svg>\n',encoding='utf-8')
print(f'Generated {width}x{height} reference with 32 swatches and transparent gutters')
