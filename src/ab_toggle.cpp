// See ab_toggle.h.

#include "ab_toggle.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace nhllegacy {

namespace {

struct Toggle {
  std::string name, a, b;
};

}  // namespace

void StartAbToggle() {
  const char* spec_env = std::getenv("NHL_AB_TOGGLE");
  if (!spec_env || !*spec_env) return;
  std::string spec(spec_env);
  unsigned period_s = 20;
  if (size_t at = spec.rfind('@'); at != std::string::npos) {
    period_s = std::max(1, std::atoi(spec.c_str() + at + 1));
    spec.resize(at);
  }
  std::vector<Toggle> toggles;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t end = spec.find(';', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    size_t eq = item.find('='), comma = item.find(',', eq == std::string::npos ? 0 : eq);
    if (eq != std::string::npos && comma != std::string::npos) {
      toggles.push_back({item.substr(0, eq), item.substr(eq + 1, comma - eq - 1),
                         item.substr(comma + 1)});
    }
    pos = end + 1;
  }
  if (toggles.empty()) {
    REXLOG_WARN("[nhl-ab] NHL_AB_TOGGLE parsed to nothing: '{}'", spec_env);
    return;
  }

  std::thread([toggles = std::move(toggles), period_s]() {
    for (bool b = false;; b = !b) {
      for (const Toggle& t : toggles) {
        if (!rex::cvar::SetFlagByName(t.name, b ? t.b : t.a)) {
          REXLOG_WARN("[nhl-ab] could not set {}={}", t.name, b ? t.b : t.a);
        }
      }
      REXLOG_INFO("[nhl-ab] phase={}", b ? 'B' : 'A');
      std::this_thread::sleep_for(std::chrono::seconds(period_s));
    }
  }).detach();
}

}  // namespace nhllegacy
