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

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common/libs/fs/shared_fd.h"
#include "common/libs/fs/shared_select.h"
#include "common/libs/strings/str_split.h"
#include "common/libs/utils/environment.h"
#include "common/libs/utils/files.h"
#include "common/libs/utils/subprocess.h"
#include "common/libs/utils/size_utils.h"
#include "common/vsoc/lib/vsoc_memory.h"
#include "common/vsoc/shm/screen_layout.h"
#include "host/commands/launch/boot_image_unpacker.h"
#include "host/commands/launch/launcher_defs.h"
#include "host/commands/launch/pre_launch_initializers.h"
#include "host/commands/launch/vsoc_shared_memory.h"
#include "host/libs/config/cuttlefish_config.h"
#include "host/commands/kernel_log_monitor/kernel_log_server.h"
#include "host/libs/vm_manager/vm_manager.h"
#include "host/libs/vm_manager/libvirt_manager.h"
#include "host/libs/vm_manager/qemu_manager.h"

using vsoc::GetPerInstanceDefault;
using cvd::LauncherExitCodes;

DEFINE_string(
    system_image, "",
    "Path to the system image, if empty it is assumed to be a file named "
    "system.img in the directory specified by -system_image_dir");
DEFINE_string(cache_image, "", "Location of the cache partition image.");
DEFINE_int32(cpus, 2, "Virtual CPU count.");
DEFINE_string(data_image, "", "Location of the data partition image.");
DEFINE_string(data_policy, "use_existing", "How to handle userdata partition."
            " Either 'use_existing', 'create_if_missing', or 'always_create'.");
DEFINE_int32(blank_data_image_mb, 0,
             "The size of the blank data image to generate, MB.");
DEFINE_string(blank_data_image_fmt, "ext4",
              "The fs format for the blank data image. Used with mkfs.");
DEFINE_string(qemu_gdb, "",
	      "Debug flag to pass to qemu. e.g. --qemu_gdb=tcp::1234");

DEFINE_int32(x_res, 720, "Width of the screen in pixels");
DEFINE_int32(y_res, 1280, "Height of the screen in pixels");
DEFINE_int32(dpi, 160, "Pixels per inch for the screen");
DEFINE_int32(refresh_rate_hz, 60, "Screen refresh rate in Hertz");
DEFINE_int32(num_screen_buffers, 3, "The number of screen buffers");

DEFINE_bool(disable_app_armor_security, false,
            "Disable AppArmor security in libvirt. For debug only.");
DEFINE_bool(disable_dac_security, false,
            "Disable DAC security in libvirt. For debug only.");
DEFINE_string(kernel_path, "",
              "Path to the kernel. Overrides the one from the boot image");
DEFINE_string(extra_kernel_cmdline, "",
              "Additional flags to put on the kernel command line");
DEFINE_int32(loop_max_part, 7, "Maximum number of loop partitions");
DEFINE_string(console, "ttyS0", "Console device for the guest kernel.");
DEFINE_string(androidboot_console, "ttyS1",
              "Console device for the Android framework");
DEFINE_string(hardware_name, "vsoc",
              "The codename of the device's hardware");
DEFINE_string(guest_security, "selinux",
              "The security module to use in the guest");
DEFINE_bool(guest_enforce_security, false,
            "Whether to run in enforcing mode (non permissive). Ignored if "
            "-guest_security is empty.");
DEFINE_bool(guest_audit_security, true,
            "Whether to log security audits.");
DEFINE_string(boot_image, "", "Location of cuttlefish boot image.");
DEFINE_int32(memory_mb, 2048,
             "Total amount of memory available for guest, MB.");
std::string g_default_mempath{vsoc::GetDefaultMempath()};
DEFINE_string(mempath, g_default_mempath.c_str(),
              "Target location for the shmem file.");
DEFINE_string(mobile_interface, "", // default handled on ParseCommandLine
              "Network interface to use for mobile networking");
DEFINE_string(mobile_tap_name, "", // default handled on ParseCommandLine
              "The name of the tap interface to use for mobile");
std::string g_default_serial_number{GetPerInstanceDefault("CUTTLEFISHCVD")};
DEFINE_string(serial_number, g_default_serial_number.c_str(),
              "Serial number to use for the device");
DEFINE_string(instance_dir, "", // default handled on ParseCommandLine
              "A directory to put all instance specific files");
DEFINE_string(
    vm_manager,
    vsoc::HostSupportsQemuCli() ? vm_manager::QemuManager::name()
                                : vm_manager::LibvirtManager::name(),
    "What virtual machine manager to use, one of libvirt or qemu_cli");
DEFINE_string(system_image_dir, vsoc::DefaultGuestImagePath(""),
              "Location of the system partition images.");
DEFINE_string(vendor_image, "", "Location of the vendor partition image.");

DEFINE_bool(deprecated_boot_completed, false, "Log boot completed message to"
            " host kernel. This is only used during transition of our clients."
            " Will be deprecated soon.");
