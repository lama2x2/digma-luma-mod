# иконка температуры (термометр) в формате LVGL 9 I4 без сжатия
import struct,sys
from PIL import Image,ImageDraw
def draw(n):
    K=16; S=n*K; im=Image.new('L',(S,S),0); d=ImageDraw.Draw(im)
    u=S/32.0
    lw=int(1.7*u)
    cx=14*u
    # трубка
    tw=6.5*u; top=3*u; bulb_c=24*u; br=5.5*u
    d.rounded_rectangle([cx-tw/2,top,cx+tw/2,bulb_c],radius=tw/2,outline=255,width=lw)
    d.ellipse([cx-br,bulb_c-br,cx+br,bulb_c+br],outline=255,width=lw)
    # стираем стык трубки и колбы внутри
    ir=br-lw; d.ellipse([cx-ir,bulb_c-ir,cx+ir,bulb_c+ir],fill=0)
    d.rectangle([cx-tw/2+lw,bulb_c-br-1*u,cx+tw/2-lw,bulb_c],fill=0)
    # ртуть
    mr=2.6*u; d.ellipse([cx-mr,bulb_c-mr,cx+mr,bulb_c+mr],fill=255)
    mw=1.0*u; d.rectangle([cx-mw,11*u,cx+mw,bulb_c],fill=255)
    # деления справа
    for y in (6,10,14):
        d.rectangle([cx+tw/2+2*u,y*u,cx+tw/2+2*u+(5 if y==10 else 3.5)*u,y*u+1.7*u],fill=255)
    return im.resize((n,n),Image.LANCZOS)
def write(n,path):
    a=draw(n).load()
    hdr=struct.pack('<BBHHHHH',0x19,9,0,n,n,n//2,0)
    pal=b''.join(bytes((0,0,0,i*17)) for i in range(16))
    px=bytearray()
    for y in range(n):
        for x in range(0,n,2):
            q=lambda v:(v*15+127)//255
            px.append(q(a[x,y])<<4|q(a[x+1,y]))
    open(path,'wb').write(hdr+pal+px)
for n,p in zip(map(int,sys.argv[1::2]),sys.argv[2::2]): write(n,p)
