import numpy as np, zipfile, sys
BASE=bytes.fromhex("361e615a53085ac3")
KEY=np.array([(BASE[i&7]+(i&0xf8))&0xff for i in range(256)],dtype=np.uint8)
def decrypt_stream(fin,fout,total):
    off=0
    while True:
        b=fin.read(1<<24)
        if not b: break
        a=np.frombuffer(b,dtype=np.uint8)
        k=np.resize(KEY, (len(a)+255)//256*256)[0:len(a)] if off%256==0 else None
        idx=(np.arange(off,off+len(a))&0xff); k=KEY[idx]
        fout.write((a-k).astype(np.uint8).tobytes()); off+=len(a)
if __name__=="__main__":
    z=zipfile.ZipFile("D:/programming/GARMIN_EMU/GPSMAPSerieswithSDCard_202608031.zip")
    for n in sys.argv[1:]:
        e=z.getinfo("Garmin/updates/"+n)
        with z.open(e) as f, open("dat/"+n.replace(".dat",".bin"),"wb") as o: decrypt_stream(f,o,e.file_size)
        print("decrypted",n,e.file_size)
