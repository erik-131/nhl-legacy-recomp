# nhl-build CT — build/test loop without the Windows PC

```
 cloud Claude session  ──(cross-session messages)──>  Claude Code in CT "nhl-build" (Remote Control)
                                                        │  builds from github.com/erik-131/nhl-legacy-recomp (nuc-perf)
                                                        │  ssh -i nhl_gate  (forced command, from=<this CT>)
                                                        ▼
                                                     CT 105 (Wolf): /root/nhlbench/nhl_gate.sh
                                                        install | bench | status | result | shot | log | xex | stop
```

## 1. Create the CT (on the Proxmox host)
Ubuntu 24.04 template, **unprivileged**, no GPU, e.g. 8 cores / 12 GB RAM / 60 GB disk:
```
pct create 108 local:vztmpl/ubuntu-24.04-standard_24.04-2_amd64.tar.zst \
  --hostname nhl-build --unprivileged 1 --cores 8 --memory 12288 --swap 2048 \
  --rootfs local-lvm:60 --net0 name=eth0,bridge=vmbr0,ip=dhcp --features nesting=1 --onboot 0
pct start 108
```
(Adjust template name / storage / bridge to your host. 60 GB: SDK ~2 GB, generated code ~0.4 GB, build ~10 GB.)

## 2. Inside CT 108
```
pct enter 108
apt-get update && apt-get install -y git
git clone --branch nuc-perf https://github.com/erik-131/nhl-legacy-recomp /opt/nhl/nhl-legacy-recomp
bash /opt/nhl/nhl-legacy-recomp/tools/nuc/buildct/setup_build_ct.sh
```
It prints the public key `nhl-build`.

## 3. Install the gate on CT 105
In CT 105's console (replace the key and the build CT's IP):
```
curl -fsSL https://raw.githubusercontent.com/erik-131/nhl-legacy-recomp/nuc-perf/tools/nuc/buildct/nhl_gate.sh -o /root/nhlbench/nhl_gate.sh
chmod 700 /root/nhlbench/nhl_gate.sh
echo 'restrict,from="<BUILD_CT_IP>",command="/root/nhlbench/nhl_gate.sh" <PASTE nhl-build PUBLIC KEY>' >> /root/.ssh/authorized_keys
```
Test from CT 108: `ssh -i /root/.ssh/nhl_gate root@192.168.0.22 status`

What the key can do: only the gate verbs. `bench` accepts only `NHL_*`/`REX_*` env vars with plain values;
`install` replaces only the three binaries in `nhl-linux/`. It cannot run arbitrary commands in CT 105.

## 4. First build + Claude Code
```
cd /opt/nhl/nhl-legacy-recomp && tools/nuc/buildct/nhl_build.sh --deploy     # ~5-10 min on 8 cores
claude            # log in with your claude.ai account, then: /remote-control
```
Keep that session running (tmux/screen). The cloud session reaches it via ListAgents/SendMessage.
Settings for unattended use: in CT 108 `~/.claude/settings.json` set `"crossSessionInbound": "accept"`.
