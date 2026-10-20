"""Rebuild tileable StandardLit texture content with Python's standard library.

Writes PNGs and the demo-private build recipe consumed by MaterialTool. It does
not invent a material binary encoder; .mat files are written by AYResource.
"""
from pathlib import Path
import argparse,json,math,struct,zlib

SIZE=256
ROOT=Path(__file__).resolve().parent/'MaterialWorkshop'/'Assets'/'materials'
TAU=math.tau
MATERIALS = [
    ('metal', 'Satin Steel'), ('stone', 'Warm Stone Blocks'), ('wood', 'Walnut Planks'),
    ('ceramic', 'Glazed Aqua Tiles'), ('rubber', 'Diamond Rubber'), ('paint', 'Chipped Safety Paint'),
    ('fabric', 'Teal Woven Fabric'), ('panel', 'Luminous Circuit Panel'),
    ('copper', 'Patinated Copper'), ('concrete', 'Poured Concrete'), ('terrazzo', 'Cream Terrazzo'),
    ('leather', 'Brown Pebbled Leather'), ('asphalt', 'Fine Asphalt'), ('brick', 'Fired Clay Bond'),
    ('marble', 'Ivory Veined Marble'), ('plastic', 'Blue Molded Polymer'),
    ('acrylic', 'Tinted Transparent Acrylic'),
]
def clamp(x,low=0.,high=1.): return max(low,min(high,x))
def mix(a,b,t): return tuple(x+(y-x)*t for x,y in zip(a,b))
def noise(u,v):
    return (math.sin(TAU*(3*u+2*v))+.5*math.sin(TAU*(7*u-5*v))+.25*math.sin(TAU*(13*u+11*v)))/1.75
def cell(u,v,tiles):
    x=u*tiles%1;y=v*tiles%1
    return min(x,1-x,y,1-y)

def hash01(x, y, seed=0):
    value = ((x * 374761393) ^ (y * 668265263) ^ seed) & 0xffffffff
    value = ((value ^ (value >> 13)) * 1274126177) & 0xffffffff
    return (value ^ (value >> 16)) / 0xffffffff

def cellular(u, v, tiles):
    """Periodic jittered cells: nearest/second distance and a stable cell tone."""
    gx, gy = u * tiles, v * tiles
    ix, iy = math.floor(gx), math.floor(gy)
    first, second, tone = 100., 100., 0.
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cx, cy = ix + dx, iy + dy
            wx, wy = cx % tiles, cy % tiles
            px = cx + .2 + .6 * hash01(wx, wy, 17) - gx
            py = cy + .2 + .6 * hash01(wx, wy, 83) - gy
            distance = px * px + py * py
            if distance < first:
                second, first, tone = first, distance, hash01(wx, wy, 131)
            elif distance < second:
                second = distance
    return math.sqrt(first), math.sqrt(second), tone

