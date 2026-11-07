import sys,re,datetime
def ts(l): return datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()
L=open(sys.argv[1],errors='replace').read().splitlines()
g=[(ts(l),int(re.search(r' draws/frame=(\d+)',l)[1]),float(re.search(r'fps=([\d.]+)',l)[1])) for l in L if 'nhl-vk-fps' in l]
s=next(i for i in range(len(g)-5) if all(p[1]>1000 for p in g[i:i+5])); t0=g[s][0]+15; t1=t0+60
gf=sorted(p[2] for p in g if t0<=p[0]<=t1)
pr=[(ts(l),float(re.search(r' fps=([\d.]+)',l)[1]),float(re.search(r'max_gap=([\d.]+)',l)[1]),int(re.search(r'gaps>50ms=(\d+)',l)[1]),float(re.search(r'in_present=([\d.]+)',l)[1])) for l in L if 'nhl-present' in l]
w=[p for p in pr if t0<=p[0]<=t1]; pf=sorted(p[1] for p in w); n=len(pf)
q=lambda a,x: a[int(x*(len(a)-1))]
print(f"guest fps  p5={q(gf,.05):.1f} median={q(gf,.5):.1f}")
print(f"present fps p5={q(pf,.05):.1f} median={q(pf,.5):.1f} min={pf[0]:.1f}; max gap {max(p[2] for p in w):.0f} ms; gaps>50ms total {sum(p[3] for p in w)}; in vkQueuePresent avg {sum(p[4] for p in w)/n:.0f} ms/s")
allgaps=sorted(((p[2],datetime.datetime.fromtimestamp(p[0]).strftime('%H:%M:%S')) for p in pr),reverse=True)[:5]
print("largest present gaps whole run:",allgaps)
la=[(ts(l),float(re.search(r'avg=([\d.]+)',l)[1])) for l in L if 'nhl-latency' in l]
w=[a for t,a in la if t0<=t<=t1]
if w: print(f"frames ahead of GPU: avg {sum(w)/len(w):.2f} (x frame time {1000/q(pf,.5):.1f} ms = ~{sum(w)/len(w)*1000/q(pf,.5):.0f} ms queue latency)")
