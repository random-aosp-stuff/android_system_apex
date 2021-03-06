/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_APEXD_APEX_DATABASE_H_
#define ANDROID_APEXD_APEX_DATABASE_H_

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include <android-base/logging.h>
#include <android-base/result.h>
#include <android-base/thread_annotations.h>

namespace android {
namespace apex {

class MountedApexDatabase {
 public:
  // Stores associated low-level data for a mounted APEX. To conserve memory,
  // the APEX file isn't stored, but must be opened to retrieve specific data.
  struct MountedApexData {
    std::string loop_name;  // Loop device used (fs path).
    std::string full_path;  // Full path to the apex file.
    std::string mount_point;  // Path this apex is mounted on.
    std::string device_name;  // Name of the dm verity device.
    // Name of the loop device backing up hashtree or empty string in case
    // hashtree is embedded inside an APEX.
    std::string hashtree_loop_name;
    // Whenever apex file specified in full_path was deleted.
    bool deleted;
    // Whether the mount is a temp mount or not.
    bool is_temp_mount;

    MountedApexData() {}
    MountedApexData(const std::string& loop_name, const std::string& full_path,
                    const std::string& mount_point,
                    const std::string& device_name,
                    const std::string& hashtree_loop_name,
                    bool is_temp_mount = false)
        : loop_name(loop_name),
          full_path(full_path),
          mount_point(mount_point),
          device_name(device_name),
          hashtree_loop_name(hashtree_loop_name),
          is_temp_mount(is_temp_mount) {}

    inline bool operator<(const MountedApexData& rhs) const {
      int compare_val = loop_name.compare(rhs.loop_name);
      if (compare_val < 0) {
        return true;
      } else if (compare_val > 0) {
        return false;
      }
      compare_val = full_path.compare(rhs.full_path);
      if (compare_val < 0) {
        return true;
      } else if (compare_val > 0) {
        return false;
      }
      compare_val = mount_point.compare(rhs.mount_point);
      if (compare_val < 0) {
        return true;
      } else if (compare_val > 0) {
        return false;
      }
      compare_val = device_name.compare(rhs.device_name);
      if (compare_val < 0) {
        return true;
      } else if (compare_val > 0) {
        return false;
      }
      return hashtree_loop_name < rhs.hashtree_loop_name;
    }
  };

  template <typename... Args>
  inline void AddMountedApexLocked(const std::string& package, bool latest,
                                   Args&&... args)
      REQUIRES(mounted_apexes_mutex_) {
    auto it = mounted_apexes_.find(package);
    if (it == mounted_apexes_.end()) {
      auto insert_it =
          mounted_apexes_.emplace(package, std::map<MountedApexData, bool>());
      CHECK(insert_it.second);
      it = insert_it.first;
    }

    auto check_it = it->second.emplace(
        MountedApexData(std::forward<Args>(args)...), latest);
    CHECK(check_it.second);

    CheckAtMostOneLatest();
    CheckUniqueLoopDm();
  }

  template <typename... Args>
  inline void AddMountedApex(const std::string& package, bool latest,
                             Args&&... args) REQUIRES(!mounted_apexes_mutex_) {
    std::lock_guard lock(mounted_apexes_mutex_);
    AddMountedApexLocked(package, latest, args...);
  }

  inline void RemoveMountedApex(const std::string& package,
                                const std::string& full_path,
                                bool match_temp_mounts = false)
      REQUIRES(!mounted_apexes_mutex_) {
    std::lock_guard lock(mounted_apexes_mutex_);
    auto it = mounted_apexes_.find(package);
    if (it == mounted_apexes_.end()) {
      return;
    }

    auto& pkg_map = it->second;

    for (auto pkg_it = pkg_map.begin(); pkg_it != pkg_map.end(); ++pkg_it) {
      if (pkg_it->first.full_path == full_path &&
          pkg_it->first.is_temp_mount == match_temp_mounts) {
        pkg_map.erase(pkg_it);
        return;
      }
    }
  }

  inline void SetLatest(const std::string& package,
                        const std::string& full_path)
      REQUIRES(!mounted_apexes_mutex_) {
    std::lock_guard lock(mounted_apexes_mutex_);
    SetLatestLocked(package, full_path);
  }

