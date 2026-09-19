import re,sys
d=open(sys.argv[1],"rb").read(); n=int(sys.argv[2]) if len(sys.argv)>2 else 8
for s in re.findall(rb'[\x20-\x7e]{%d,}'%n,d): print(s.decode())
