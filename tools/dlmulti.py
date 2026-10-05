import os,sys,subprocess,threading,time
NAME="esp32-arduino-libs-idf-release_v5.5-b66b5448-v1.zip"
REL="espressif/esp32-arduino-lib-builder/releases/download/idf-release_v5.5/"+NAME
TOTAL=430471837
PROXIES=["https://ghproxy.net/","https://gh-proxy.com/","https://ghfast.top/"]
OUT=r"D:/wb/wave/tools/dl"
FINAL=os.path.join(OUT,NAME)
if os.path.exists(FINAL) and os.path.getsize(FINAL)==TOTAL:
    print("already complete",flush=True); sys.exit(0)
CHUNK=8*1024*1024
N=(TOTAL+CHUNK-1)//CHUNK
PARTS=os.path.join(OUT,"_parts_libs55")
os.makedirs(PARTS,exist_ok=True)
def exp(i): return min((i+1)*CHUNK,TOTAL)-i*CHUNK
def ok(i):
    p=os.path.join(PARTS,"%05d"%i)
    return os.path.exists(p) and os.path.getsize(p)==exp(i)
todo=[i for i in range(N) if not ok(i)]
print("chunks",N,"todo",len(todo),flush=True)
done=set(); lock=threading.Lock(); queue=list(todo); qlock=threading.Lock()
def worker(i):
    s=i*CHUNK; e=s+exp(i)-1
    p=os.path.join(PARTS,"%05d"%i); part=p+".part"
    for a in range(1,40):
        if i in done or ok(i): return
        pr=PROXIES[a%len(PROXIES)]
        subprocess.call(["curl","-sS","-L","--fail","--connect-timeout","15","--max-time","900",
                         "-r","%d-%d"%(s,e),"-o",part,pr+"https://github.com/"+REL],stderr=subprocess.DEVNULL)
        if os.path.exists(part) and os.path.getsize(part)==exp(i):
            for _ in range(8):
                try:
                    os.replace(part,p)
                    with lock: done.add(i)
                    return
                except PermissionError: time.sleep(1)
        time.sleep(0.5)
        pr=PROXIES[(a+1)%len(PROXIES)]
def runner():
    while True:
        with qlock:
            if not queue: return
            i=queue.pop(0)
        worker(i)
NW=12
t0=time.time()
for _ in range(NW): threading.Thread(target=runner,daemon=True).start()
while True:
    rem=[i for i in todo if not ok(i)]
    if not rem: break
    if time.time()-t0>5400: break
    cur=sum(os.path.getsize(os.path.join(PARTS,f)) for f in os.listdir(PARTS) if f[:5].isdigit())
    print("  %.1f%% (%d/%d) %.0fs %.2f MB/s" % (100.0*cur/TOTAL, len(todo)-len(rem), len(todo), time.time()-t0, cur/1048576.0/max(time.time()-t0,1)),flush=True)
    time.sleep(30)
rem=[i for i in todo if not ok(i)]
print("remaining",len(rem),"elapsed %.0f"%(time.time()-t0),flush=True)
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