  inline void SetLatestLocked(const std::string& package,
                              const std::string& full_path)
      REQUIRES(mounted_apexes_mutex_) {
    auto it = mounted_apexes_.find(package);
    CHECK(it != mounted_apexes_.end());

    auto& pkg_map = it->second;

    for (auto pkg_it = pkg_map.begin(); pkg_it != pkg_map.end(); ++pkg_it) {
      if (pkg_it->first.full_path == full_path) {
        pkg_it->second = true;
        for (auto reset_it = pkg_map.begin(); reset_it != pkg_map.end();
             ++reset_it) {
          if (reset_it != pkg_it) {
            reset_it->second = false;
          }
        }
        return;
      }
    }

    LOG(FATAL) << "Did not find " << package << " " << full_path;
  }

  template <typename T>
  inline void ForallMountedApexes(const std::string& package, const T& handler,
                                  bool match_temp_mounts = false) const
      REQUIRES(!mounted_apexes_mutex_) {
    std::lock_guard lock(mounted_apexes_mutex_);
    auto it = mounted_apexes_.find(package);
    if (it == mounted_apexes_.end()) {
      return;
    }
    for (auto& pair : it->second) {
      if (pair.first.is_temp_mount == match_temp_mounts) {
        handler(pair.first, pair.second);
      }
    }
  }

  template <typename T>
  inline void ForallMountedApexes(const T& handler,
                                  bool match_temp_mounts = false) const
      REQUIRES(!mounted_apexes_mutex_) {
    std::lock_guard lock(mounted_apexes_mutex_);
    for (const auto& pkg : mounted_apexes_) {
      for (const auto& pair : pkg.second) {
        if (pair.first.is_temp_mount == match_temp_mounts) {
          handler(pkg.first, pair.first, pair.second);
        }
      }
    }
  }

  inline std::optional<MountedApexData> GetLatestMountedApex(
      const std::string& package) REQUIRES(!mounted_apexes_mutex_) {
    std::optional<MountedApexData> ret;
    ForallMountedApexes(package,
                        [&ret](const MountedApexData& data, bool latest) {
                          if (latest) {
                            ret.emplace(data);
                          }
                        });
    return ret;
  }

  void PopulateFromMounts(const std::string& active_apex_dir,
                          const std::string& decompression_dir,
                          const std::string& apex_hash_tree_dir);

  // Resets state of the database. Should only be used in testing.
  inline void Reset() REQUIRES(!mounted_apexes_mutex_) {
    std::lock_guard lock(mounted_apexes_mutex_);
    mounted_apexes_.clear();
  }

 private:
  // A map from package name to mounted apexes.
  // Note: using std::maps to
  //         a) so we do not have to worry about iterator invalidation.
  //         b) do not have to const_cast (over std::set)
  // TODO(b/158467745): This structure (and functions) need to be guarded by
  //   locks.
  std::map<std::string, std::map<MountedApexData, bool>> mounted_apexes_
      GUARDED_BY(mounted_apexes_mutex_);

  // To fix thread safety negative capability warning
  class Mutex : public std::mutex {
   public:
    // for negative capabilities
    const Mutex& operator!() const { return *this; }
  };
  mutable Mutex mounted_apexes_mutex_;

  inline void CheckAtMostOneLatest() REQUIRES(mounted_apexes_mutex_) {
    for (const auto& apex_set : mounted_apexes_) {
      size_t count = 0;
      for (const auto& pair : apex_set.second) {
        if (pair.second) {
          count++;
        }
      }
      CHECK_LE(count, 1u) << apex_set.first;
    }
  }

  inline void CheckUniqueLoopDm() REQUIRES(mounted_apexes_mutex_) {
    std::unordered_set<std::string> loop_devices;
    std::unordered_set<std::string> dm_devices;
    for (const auto& apex_set : mounted_apexes_) {
      for (const auto& pair : apex_set.second) {
        if (pair.first.loop_name != "") {
          CHECK(loop_devices.insert(pair.first.loop_name).second)
              << "Duplicate loop device: " << pair.first.loop_name;
        }
        if (pair.first.device_name != "") {
          CHECK(dm_devices.insert(pair.first.device_name).second)
              << "Duplicate dm device: " << pair.first.device_name;
        }
        if (pair.first.hashtree_loop_name != "") {
          CHECK(loop_devices.insert(pair.first.hashtree_loop_name).second)
              << "Duplicate loop device: " << pair.first.hashtree_loop_name;
        }
      }
    }
  }
};

}  // namespace apex
}  // namespace android

#endif  // ANDROID_APEXD_APEX_DATABASE_H_
