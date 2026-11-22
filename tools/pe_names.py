import struct,sys
d=open(sys.argv[1],'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]; optsz=struct.unpack_from('<H',d,pe+20)[0]
opt=pe+24; magic=struct.unpack_from('<H',d,opt)[0]
ddoff=opt+(112 if magic==0x20b else 96)
secs=[struct.unpack_from('<8sIIII',d,opt+optsz+i*40) for i in range(nsec)]
def r2o(rva):
    for n,vs,va,rs,ro in secs:
        if va<=rva<va+max(vs,rs): return rva-va+ro
def cstr(o): return d[o:d.index(b'\0',o)].decode()
mode=sys.argv[2]
if mode=='imp':
    rva=struct.unpack_from('<I',d,ddoff+8)[0]; o=r2o(rva)
    while True:
        oft,_,_,name,ft=struct.unpack_from('<IIIII',d,o)
        if name==0: break
        dll=cstr(r2o(name))
        if 'steam' in dll.lower() or (len(sys.argv)>3 and sys.argv[3].lower() in dll.lower()):
            t=r2o(oft or ft)
            while True:
                v=struct.unpack_from('<Q',d,t)[0]
                if v==0: break
                print(dll, cstr(r2o(v&0x7fffffff)+2) if not v>>63 else '#%d'%(v&0xffff)); t+=8
        o+=20
else:
    rva=struct.unpack_from('<I',d,ddoff)[0]; o=r2o(rva)
    n=struct.unpack_from('<I',d,o+24)[0]; names=r2o(struct.unpack_from('<I',d,o+32)[0])
    for i in range(n): print(cstr(r2o(struct.unpack_from('<I',d,names+4*i)[0])))