DEFINE_bool(start_vnc_server, true, "Whether to start the vnc server process.");
DEFINE_string(vnc_server_binary,
              vsoc::DefaultHostArtifactsPath("bin/vnc_server"),
              "Location of the vnc server binary.");
DEFINE_string(virtual_usb_manager_binary,
              vsoc::DefaultHostArtifactsPath("bin/virtual_usb_manager"),
              "Location of the virtual usb manager binary.");
DEFINE_string(kernel_log_monitor_binary,
              vsoc::DefaultHostArtifactsPath("bin/kernel_log_monitor"),
              "Location of the log monitor binary.");
DEFINE_string(ivserver_binary,
              vsoc::DefaultHostArtifactsPath("bin/ivserver"),
              "Location of the ivshmem server binary.");
DEFINE_int32(vnc_server_port, GetPerInstanceDefault(6444),
             "The port on which the vnc server should listen");
DEFINE_string(socket_forward_proxy_binary,
              vsoc::DefaultHostArtifactsPath("bin/socket_forward_proxy"),
              "Location of the socket_forward_proxy binary.");
DEFINE_string(adb_mode, "tunnel",
              "Mode for adb connection. Can be 'usb' for usb forwarding, "
              "'tunnel' for tcp connection, or a comma separated list of types "
              "as in 'usb,tunnel'");
DEFINE_bool(run_adb_connector, true,
            "Maintain adb connection by sending 'adb connect' commands to the "
            "server. Only relevant with --adb_mode=tunnel");
DEFINE_string(adb_connector_binary,
              vsoc::DefaultHostArtifactsPath("bin/adb_connector"),
              "Location of the adb_connector binary. Only relevant if "
              "--run_adb_connector is true");
DEFINE_int32(vhci_port, GetPerInstanceDefault(0), "VHCI port to use for usb");
DEFINE_string(guest_mac_address,
              GetPerInstanceDefault("00:43:56:44:80:"), // 00:43:56:44:80:0x
              "MAC address of the wifi interface to be created on the guest.");
DEFINE_string(host_mac_address,
              "42:00:00:00:00:00",
              "MAC address of the wifi interface running on the host.");
DEFINE_string(wifi_interface, "", // default handled on ParseCommandLine
              "Network interface to use for wifi");
DEFINE_string(wifi_tap_name, "", // default handled on ParseCommandLine
              "The name of the tap interface to use for wifi");
// TODO(b/72969289) This should be generated
DEFINE_string(dtb, "", "Path to the cuttlefish.dtb file");

DEFINE_string(uuid, vsoc::GetPerInstanceDefault(vsoc::kDefaultUuidPrefix),
              "UUID to use for the device. Random if not specified");
DEFINE_bool(daemon, false,
            "Run cuttlefish in background, the launcher exits on boot "
            "completed/failed");

DEFINE_string(device_title, "", "Human readable name for the instance, "
              "used by the vnc_server for its server title");
DEFINE_string(setupwizard_mode, "DISABLED",
	      "One of DISABLED,OPTIONAL,REQUIRED");

DECLARE_string(config_file);

