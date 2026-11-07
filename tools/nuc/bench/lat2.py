import sys,re
L=open(sys.argv[1],errors='replace').read().splitlines()
ev={}; order=[]
for l in L:
    m=re.search(r'\[nhl-lat\] press=(\d+) dt=([\d.]+)ms draws=(\d+)',l)
    if m:
        if m[1] not in ev: order.append(m[1])
        ev.setdefault(m[1],[]).append((float(m[2]),int(m[3])))
g=[k for k in order if ev[k][0][1]>1000][-22:]   # gameplay presses: start,a,start,a,...
out={'start':[],'a':[]}
for i,k in enumerate(g):
    fr=ev[k]; base=fr[0][1]
    hit=next((dt for dt,d in fr[1:] if abs(d-base)>=40),None)
    if hit is not None: out['start' if i%2==0 else 'a'].append(hit)
for b,v in out.items():
    v.sort(); n=len(v)
    if n: print(f"  {b:5s}: N={n} median {v[n//2]:.0f} ms (min {v[0]:.0f}, max {v[-1]:.0f})")
