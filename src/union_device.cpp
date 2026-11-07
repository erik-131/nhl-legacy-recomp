// nhllegacy - loose-file overlay device. See union_device.h.

#include "union_device.h"

#include <cstdlib>
#include <system_error>
#include <string>

#include <rex/logging.h>

namespace nhllegacy {

namespace fs = rex::filesystem;

UnionDevice::UnionDevice(std::string_view mount_path,
                         const std::filesystem::path& writable_host_path,
                         const std::filesystem::path& readonly_host_path,
                         const std::filesystem::path& override_root)
    : fs::Device(mount_path),
      upper_(mount_path, writable_host_path, /*read_only=*/false),
      lower_(mount_path, readonly_host_path, override_root),
      probe_(std::getenv("NHL_LOOSE_PROBE") != nullptr) {}

UnionDevice::~UnionDevice() = default;

bool UnionDevice::Initialize() {
  // The writable scratch layer is required (the guest probes/writes cache: on
  // boot). The loose-asset layer is best-effort: if it fails to initialize we
  // still want a functional writable cache rather than a failed mount.
  if (!upper_.Initialize()) {
    return false;
  }
  lower_active_ = lower_.Initialize();
  return true;
}

void UnionDevice::SetMods(std::shared_ptr<const mods::ModIndex> index) {
  mods_.reset();
  mod_devs_.clear();
  if (!index || index->empty()) return;
  mod_devs_.resize(index->mods().size());
  for (size_t i = 0; i < index->mods().size(); ++i) {
    const auto& m = index->mods()[i];
    if (!m.enabled || m.files == 0) continue;
    auto dev = std::make_unique<fs::HostPathDevice>(mount_path(), m.dir, /*read_only=*/true);
    if (dev->Initialize()) mod_devs_[i] = std::move(dev);
    else REXLOG_WARN("[nhl-mods] mod '{}' device failed to initialize; its files are skipped", m.id);
  }
  mods_ = std::move(index);
}

void UnionDevice::Dump(rex::string::StringBuffer* string_buffer) {
  upper_.Dump(string_buffer);
}

fs::Entry* UnionDevice::ResolvePath(std::string_view path) {
  // Upper (scratch) wins so the game sees its own writes; otherwise fall
  // through to the read-only loose tree. Resolving a parent directory for a
  // create returns the writable upper entry, so new files land in scratch.
  //
  // Note: the guest opens most asset files RELATIVE to a directory handle, so
  // those go through Entry::GetChild on the resolved directory entry rather
  // than here — which is why per-file .rx2 synth lives in LooseTreeDevice's
  // entry tree, not in this method.
  fs::Entry* upper = upper_.ResolvePath(path);
  fs::Entry* result = upper;
  const char* mod_layer = nullptr;
  if (!result && mods_) {
    // User mods: one hash lookup (no directory probing). Only FILES are served here;
    // directories keep resolving through scratch/loose as before. The entry comes from
    // the winning mod's own read-only device, addressed by its exact on-disk case.
    if (const mods::FileHit* hit = mods_->Resolve(path)) {
      if (hit->mod < mod_devs_.size() && mod_devs_[hit->mod]) {
        std::error_code ec;
        const std::string rel =
            std::filesystem::relative(hit->host, mods_->mods()[hit->mod].dir, ec).generic_string();
        if (!ec && !rel.empty()) {
          result = mod_devs_[hit->mod]->ResolvePath(rel);
          if (result) mod_layer = "mod    ";
        }
      }
    }
  }
  if (!result) result = lower_.ResolvePath(path);

  if (probe_) {
    const char* layer = upper ? "scratch" : mod_layer ? mod_layer : (result ? "loose  " : "miss   ");
    std::string key(path);
    std::lock_guard<std::mutex> guard(probe_mu_);
    if (probe_seen_.insert(key).second) {
      REXLOG_INFO("[loose-probe] {} {}", layer, key);
    }
  }
  return result;
}

}  // namespace nhllegacy