def surface(kind,u,v,variant='base'):
    u %= 1.; v %= 1.
    n=noise(u,v);h=.5;metal=0;rough=.7;ao=1;emission=(0,0,0)
    if kind=='metal':
        groove=math.sin(TAU*64*v)*.5+.5
        c=tuple(clamp(x+.035*n) for x in (.48,.53,.59));h=.5+.025*groove
        metal=1;rough=.26+.10*groove+.035*n
    elif kind=='stone':
        border=cell(u,v,4);mortar=clamp((border-.018)/.035)
        shade=.5+.5*math.sin((int(u*4)*17+int(v*4)*23)*2.7)
        c=mix((.22,.23,.22),(.48+.08*shade,.46+.07*shade,.40+.06*shade),mortar)
        c=tuple(clamp(x+.035*n) for x in c);h=.22+.35*mortar+.04*n*mortar
        rough=.91-.12*mortar+.03*n;ao=.65+.35*mortar
    elif kind=='wood':
        grain=math.sin(TAU*(32*v+.34*math.sin(TAU*2*u)+.12*math.sin(TAU*5*u)))
        board=clamp(cell(u,v,4)/.025)
        c=mix((.16,.065,.025),(.58,.31,.12),(.5+.5*grain)*board)
        h=.43+.05*grain-.14*(1-board);rough=.63+.13*grain;ao=.72+.28*board
    elif kind=='ceramic':
        tile=clamp((cell(u,v,6)-.015)/.035)
        c=mix((.24,.29,.30),(.70+.015*n,.88+.015*n,.90+.015*n),tile)
        h=.26+.29*tile;rough=.85*(1-tile)+.17*tile;ao=.7+.3*tile
    elif kind=='rubber':
        lattice=max(abs(math.sin(TAU*(8*u+8*v))),abs(math.sin(TAU*(8*u-8*v))))
        ridge=clamp((lattice-.75)*5)
        c=tuple(x+.018*n+.025*ridge for x in (.065,.075,.08));h=.4+.13*ridge
        rough=.9-.08*ridge;ao=.85+.15*ridge
    elif kind=='paint':
        chips=0 if variant=='clean' else clamp((n-.5)*5)
        stripe=.5+.5*math.sin(TAU*(4*u+4*v))
        c=mix((.88,.28+.06*stripe,.07),(.27,.30,.32),chips)
        h=.58-.06*chips;rough=.4+.35*chips;metal=chips;ao=.95
    elif kind=='fabric':
        weave=math.sin(TAU*32*u)*math.sin(TAU*32*v)
        c=mix((.035,.19,.24),(.13,.43,.49),.5+.5*weave)
        h=.5+.035*weave;rough=.92;ao=.85+.15*(.5+.5*weave)
    elif kind=='panel':
        line=(cell(u,v,4)<.025) or (abs(u-.5)<.02 and .12<v<.88)
        grid=.5+.5*math.sin(TAU*16*u)*math.sin(TAU*16*v)
        c=(.06+.025*grid,.09+.025*grid,.12+.025*grid)
        emission=(.08,.82,.94) if line else (0,0,0)
        h=.43 if line else .51;rough=.32;metal=.65
    elif kind == 'copper':
        patina = clamp((noise(u + .21, v + .14) + .1) * 2)
        c = mix((.72 + .03*n, .32 + .02*n, .13), (.035, .28, .23), patina)
        metal = 1 - .9*patina; rough = .25 + .5*patina; h = .45 + .02*patina
    elif kind == 'concrete':
        pores = clamp((noise(u*13, v*13) - .62)*7)
        c = tuple(value + .04*n - .09*pores for value in (.43, .44, .42))
        h = .45 - .04*pores + .01*n; rough = .88; ao = 1 - .25*pores
    elif kind == 'terrazzo':
        nearest, _, tone = cellular(u, v, 24)
        stone = clamp((.22 + .13*tone - nearest)/.025)
        palette = ((.18,.24,.26), (.55,.30,.22), (.72,.63,.45), (.32,.43,.39))
        c = mix((.80,.77,.68), palette[min(3,int(tone*4))], stone)
        h = .5 + .002*stone; rough = .36 + .08*stone
    elif kind == 'leather':
        nearest, second, tone = cellular(u, v, 32)
        grain = clamp((second - nearest)*4)
        c = tuple(value*(.73 + .27*grain) + .012*(tone-.5) for value in (.29,.105,.045))
        h = .45 + .035*grain; rough = .72 + .1*(1-grain); ao = .75 + .25*grain
    elif kind == 'asphalt':
        grit = .5 + .5*math.sin(TAU*47*u)*math.cos(TAU*43*v)
        c = tuple(value + .018*n + .035*grit for value in (.045,.05,.055))
        h = .5 + .008*grit; rough = .94; ao = .9 + .1*grit
    elif kind == 'brick':
        row = int(v*8)
        gx, gy = u*4 + .5*(row%2), v*8
        fx, fy = gx%1, gy%1
        edge = min(fx*.25, (1-fx)*.25, fy*.125, (1-fy)*.125)
        face = clamp((edge-.005)/.01)
        tone = hash01(int(gx)%4, row, 23)
        c = mix((.32,.31,.28), (.56+.05*tone,.23+.03*tone,.12+.025*tone), face)
        h = .3 + .17*face + .008*n*face; rough = .88; ao = .6 + .4*face
    elif kind == 'marble':
        phase = TAU*(2*u + v + .23*noise(u,v))
        vein = clamp((abs(math.sin(phase))-.88)*9)
        c = mix((.82,.79,.70), (.16,.22,.26), vein*.8)
        h = .5 + .002*vein; rough = .22 + .08*vein
    elif kind == 'plastic':
        peel = math.sin(TAU*32*u)*math.cos(TAU*32*v)
        c = tuple(value*(1+.025*n) for value in (.035,.22,.48))
        h = .5 + .0015*peel; rough = .3 + .04*peel
    elif kind == 'acrylic':
        c = (.32, .68, .74)
        h = .5 + .0005*n; rough = .1
    # Appearance studies within StandardLit's existing single-layer BRDF.
    # No clear-coat, cloth or liquid simulation model is implied.
    if variant=='wet':
        puddle=clamp(.55+.65*noise(u+.13,v+.27))
        c=tuple(x*(.70+.15*(1-puddle)) for x in c)
        rough=rough*(1-puddle)+(.07+.06*noise(u,v))*puddle
        h=h*(1-.35*puddle);ao=.1+.9*ao
    elif variant=='worn':
        scuff=clamp((noise(u+.19,v+.31)+.35)*1.6)
        scratch=clamp((math.sin(TAU*(19*u+7*v))-.94)*13)*scuff
        dirt=clamp((noise(u*1.,v*1.)-.1)*1.8)
        c=mix(c,(.22,.16,.11),dirt*.35)
        c=mix(c,(.62,.59,.51),scratch*.45)
        rough=clamp(rough+.2*scuff+.12*scratch)
        h-=.035*scratch;ao*=1-.25*dirt
        emission=tuple(x*(1-.65*scuff) for x in emission)
    elif variant=='clean':
        rough=clamp(rough*.78+.02)
        c=tuple(clamp(x*1.07) for x in c)
        ao=.12+.88*ao
    return c,h,rough,metal,ao,emission

