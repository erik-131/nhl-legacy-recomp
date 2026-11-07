#!/bin/bash
# nhl_pin.sh <pid>: keep the two heavy game threads - "GPU Commands" (command processor) and the busiest
# XThread (main guest thread) - on the P-cores this container may use. Unpinned, the scheduler keeps the
# main thread on a P-core but parks the command processor on the E-cores most of the time (99 of 120 s).
# Pinning both looks like a gain but its size is uncertain: unpinned 73.0/75.4 fps median, the same binary
# pinned 86.8/87.5 in one pair of runs and 80.3/71.9 in another (2026-11-04, Core Ultra 5 225H). Started by nhl_play.sh; NHL_PIN_PCORES=0 disables it.
P=$1
expand() { tr ',' '\n' | awk -F- '{ if (NF == 2) for (i = $1; i <= $2; i++) print i; else if ($1 != "") print $1 }'; }
allowed=$(expand < /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null || expand < /sys/devices/system/cpu/online)
pcores=$(comm -12 <(expand < /sys/devices/cpu_core/cpus 2>/dev/null | sort) <(echo "$allowed" | sort) | sort -n | paste -sd,)
[ -z "$pcores" ] && exit 0
all=$(echo "$allowed" | paste -sd,)
# NHL_CPU_CLUSTER=1: everything on the first E-core cluster, shader compile threads on all the other cores.
if [ "${NHL_CPU_CLUSTER:-0}" != 0 ] && [ -r /sys/devices/cpu_atom/cpus ]; then
  first=$(cut -d- -f1 /sys/devices/cpu_atom/cpus | cut -d, -f1)
  cluster=$(expand < "/sys/devices/system/cpu/cpu$first/cache/index2/shared_cpu_list" | sort -n)
  if [ -n "$cluster" ] && [ -n "$(comm -12 <(echo "$cluster" | sort) <(echo "$allowed" | sort))" ]; then
    rest=${NHL_COMPILE_CPUS:-$(comm -13 <(echo "$cluster" | sort) <(echo "$allowed" | sort) | sort -n | paste -sd,)}
    cluster=$(echo "$cluster" | paste -sd,)
    while [ -d /proc/$P ]; do
      for t in /proc/$P/task/*; do
        case "$(cat $t/comm 2>/dev/null)" in
          "Vulkan Pipeline"*) taskset -pc "$rest" "${t##*/}" >/dev/null 2>&1;;
          *) taskset -pc "$cluster" "${t##*/}" >/dev/null 2>&1;;
        esac
      done
      sleep 2
    done
    exit 0
  fi
fi
cpu_time() { sed 's/.*) //' "$1/stat" 2>/dev/null | awk '{ print $12 + $13 }'; }
cp="" mt=""
while [ -d /proc/$P ]; do
  if [ -z "$cp" ]; then
    for t in /proc/$P/task/*; do
      case "$(cat $t/comm 2>/dev/null)" in "GPU Commands"*) cp=${t##*/}; taskset -pc "$pcores" "$cp" >/dev/null 2>&1;; esac
    done
  fi
  best="" best_time=-1
  for t in /proc/$P/task/*; do
    case "$(cat $t/comm 2>/dev/null)" in XThread*) ;; *) continue;; esac
    c=$(cpu_time $t); [ -n "$c" ] && [ "$c" -gt "$best_time" ] && { best=${t##*/}; best_time=$c; }
  done
  if [ -n "$best" ] && [ "$best" != "$mt" ]; then
    [ -n "$mt" ] && taskset -pc "$all" "$mt" >/dev/null 2>&1
    taskset -pc "$pcores" "$best" >/dev/null 2>&1 && mt=$best
  fi
  sleep 10
done
