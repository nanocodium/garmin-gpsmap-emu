BASE=bytes.fromhex("361e615a53085ac3")
def key(i): return (BASE[i&7]+(i&0xf8))&0xff
def dec(buf,off=0):
    return bytes((buf[j]-key((off+j)&0xff))&0xff for j in range(len(buf)))