def png(path,width,height,pixels):
    def chunk(kind,data): return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    rows=b''.join(b'\x00'+pixels[y*width*4:(y+1)*width*4] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b''))
def rgba(rgb): return bytes(round(clamp(x)*255) for x in rgb)+(b'\xff')

def generate():
    textureRoot=ROOT/'textures';textureRoot.mkdir(parents=True,exist_ok=True)
    recipes=[]
    for kind,title in MATERIALS:
        for variant in ['base','clean','wet','worn']:
            identity=kind if variant=='base' else f'{kind}-{variant}'
            data=[surface(kind,x/SIZE,y/SIZE,variant) for y in range(SIZE) for x in range(SIZE)]
            maps={name:bytearray() for name in ['d','n','r','m','ao','e']}
            for y in range(SIZE):
                for x in range(SIZE):
                    c,h,r,m,a,e=data[y*SIZE+x]
                    # Central wrapped height derivatives: density remains periodic.
                    dx=(data[y*SIZE+(x+1)%SIZE][1]-data[y*SIZE+(x-1)%SIZE][1])*(SIZE/64)
                    dy=(data[((y+1)%SIZE)*SIZE+x][1]-data[((y-1)%SIZE)*SIZE+x][1])*(SIZE/64)
                    length=math.sqrt(dx*dx+dy*dy+1)
                    normal=((1-dx/length)*.5,(1-dy/length)*.5,(1+1/length)*.5)
                    for name,color in [('d',c),('n',normal),('r',(r,r,r)),('m',(m,m,m)),('ao',(a,a,a)),('e',e)]:
                        maps[name].extend(rgba(color))
            for suffix,pixels in maps.items():png(textureRoot/f'{identity}_{suffix}.png',SIZE,SIZE,pixels)
            textures={key:f'textures/{identity}_{suffix}.png' for key,suffix in [
                ('baseColorTexture','d'),('normalTexture','n'),('roughnessTexture','r'),
                ('metallicTexture','m'),('aoTexture','ao'),('emissiveTexture','e')]}
            recipes.append(dict(id=identity,name=title+('' if variant=='base' else ' / '+variant.title()),baseColor=[1,1,1,1],metallic=1,roughness=1,ao=1,
                                emissive=[1,1,1] if kind=='panel' else [0,0,0],uvTransform=[1,1,0,0],
                                normalStrength=1,roughnessBias=0,textures=textures))
            if kind == 'acrylic':
                # Existing straight-alpha StandardLit; no refraction/transmission BRDF.
                recipes[-1].update(baseColor=[1,1,1,.28], alphaMode=2)
            print(identity,flush=True)
    # Portable one-control studies reuse the source maps without duplicating PNGs.
    studies=[
        ('study-uv-half','UV 0.5x','wood',[.5,.5,0,0],1,0),
        ('study-uv-one','UV 1x','wood',[1,1,0,0],1,0),
        ('study-uv-two','UV 2x','wood',[2,2,0,0],1,0),
        ('study-uv-four','UV 4x','wood',[4,4,0,0],1,0),
        ('study-normal-zero','Normal 0','ceramic',[1,1,0,0],0,0),
        ('study-normal-half','Normal 0.5','ceramic',[1,1,0,0],.5,0),
        ('study-normal-one','Normal 1','ceramic',[1,1,0,0],1,0),
        ('study-normal-two','Normal 2','ceramic',[1,1,0,0],2,0),
        ('study-rough-low','Rough -0.25','metal',[1,1,0,0],1,-.25),
        ('study-rough-zero','Rough +0','metal',[1,1,0,0],1,0),
        ('study-rough-mid','Rough +0.25','metal',[1,1,0,0],1,.25),
        ('study-rough-high','Rough +0.5','metal',[1,1,0,0],1,.5),
    ]
    sources={item['id']:item for item in recipes}
    for variant in ['base','clean','wet','worn']:
        suffix='' if variant=='base' else '-'+variant
        for identity,label,source,uv,normal,rough in studies:
            study=dict(sources[source+suffix])
            study.update(id=identity+suffix,name=label+' / '+source+suffix,
                         uvTransform=uv,normalStrength=normal,roughnessBias=rough)
            recipes.append(study)
    # Equivalent alpha footprints carried by three explicit channel contracts.
    base_alpha=bytearray();base_opaque=bytearray();opacity=bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            u=x/SIZE;v=y/SIZE
            du=(u*8)%1-.5;dv=(v*8)%1-.5
            coverage=255 if du*du+dv*dv>.29*.29 else 0
            base_alpha.extend(bytes((147,167,182,coverage)))
            base_opaque.extend(bytes((147,167,182,255)))
            opacity.extend(bytes((coverage,coverage,coverage,coverage)))
    png(textureRoot/'screen_base_alpha.png',SIZE,SIZE,base_alpha)
    png(textureRoot/'screen_base_opaque.png',SIZE,SIZE,base_opaque)
    png(textureRoot/'screen_opacity.png',SIZE,SIZE,opacity)
    for variant in ['base','clean','wet','worn']:
        suffix='' if variant=='base' else '-'+variant
        for identity,label,source in [('screen-base-alpha','Base color alpha',0),
                                      ('screen-opacity-red','Opacity red',1),
                                      ('screen-opacity-alpha','Opacity alpha',2)]:
            screen=dict(sources['metal'+suffix])
            screen.update(id=identity+suffix,name='Perforated screen / '+label+suffix,
                          alphaMode=1,alphaCutoff=.5,doubleSided=True,opacitySource=source,
                          uvTransform=[2,2,.13,.19],normalStrength=.5)
            screen['textures']=dict(screen['textures'])
            screen['textures']['baseColorTexture']='textures/screen_base_alpha.png' if source==0 else 'textures/screen_base_opaque.png'
            if source:
                screen['textures']['opacityTexture']='textures/screen_opacity.png'
            recipes.append(screen)
    (ROOT/'library.json').write_text(json.dumps(dict(schemaVersion=1,materials=recipes),indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT,help='material-library directory')
    parser.add_argument('--size',type=int,choices=[128,256,512,1024],default=256,
                        help='square map resolution; normal slopes keep the same UV-space scale')
    arguments=parser.parse_args()
    ROOT=arguments.output.resolve();SIZE=arguments.size
    generate()
