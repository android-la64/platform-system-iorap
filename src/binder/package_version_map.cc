// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "package_version_map.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

namespace iorap::binder {

std::shared_ptr<PackageVersionMap> PackageVersionMap::Create() {
  std::shared_ptr<PackageManagerRemote> package_manager =
      PackageManagerRemote::Create();
  if (!package_manager) {
    return nullptr;
  }

  VersionMap map = package_manager->GetPackageVersionMap();

  return std::make_shared<PackageVersionMap>(package_manager, map);
}

void PackageVersionMap::Update() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t old_size = version_map_.size();
  version_map_ = package_manager_->GetPackageVersionMap();
  LOG(DEBUG) << "Update for version is done. The size is from " << old_size
             << " to " << version_map_.size();
}

size_t PackageVersionMap::Size() { return version_map_.size(); }

int64_t PackageVersionMap::GetOrQueryPackageVersion(const std::string& package_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  VersionMap::iterator it = version_map_.find(package_name);

  if (it == version_map_.end()) {
    LOG(WARNING) << "Cannot find version for: " << package_name
                 << " in the hash table";
    std::optional<int64_t> version =
        package_manager_->GetPackageVersion(package_name);
    if (version) {
      LOG(VERBOSE) << "Find version for: " << package_name << " on the fly.";
      version_map_[package_name] = *version;
      return *version;
    } else {
      LOG(ERROR) << "Cannot find version for: " << package_name
                 << " on the fly.";
      return -1;
    }
  }

  return it->second;
}
}  // namespace iorap::binder