namespace {
const std::string kDataPolicyUseExisting = "use_existing";
const std::string kDataPolicyCreateIfMissing = "create_if_missing";
const std::string kDataPolicyAlwaysCreate = "always_create";

constexpr char kAdbModeTunnel[] = "tunnel";
constexpr char kAdbModeUsb[] = "usb";

void CreateBlankImage(
    const std::string& image, int image_mb, const std::string& image_fmt) {
  LOG(INFO) << "Creating " << image;
  std::string of = "of=";
  of += image;
  std::string count = "count=";
  count += std::to_string(image_mb);
  cvd::execute({"/bin/dd", "if=/dev/zero", of, "bs=1M", count});
  cvd::execute({"/sbin/mkfs", "-t", image_fmt, image}, {"PATH=/sbin"});
}

void RemoveFile(const std::string& file) {
  LOG(INFO) << "Removing " << file;
  cvd::execute({"/bin/rm", "-f", file});
}

bool ApplyDataImagePolicy(const char* data_image) {
  bool data_exists = cvd::FileHasContent(data_image);
  bool remove{};
  bool create{};

  if (FLAGS_data_policy == kDataPolicyUseExisting) {
    if (!data_exists) {
      LOG(ERROR) << "Specified data image file does not exists: " << data_image;
      return false;
    }
    if (FLAGS_blank_data_image_mb > 0) {
      LOG(ERROR) << "You should NOT use -blank_data_image_mb with -data_policy="
                 << kDataPolicyUseExisting;
      return false;
    }
    create = false;
    remove = false;
  } else if (FLAGS_data_policy == kDataPolicyAlwaysCreate) {
    remove = data_exists;
    create = true;
  } else if (FLAGS_data_policy == kDataPolicyCreateIfMissing) {
    create = !data_exists;
    remove = false;
  } else {
    LOG(ERROR) << "Invalid data_policy: " << FLAGS_data_policy;
    return false;
  }

  if (remove) {
    RemoveFile(data_image);
  }

  if (create) {
    if (FLAGS_blank_data_image_mb <= 0) {
      LOG(ERROR) << "-blank_data_image_mb is required to create data image";
      return false;
    }
    CreateBlankImage(
        data_image, FLAGS_blank_data_image_mb, FLAGS_blank_data_image_fmt);
  } else {
    LOG(INFO) << data_image << " exists. Not creating it.";
  }

  return true;
}

std::string GetConfigFile() {
  return vsoc::CuttlefishConfig::Get()->PerInstancePath(
      "cuttlefish_config.json");
}

std::string GetConfigFileArg() { return "-config_file=" + GetConfigFile(); }

std::string GetGuestPortArg() {
  constexpr int kEmulatorPort = 5555;
  return std::string{"--guest_ports="} + std::to_string(kEmulatorPort);
}

int GetHostPort() {
  constexpr int kFirstHostPort = 6520;
  return vsoc::GetPerInstanceDefault(kFirstHostPort);
}

std::string GetHostPortArg() {
  return std::string{"--host_ports="} + std::to_string(GetHostPort());
}

std::string GetAdbConnectorPortArg() {
  return std::string{"--ports="} + std::to_string(GetHostPort());
}

bool AdbModeEnabled(const char* mode) {
  auto modes = cvd::StrSplit(FLAGS_adb_mode, ',');
  return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

bool AdbTunnelEnabled() {
  return AdbModeEnabled(kAdbModeTunnel);
}

bool AdbUsbEnabled() {
  return AdbModeEnabled(kAdbModeUsb);
}

void ValidateAdbModeFlag() {
  if (!AdbUsbEnabled() && !AdbTunnelEnabled()) {
    LOG(INFO) << "ADB not enabled";
  }
}

int CreateIvServerUnixSocket(const std::string& path) {
  return cvd::SharedFD::SocketLocalServer(path.c_str(), false, SOCK_STREAM,
                                          0666)->UNMANAGED_Dup();
}

bool AdbConnectorEnabled() {
  return FLAGS_run_adb_connector && AdbTunnelEnabled();
}

void LaunchUsbServerIfEnabled(vsoc::CuttlefishConfig* config) {
  if (!AdbUsbEnabled()) {
    return;
  }
  auto socket_name = config->usb_v1_socket_name();
  auto usb_v1_server = cvd::SharedFD::SocketLocalServer(
      socket_name.c_str(), false, SOCK_STREAM, 0666);
  if (!usb_v1_server->IsOpen()) {
    LOG(ERROR) << "Unable to create USB v1 server socket: "
               << usb_v1_server->StrError();
    std::exit(cvd::LauncherExitCodes::kUsbV1SocketError);
  }
  int server_fd = usb_v1_server->UNMANAGED_Dup();
  if (server_fd < 0) {
    LOG(ERROR) << "Unable to dup USB v1 server socket file descriptor: "
               << strerror(errno);
    std::exit(cvd::LauncherExitCodes::kUsbV1SocketError);
  }

  cvd::subprocess({FLAGS_virtual_usb_manager_binary,
                   "-usb_v1_fd=" + std::to_string(server_fd),
                   GetConfigFileArg()});

  close(server_fd);
}

void LaunchKernelLogMonitor(vsoc::CuttlefishConfig* config,
                            cvd::SharedFD boot_events_pipe) {
  auto log_name = config->kernel_log_socket_name();
  auto server = cvd::SharedFD::SocketLocalServer(log_name.c_str(), false,
                                                 SOCK_STREAM, 0666);
  int server_fd = server->UNMANAGED_Dup();
  int subscriber_fd = -1;
  if (boot_events_pipe->IsOpen()) {
    subscriber_fd = boot_events_pipe->UNMANAGED_Dup();
  }
  cvd::subprocess({FLAGS_kernel_log_monitor_binary,
                   "-log_server_fd=" + std::to_string(server_fd),
                   "-subscriber_fd=" + std::to_string(subscriber_fd),
                   GetConfigFileArg()});
  close(server_fd);
  if (subscriber_fd >= 0) {
    close(subscriber_fd);
  }
}

void LaunchIvServer(vsoc::CuttlefishConfig* config) {
  // Resize gralloc region
  auto actual_width = cvd::AlignToPowerOf2(FLAGS_x_res * 4, 4);  // align to 16
  uint32_t screen_buffers_size =
      FLAGS_num_screen_buffers *
      cvd::AlignToPageSize(actual_width * FLAGS_y_res + 16 /* padding */);
  screen_buffers_size +=
      (FLAGS_num_screen_buffers - 1) * 4096; /* Guard pages */

  // TODO(b/79170615) Resize gralloc region too.

  vsoc::CreateSharedMemoryFile(
      config->mempath(),
      {{vsoc::layout::screen::ScreenLayout::region_name, screen_buffers_size}});

  auto qemu_channel =
      CreateIvServerUnixSocket(config->ivshmem_qemu_socket_path());
  auto client_channel =
      CreateIvServerUnixSocket(config->ivshmem_client_socket_path());
  auto qemu_socket_arg = "-qemu_socket_fd=" + std::to_string(qemu_channel);
  auto client_socket_arg =
      "-client_socket_fd=" + std::to_string(client_channel);
  cvd::subprocess({FLAGS_ivserver_binary, qemu_socket_arg, client_socket_arg,
                   GetConfigFileArg()});
  close(qemu_channel);
  close(client_channel);
}

void LaunchAdbConnectorIfEnabled() {
  if (AdbConnectorEnabled()) {
    cvd::subprocess({FLAGS_adb_connector_binary,
                     GetAdbConnectorPortArg()});
  }
}

void LaunchSocketForwardProxyIfEnabled() {
  if (AdbTunnelEnabled()) {
    cvd::subprocess({FLAGS_socket_forward_proxy_binary,
                     GetGuestPortArg(),
                     GetHostPortArg(),
                     GetConfigFileArg()});
  }
}

void LaunchVNCServerIfEnabled() {
  if (FLAGS_start_vnc_server) {
    // Launch the vnc server, don't wait for it to complete
    auto port_options = "-port=" + std::to_string(FLAGS_vnc_server_port);
    cvd::subprocess(
        {FLAGS_vnc_server_binary, port_options, GetConfigFileArg()});
  }
}

bool ResolveInstanceFiles() {
  if (FLAGS_system_image_dir.empty()) {
    LOG(ERROR) << "--system_image_dir must be specified.";
    return false;
  }

  // If user did not specify location of either of these files, expect them to
  // be placed in --system_image_dir location.
  if (FLAGS_system_image.empty()) {
    FLAGS_system_image = FLAGS_system_image_dir + "/system.img";
  }
  if (FLAGS_boot_image.empty()) {
    FLAGS_boot_image = FLAGS_system_image_dir + "/boot.img";
  }
  if (FLAGS_cache_image.empty()) {
    FLAGS_cache_image = FLAGS_system_image_dir + "/cache.img";
  }
  if (FLAGS_data_image.empty()) {
    FLAGS_data_image = FLAGS_system_image_dir + "/userdata.img";
  }
  if (FLAGS_vendor_image.empty()) {
    FLAGS_vendor_image = FLAGS_system_image_dir + "/vendor.img";
  }

  // Create data if necessary
  if (!ApplyDataImagePolicy(FLAGS_data_image.c_str())) {
    return false;
  }

  // Check that the files exist
  for (const auto& file :
       {FLAGS_system_image, FLAGS_vendor_image, FLAGS_cache_image,
        FLAGS_data_image, FLAGS_boot_image}) {
    if (!cvd::FileHasContent(file.c_str())) {
      LOG(ERROR) << "File not found: " << file;
      return false;
    }
  }
  return true;
}

bool UnpackBootImage(const cvd::BootImageUnpacker& boot_image_unpacker,
                     vsoc::CuttlefishConfig* config) {
  if (boot_image_unpacker.HasRamdiskImage()) {
    if (!boot_image_unpacker.ExtractRamdiskImage(
            config->ramdisk_image_path())) {
      LOG(ERROR) << "Error extracting ramdisk from boot image";
      return false;
    }
  }
  if (!FLAGS_kernel_path.size()) {
    if (boot_image_unpacker.HasKernelImage()) {
      if (!boot_image_unpacker.ExtractKernelImage(
              config->kernel_image_path())) {
        LOG(ERROR) << "Error extracting kernel from boot image";
        return false;
      }
    } else {
      LOG(ERROR) << "No kernel found on boot image";
      return false;
    }
  }
  return true;
}

template<typename S, typename T>
static std::string concat(const S& s, const T& t) {
  std::ostringstream os;
  os << s << t;
  return os.str();
}

vsoc::CuttlefishConfig* InitializeCuttlefishConfiguration(
    const cvd::BootImageUnpacker& boot_image_unpacker) {
  auto& memory_layout = *vsoc::VSoCMemoryLayout::Get();
  auto config = vsoc::CuttlefishConfig::Get();
  if (!config) {
    LOG(ERROR) << "Failed to instantiate config object. Most likely because "
                  "config file was specified and doesn't exits: '"
               << FLAGS_config_file << "'";
    return nullptr;
  }
  // Set this first so that calls to PerInstancePath below are correct
  config->set_instance_dir(FLAGS_instance_dir);
  if (!vm_manager::VmManager::IsValidName(FLAGS_vm_manager)) {
    LOG(ERROR) << "Invalid vm_manager: " << FLAGS_vm_manager;
    return nullptr;
  }
  config->set_vm_manager(FLAGS_vm_manager);

  config->set_serial_number(FLAGS_serial_number);

  config->set_cpus(FLAGS_cpus);
  config->set_memory_mb(FLAGS_memory_mb);

  config->set_dpi(FLAGS_dpi);
  config->set_setupwizard_mode(FLAGS_setupwizard_mode);
  config->set_x_res(FLAGS_x_res);
  config->set_y_res(FLAGS_y_res);
  config->set_refresh_rate_hz(FLAGS_refresh_rate_hz);
  config->set_gdb_flag(FLAGS_qemu_gdb);
  config->set_adb_mode(FLAGS_adb_mode);
  config->set_device_title(FLAGS_device_title);
  if (FLAGS_kernel_path.size()) {
    config->set_kernel_image_path(FLAGS_kernel_path);
  } else {
    config->set_kernel_image_path(config->PerInstancePath("kernel"));
  }

  auto ramdisk_path = config->PerInstancePath("ramdisk.img");
  bool use_ramdisk = boot_image_unpacker.HasRamdiskImage();
  if (!use_ramdisk) {
    LOG(INFO) << "No ramdisk present; assuming system-as-root build";
    ramdisk_path = "";
  }

  // This needs to be done here because the dtb path depends on the presence of
  // the ramdisk
  if (FLAGS_dtb.empty()) {
    if (use_ramdisk) {
      FLAGS_dtb = vsoc::DefaultHostArtifactsPath("config/initrd-root.dtb");
    } else {
      FLAGS_dtb = vsoc::DefaultHostArtifactsPath("config/system-root.dtb");
    }
  }

  config->add_kernel_cmdline(boot_image_unpacker.kernel_cmdline());
  if (!use_ramdisk) {
    config->add_kernel_cmdline("root=/dev/vda init=/init");
  }
  config->add_kernel_cmdline(
      concat("androidboot.serialno=", FLAGS_serial_number));
  config->add_kernel_cmdline("mac80211_hwsim.radios=0");
  config->add_kernel_cmdline(concat("androidboot.lcd_density=", FLAGS_dpi));
  config->add_kernel_cmdline(concat("androidboot.setupwizard_mode=",
				    FLAGS_setupwizard_mode));
  config->add_kernel_cmdline(concat("loop.max_part=", FLAGS_loop_max_part));
  if (!FLAGS_console.empty()) {
    config->add_kernel_cmdline(concat("console=", FLAGS_console));
  }
  if (!FLAGS_androidboot_console.empty()) {
    config->add_kernel_cmdline(
        concat("androidboot.console=", FLAGS_androidboot_console));
  }
  if (!FLAGS_hardware_name.empty()) {
    config->add_kernel_cmdline(
        concat("androidboot.hardware=", FLAGS_hardware_name));
  }
  if (!FLAGS_guest_security.empty()) {
    config->add_kernel_cmdline(concat("security=", FLAGS_guest_security));
    if (FLAGS_guest_enforce_security) {
      config->add_kernel_cmdline("enforcing=1");
    } else {
      config->add_kernel_cmdline("enforcing=0");
      config->add_kernel_cmdline("androidboot.selinux=permissive");
    }
    if (FLAGS_guest_audit_security) {
      config->add_kernel_cmdline("audit=1");
    } else {
      config->add_kernel_cmdline("audit=0");
    }
  }
  if (FLAGS_extra_kernel_cmdline.size()) {
    config->add_kernel_cmdline(FLAGS_extra_kernel_cmdline);
  }

  config->set_ramdisk_image_path(ramdisk_path);
  config->set_system_image_path(FLAGS_system_image);
  config->set_cache_image_path(FLAGS_cache_image);
  config->set_data_image_path(FLAGS_data_image);
  config->set_vendor_image_path(FLAGS_vendor_image);
  config->set_dtb_path(FLAGS_dtb);

  config->set_mempath(FLAGS_mempath);
  config->set_ivshmem_qemu_socket_path(
      config->PerInstancePath("ivshmem_socket_qemu"));
  config->set_ivshmem_client_socket_path(
      config->PerInstancePath("ivshmem_socket_client"));
  config->set_ivshmem_vector_count(memory_layout.GetRegions().size());

  if (AdbUsbEnabled()) {
    config->set_usb_v1_socket_name(config->PerInstancePath("usb-v1"));
    config->set_vhci_port(FLAGS_vhci_port);
    config->set_usb_ip_socket_name(config->PerInstancePath("usb-ip"));
  }

  config->set_kernel_log_socket_name(config->PerInstancePath("kernel-log"));
  config->set_deprecated_boot_completed(FLAGS_deprecated_boot_completed);
  config->set_console_path(config->PerInstancePath("console"));
  config->set_logcat_path(config->PerInstancePath("logcat"));
  config->set_launcher_log_path(config->PerInstancePath("launcher.log"));
  config->set_launcher_monitor_socket_path(
      config->PerInstancePath("launcher_monitor.sock"));

  config->set_mobile_bridge_name(FLAGS_mobile_interface);
  config->set_mobile_tap_name(FLAGS_mobile_tap_name);

  config->set_wifi_bridge_name(FLAGS_wifi_interface);
  config->set_wifi_tap_name(FLAGS_wifi_tap_name);

  config->set_wifi_guest_mac_addr(FLAGS_guest_mac_address);
  config->set_wifi_host_mac_addr(FLAGS_host_mac_address);

  config->set_entropy_source("/dev/urandom");
  config->set_uuid(FLAGS_uuid);

  config->set_disable_dac_security(FLAGS_disable_dac_security);
  config->set_disable_app_armor_security(FLAGS_disable_app_armor_security);

  if(!AdbUsbEnabled()) {
    config->disable_usb_adb();
  }

  config->set_cuttlefish_env_path(cvd::StringFromEnv("HOME", ".") +
                                  "/.cuttlefish.sh");

  return config;
}

void SetDefaultFlagsForQemu() {
  auto default_mobile_interface = GetPerInstanceDefault("cvd-mbr-");
  SetCommandLineOptionWithMode("mobile_interface",
                               default_mobile_interface.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_mobile_tap_name = GetPerInstanceDefault("cvd-mtap-");
  SetCommandLineOptionWithMode("mobile_tap_name",
                               default_mobile_tap_name.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_wifi_interface = GetPerInstanceDefault("cvd-wbr-");
  SetCommandLineOptionWithMode("wifi_interface",
                               default_wifi_interface.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_wifi_tap_name = GetPerInstanceDefault("cvd-wtap-");
  SetCommandLineOptionWithMode("wifi_tap_name",
                               default_wifi_tap_name.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_instance_dir =
      cvd::StringFromEnv("HOME", ".") + "/cuttlefish_runtime";
  SetCommandLineOptionWithMode("instance_dir",
                               default_instance_dir.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
}

void SetDefaultFlagsForLibvirt() {
  auto default_mobile_interface = GetPerInstanceDefault("cvd-mobile-");
  SetCommandLineOptionWithMode("mobile_interface",
                               default_mobile_interface.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_mobile_tap_name = GetPerInstanceDefault("amobile");
  SetCommandLineOptionWithMode("mobile_tap_name",
                               default_mobile_tap_name.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_wifi_interface = GetPerInstanceDefault("cvd-wifi-");
  SetCommandLineOptionWithMode("wifi_interface",
                               default_wifi_interface.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_wifi_tap_name = GetPerInstanceDefault("awifi");
  SetCommandLineOptionWithMode("wifi_tap_name",
                               default_wifi_tap_name.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
  auto default_instance_dir =
      "/var/run/libvirt-" +
      vsoc::GetPerInstanceDefault(vsoc::kDefaultUuidPrefix);
  SetCommandLineOptionWithMode("instance_dir",
                               default_instance_dir.c_str(),
                               google::FlagSettingMode::SET_FLAGS_DEFAULT);
}

bool ParseCommandLineFlags(int* argc, char*** argv) {
  // The config_file is created by the launcher, so the launcher is the only
  // host process that doesn't use the flag.
  // Set the default to empty.
  google::SetCommandLineOptionWithMode("config_file", "",
                                       gflags::SET_FLAGS_DEFAULT);
  google::ParseCommandLineNonHelpFlags(argc, argv, true);
  bool invalid_manager = false;
  if (FLAGS_vm_manager == vm_manager::LibvirtManager::name()) {
    SetDefaultFlagsForLibvirt();
  } else if (FLAGS_vm_manager == vm_manager::QemuManager::name()) {
    SetDefaultFlagsForQemu();
  } else {
    std::cerr << "Unknown Virtual Machine Manager: " << FLAGS_vm_manager
              << std::endl;
    invalid_manager = true;
  }
  google::HandleCommandLineHelpFlags();
  if (invalid_manager) {
    return false;
  }
  // Set the flag value to empty (in case the caller passed a value for it).
  FLAGS_config_file = "";

  ValidateAdbModeFlag();

  return ResolveInstanceFiles();
}

bool WriteCuttlefishEnvironment(vsoc::CuttlefishConfig* config) {
  auto env = cvd::SharedFD::Open(config->cuttlefish_env_path().c_str(),
                                 O_CREAT | O_RDWR, 0755);
  if (!env->IsOpen()) {
    LOG(ERROR) << "Unable to create cuttlefish.env file";
    return false;
  }
  std::string config_env = "export CUTTLEFISH_PER_INSTANCE_PATH=\"" +
                           config->PerInstancePath(".") + "\"\n";
  config_env += "export ANDROID_SERIAL=";
  if (AdbUsbEnabled()) {
    config_env += config->serial_number();
  } else {
    config_env += "127.0.0.1:" + std::to_string(GetHostPort());
  }
  config_env += "\n";
  env->Write(config_env.c_str(), config_env.size());
  return true;
}

// Forks and returns the write end of a pipe to the child process. The parent
// process waits for boot events to come through the pipe and exits accordingly.
cvd::SharedFD DaemonizeLauncher(vsoc::CuttlefishConfig* config) {
  cvd::SharedFD read_end, write_end;
  if (!cvd::SharedFD::Pipe(&read_end, &write_end)) {
    LOG(ERROR) << "Unable to create pipe";
    return cvd::SharedFD(); // a closed FD
  }
  auto pid = fork();
  if (pid) {
    // Explicitly close here, otherwise we may end up reading forever if the
    // child process dies.
    write_end->Close();
    monitor::BootEvent evt;
    while(true) {
      auto bytes_read = read_end->Read(&evt, sizeof(evt));
      if (bytes_read != sizeof(evt)) {
        LOG(ERROR) << "Fail to read a complete event, read " << bytes_read
                   << " bytes only instead of the expected " << sizeof(evt);
        std::exit(LauncherExitCodes::kPipeIOError);
      }
      if (evt == monitor::BootEvent::BootCompleted) {
        LOG(INFO) << "Virtual device booted successfully";
        std::exit(LauncherExitCodes::kSuccess);
      }
      if (evt == monitor::BootEvent::BootFailed) {
        LOG(ERROR) << "Virtual device failed to boot";
        std::exit(LauncherExitCodes::kVirtualDeviceBootFailed);
      }
      // Do nothing for the other signals
    }
  } else {
    // The child returns the write end of the pipe
    if (daemon(/*nochdir*/ 1, /*noclose*/ 1) != 0) {
      LOG(ERROR) << "Failed to daemonize child process: " << strerror(errno);
      std::exit(LauncherExitCodes::kDaemonizationError);
    }
    // Redirect standard I/O
    auto log_path = config->launcher_log_path();
    auto log =
        cvd::SharedFD::Open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC,
                            S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (!log->IsOpen()) {
      LOG(ERROR) << "Failed to create launcher log file: " << log->StrError();
      std::exit(LauncherExitCodes::kDaemonizationError);
    }
    auto dev_null = cvd::SharedFD::Open("/dev/null", O_RDONLY);
    if (!dev_null->IsOpen()) {
      LOG(ERROR) << "Failed to open /dev/null: " << dev_null->StrError();
      std::exit(LauncherExitCodes::kDaemonizationError);
    }
    if (dev_null->UNMANAGED_Dup2(0) < 0) {
      LOG(ERROR) << "Failed dup2 stdin: " << dev_null->StrError();
      std::exit(LauncherExitCodes::kDaemonizationError);
    }
    if (log->UNMANAGED_Dup2(1) < 0) {
      LOG(ERROR) << "Failed dup2 stdout: " << log->StrError();
      std::exit(LauncherExitCodes::kDaemonizationError);
    }
    if (log->UNMANAGED_Dup2(2) < 0) {
      LOG(ERROR) << "Failed dup2 seterr: " << log->StrError();
      std::exit(LauncherExitCodes::kDaemonizationError);
    }

    read_end->Close();
    return write_end;
  }
}

// Stops the device. If this function is successful it returns on a child of the
// launcher (after it killed the laucher) and it should exit immediately
bool StopCvd(vm_manager::VmManager* vm_manager) {
  vm_manager->Stop();
  auto pgid = getpgid(0);
  auto child_pid = fork();
  if (child_pid > 0) {
    // The parent just waits for the child to kill it.
    int wstatus;
    waitpid(child_pid, &wstatus, 0);
    LOG(ERROR) << "The forked child exited before delivering signal with "
                  "status: "
               << wstatus;
    // If waitpid returns it means the child exited before the signal was
    // delivered, notify the client of the error and continue serving
    return false;
  } else if (child_pid == 0) {
    // The child makes sure it is in a different process group before
    // killing everyone on its parent's
    // This call should never fail (see SETPGID(2))
    if (setpgid(0, 0) != 0) {
      LOG(ERROR) << "setpgid failed (" << strerror(errno)
                 << ") the launcher's child is about to kill itself";
    }
    killpg(pgid, SIGKILL);
    return true;
  } else {
    // The fork failed, the system is in pretty bad shape
    LOG(FATAL) << "Unable to fork before on Stop: " << strerror(errno);
    return false;
  }
}

void ServerLoop(cvd::SharedFD server,
                vm_manager::VmManager* vm_manager) {
  while (true) {
    // TODO: use select to handle simultaneous connections.
    auto client = cvd::SharedFD::Accept(*server);
    cvd::LauncherAction action;
    while (client->IsOpen() && client->Read(&action, sizeof(action)) > 0) {
      switch (action) {
        case cvd::LauncherAction::kStop:
          if (StopCvd(vm_manager)) {
            auto response = cvd::LauncherResponse::kSuccess;
            client->Write(&response, sizeof(response));
            std::exit(0);
          } else {
            auto response = cvd::LauncherResponse::kError;
            client->Write(&response, sizeof(response));
          }
          break;
        default:
          LOG(ERROR) << "Unrecognized launcher action: "
                     << static_cast<char>(action);
          auto response = cvd::LauncherResponse::kError;
          client->Write(&response, sizeof(response));
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  ::android::base::InitLogging(argv, android::base::StderrLogger);
  if (!ParseCommandLineFlags(&argc, &argv)) {
    return LauncherExitCodes::kArgumentParsingError;
  }

  auto boot_img_unpacker = cvd::BootImageUnpacker::FromImage(FLAGS_boot_image);
  // Do this early so that the config object is ready for anything that needs it
  auto config = InitializeCuttlefishConfiguration(*boot_img_unpacker);
  if (!config) {
    return LauncherExitCodes::kCuttlefishConfigurationInitError;
  }

  auto vm_manager = vm_manager::VmManager::Get(config->vm_manager(), config);

  // Check host configuration
  std::vector<std::string> config_commands;
  if (!vm_manager->ValidateHostConfiguration(&config_commands)) {
    LOG(ERROR) << "Validation of user configuration failed";
    std::cout << "Execute the following to correctly configure:" << std::endl;
    for (auto& command : config_commands) {
      std::cout << "  " << command << std::endl;
    }
    std::cout << "You may need to logout for the changes to take effect"
              << std::endl;
    return LauncherExitCodes::kInvalidHostConfiguration;
  }

  if (!vm_manager->EnsureInstanceDirExists()) {
    LOG(ERROR) << "Failed to create instance directory: " << FLAGS_instance_dir;
    return LauncherExitCodes::kInstanceDirCreationError;
  }

  if (!vm_manager->CleanPriorFiles()) {
    LOG(ERROR) << "Failed to clean prior files";
    return LauncherExitCodes::kPrioFilesCleanupError;
  }

  if (!UnpackBootImage(*boot_img_unpacker, config)) {
    LOG(ERROR) << "Failed to unpack boot image";
    return LauncherExitCodes::kBootImageUnpackError;
  }

  if (!WriteCuttlefishEnvironment(config)) {
    LOG(ERROR) << "Unable to write cuttlefish environment file";
  }

  auto config_file = GetConfigFile();
  auto config_link = vsoc::GetGlobalConfigFileLink();
  // Save the config object before starting any host process
  if (!config->SaveToFile(config_file)) {
    return LauncherExitCodes::kCuttlefishConfigurationSaveError;
  }
  if (symlink(config_file.c_str(), config_link.c_str()) != 0) {
    LOG(ERROR) << "Failed to create symlink to config file at " << config_link
               << ": " << strerror(errno);
    return LauncherExitCodes::kCuttlefishConfigurationSaveError;
  }

  LOG(INFO) << "The following files contain useful debugging information:";
  if (FLAGS_daemon) {
    LOG(INFO) << "  Launcher log: " << config->launcher_log_path();
  }
  LOG(INFO) << "  Android's logcat output: " << config->logcat_path();
  LOG(INFO) << "  Kernel log: " << config->PerInstancePath("kernel.log");
  LOG(INFO) << "  Instance configuration: " << GetConfigFile();
  LOG(INFO) << "  Instance environment: " << config->cuttlefish_env_path();
  LOG(INFO) << "To access the console run: socat file:$(tty),raw,echo=0 "
            << config->console_path();

  auto launcher_monitor_path = config->launcher_monitor_socket_path();
  auto launcher_monitor_socket = cvd::SharedFD::SocketLocalServer(
      launcher_monitor_path.c_str(), false, SOCK_STREAM, 0666);
  if (!launcher_monitor_socket->IsOpen()) {
    LOG(ERROR) << "Error when opening launcher server: "
               << launcher_monitor_socket->StrError();
    return cvd::LauncherExitCodes::kMonitorCreationFailed;
  }
  cvd::SharedFD boot_events_pipe;
  if (FLAGS_daemon) {
    boot_events_pipe = DaemonizeLauncher(config);
    if (!boot_events_pipe->IsOpen()) {
      return LauncherExitCodes::kDaemonizationError;
    }
  } else {
    // Make sure the launcher runs in its own process group even when running in
    // foreground
    if (getsid(0) != getpid()) {
      int retval = setpgid(0, 0);
      if (retval) {
        LOG(ERROR) << "Failed to create new process group: " << strerror(errno);
        std::exit(LauncherExitCodes::kProcessGroupError);
      }
    }
  }

  LaunchKernelLogMonitor(config, boot_events_pipe);
  LaunchUsbServerIfEnabled(config);
  LaunchIvServer(config);

  // Initialize the regions that require so before the VM starts.
  PreLaunchInitializers::Initialize(config);

  // Start the guest VM
  if (!vm_manager->Start()) {
    LOG(ERROR) << "Unable to start vm_manager";
    // TODO(111453282): All host processes should die here.
    return LauncherExitCodes::kVMCreationError;
  }

  LaunchSocketForwardProxyIfEnabled();
  LaunchVNCServerIfEnabled();
  LaunchAdbConnectorIfEnabled();

  ServerLoop(launcher_monitor_socket, vm_manager); // Should not return
  LOG(ERROR) << "The server loop returned, it should never happen!!";
  return cvd::LauncherExitCodes::kServerError;
}
