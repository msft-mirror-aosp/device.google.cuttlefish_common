/*
 * Copyright (C) 2017 The Android Open Source Project
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
#pragma once

#include <string>

#include "host/libs/vm_manager/vm_manager.h"

namespace vm_manager {

class LibvirtManager : public VmManager {
 public:
  static const std::string name();
  static bool EnsureInstanceDirExists(const std::string& instance_dir);

  LibvirtManager(const vsoc::CuttlefishConfig* config);
  virtual ~LibvirtManager() = default;

  bool Start() override;
  bool Stop() override;

  bool ValidateHostConfiguration(
      std::vector<std::string>* config_commands) const override;
};

}  // namespace vm_manager
