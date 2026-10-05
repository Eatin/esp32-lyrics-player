import os,sys,subprocess,threading,time,tarfile
URL=sys.argv[1]; TOTAL=int(sys.argv[2]); NAME=sys.argv[3]
OUT=r"D:/wb/wave/tools/dl"
os.makedirs(OUT,exist_ok=True)
FINAL=os.path.join(OUT,NAME)
if os.path.exists(FINAL) and os.path.getsize(FINAL)==TOTAL:
    print("already complete",flush=True); sys.exit(0)
CHUNK=8*1024*1024
N=(TOTAL+CHUNK-1)//CHUNK
PARTS=os.path.join(OUT,"_parts_"+NAME)
os.makedirs(PARTS,exist_ok=True)
def exp(i): return min((i+1)*CHUNK,TOTAL)-i*CHUNK
def ok(i):
    p=os.path.join(PARTS,"%05d"%i)
    return os.path.exists(p) and os.path.getsize(p)==exp(i)
todo=[i for i in range(N) if not ok(i)]
print("chunks",N,"todo",len(todo),flush=True)
done=set(); lock=threading.Lock()
def worker(i,wid):
    s=i*CHUNK; e=s+exp(i)-1
    p=os.path.join(PARTS,"%05d"%i); part=p+".part"
    for a in range(1,25):
        if i in done or ok(i): return
        subprocess.call(["curl","-sS","-L","--fail","--connect-timeout","15","--max-time","900",
                         "-r","%d-%d"%(s,e),"-o",part,URL],stderr=subprocess.DEVNULL)
        if os.path.exists(part) and os.path.getsize(part)==exp(i):
            for _ in range(8):
                try:
                    os.replace(part,p)
                    with lock: done.add(i)
                    return
                except PermissionError: time.sleep(1)
        time.sleep(0.5)
NW=10
queue=list(todo)
qlock=threading.Lock()
def runner(wid):
    while True:
        with qlock:
            if not queue: return
            i=queue.pop(0)
        worker(i,wid)
t0=time.time()
ths=[threading.Thread(target=runner,args=(w,),daemon=True) for w in range(NW)]
for t in ths: t.start()
last=0
while True:
    rem=[i for i in todo if not ok(i)]
    if not rem: break
    if time.time()-t0>3600: break
    cur=sum(os.path.getsize(os.path.join(PARTS,f)) for f in os.listdir(PARTS) if f[:5].isdigit())
    print("  %.1f%% (%d/%d) %.0fs  %.2f MB/s" % (100.0*cur/TOTAL, (len(todo)-len(rem)), len(todo), time.time()-t0, cur/1048576.0/max(time.time()-t0,1)),flush=True)
    time.sleep(20)
rem=[i for i in todo if not ok(i)]
print("remaining",len(rem),"elapsed %.0f s"%(time.time()-t0),flush=True)
if rem: sys.exit(1)
out=FINAL+".assembling"
with open(out,"wb") as o:
    for i in range(N):
        with open(os.path.join(PARTS,"%05d"%i),"rb") as f:
            while True:
                b=f.read(1<<20)
                if not b: break
                o.write(b)
sz=os.path.getsize(out); print("assembled",sz,"expect",TOTAL,flush=True)
assert sz==TOTAL
for _ in range(10):
    try: os.replace(out,FINAL); break
    except PermissionError: time.sleep(1)
print("DONE",FINAL,flush=True)
