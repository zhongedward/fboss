// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
#include "fboss/platform/config_lib/ConfigLib.h"

#include <folly/FileUtil.h>
#include <folly/logging/xlog.h>

#include "fboss/platform/helpers/PlatformNameLib.h"

#include "fboss/platform/config_lib/GeneratedConfig.h"

DEFINE_string(
    config_file,
    "",
    "Optional configuration file. "
    "If empty we pick the platform default config");

using namespace facebook::fboss::platform;

namespace {

std::string getPlatformName(const std::optional<std::string>& platformName) {
  auto platformStr = platformName
      ? *platformName
      : *helpers::PlatformNameLib().getPlatformName();
  std::transform(
      platformStr.begin(), platformStr.end(), platformStr.begin(), ::tolower);
  XLOG(INFO) << "The inferred platform is " << platformStr;
  return platformStr;
}

} // namespace

namespace facebook::fboss::platform {

std::optional<std::string> ConfigLib::getConfigFromFile() const {
  // Use the member variable if provided, otherwise fall back to gflags
  std::string configFile =
      !configFilePath_.empty() ? configFilePath_ : FLAGS_config_file;
  if (!configFile.empty()) {
    XLOG(INFO) << "Using config file: " << configFile;
    std::string configJson;
    if (!folly::readFile(configFile.c_str(), configJson)) {
      throw std::runtime_error("Can not read config file: " + configFile);
    }
    return configJson;
  }
  return std::nullopt;
}

std::string ConfigLib::getSensorServiceConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  try {
    return configs::sensor_service.at(getPlatformName(platformName));
  } catch (const std::out_of_range&) {
    throw std::runtime_error(
        "sensor_service configuration not found for platform: " +
        getPlatformName(platformName));
  }
}

std::string ConfigLib::getFanServiceConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  try {
    return configs::fan_service.at(getPlatformName(platformName));
  } catch (const std::out_of_range&) {
    throw std::runtime_error(
        "fan_service configuration not found for platform: " +
        getPlatformName(platformName));
  }
}

std::string ConfigLib::getWeutilConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  try {
    return configs::weutil.at(getPlatformName(platformName));
  } catch (const std::out_of_range&) {
    throw std::runtime_error(
        "weutil configuration not found for platform: " +
        getPlatformName(platformName));
  }
}

std::string ConfigLib::getFwUtilConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  try {
    return configs::fw_util.at(getPlatformName(platformName));
  } catch (const std::out_of_range&) {
    throw std::runtime_error(
        "fw_util configuration not found for platform: " +
        getPlatformName(platformName));
  }
}

std::string ConfigLib::getPlatformManagerConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  try {
    return configs::platform_manager.at(getPlatformName(platformName));
  } catch (const std::out_of_range&) {
    throw std::runtime_error(
        "platform_manager configuration not found for platform: " +
        getPlatformName(platformName));
  }
}

std::string ConfigLib::getLedManagerConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  try {
    return configs::led_manager.at(getPlatformName(platformName));
  } catch (const std::out_of_range&) {
    throw std::runtime_error(
        "led_manager configuration not found for platform: " +
        getPlatformName(platformName));
  }
}

std::string ConfigLib::getBspTestConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  return configs::bsp_tests.at(getPlatformName(platformName));
}

std::string ConfigLib::getShowtechConfig(
    const std::optional<std::string>& platformName) const {
  if (auto configJson = getConfigFromFile()) {
    return *configJson;
  }
  return configs::showtech.at(getPlatformName(platformName));
}

} // namespace facebook::fboss::platform
