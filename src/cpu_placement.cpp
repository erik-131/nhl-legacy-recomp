// See cpu_placement.h.
//
// On a hybrid CPU whose integrated GPU shares the power budget, the game runs better on one E-core
// cluster (cores sharing an L2) than on P-cores: Core Ultra 5 225H, A/B in one match, ~8 fps more and
// ~100 MHz more GPU clock, CPU per frame 36.1 -> 24.9 ms. Shader compilation goes on the other cores:
// inside the cluster it made freezes on new shaders 2-3 times longer (worst 3.1 s v 0.5 s).
// Not used with a discrete GPU, or on CPUs without E-cores.

#include "cpu_placement.h"

#include <sched.h>

#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(nhl_cpu_cluster, "auto", "NHL", "Game threads on one E-core cluster: auto, on or off");

namespace nhllegacy {

namespace {

std::string ReadLine(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

// "4-7,12" -> set.
cpu_set_t ParseCpuList(const std::string& list) {
  cpu_set_t set;
  CPU_ZERO(&set);
  std::stringstream ss(list);
  std::string part;
  while (std::getline(ss, part, ',')) {
    const auto dash = part.find('-');
    if (part.empty()) continue;
    const int first = std::atoi(part.c_str());
    const int last = dash == std::string::npos ? first : std::atoi(part.c_str() + dash + 1);
    for (int cpu = first; cpu <= last && cpu < CPU_SETSIZE; ++cpu) CPU_SET(cpu, &set);
  }
  return set;
}

// Any display adapter other than Intel's means the GPU has its own power budget.
bool HasOtherGpu() {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
    const std::string vendor = ReadLine(entry.path().string() + "/device/vendor");
    if (!vendor.empty() && vendor != "0x8086") return true;
  }
  return false;
}

}  // namespace

void StartCpuPlacement() {
  const std::string mode = REXCVAR_GET(nhl_cpu_cluster);
  if (mode == "off") return;
  if (mode != "on" && HasOtherGpu()) return;
  const std::string atom = ReadLine("/sys/devices/cpu_atom/cpus");
  if (atom.empty()) return;
  cpu_set_t allowed;
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return;
  const int first_atom = std::atoi(atom.c_str());
  cpu_set_t cluster = ParseCpuList(
      ReadLine("/sys/devices/system/cpu/cpu" + std::to_string(first_atom) + "/cache/index2/shared_cpu_list"));
  CPU_AND(&cluster, &cluster, &allowed);
  cpu_set_t rest;
  CPU_XOR(&rest, &allowed, &cluster);
  if (CPU_COUNT(&cluster) < 4 || !CPU_COUNT(&rest)) return;
  REXLOG_INFO("[nhl-cpu] game on an E-core cluster of {}, shader compilation on the other {} cores", CPU_COUNT(&cluster),
              CPU_COUNT(&rest));
  std::thread([cluster, rest] {
    // New threads inherit the creator's affinity, and pipeline threads start later, so keep looking.
    for (;;) {
      if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* entry = readdir(dir)) {
          if (entry->d_name[0] == '.') continue;
          const std::string comm = ReadLine(std::string("/proc/self/task/") + entry->d_name + "/comm");
          const bool compile = comm.rfind("Vulkan Pipeline", 0) == 0;
          sched_setaffinity(std::atoi(entry->d_name), sizeof(cpu_set_t), compile ? &rest : &cluster);
        }
        closedir(dir);
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();
}

}  // namespace nhllegacy
