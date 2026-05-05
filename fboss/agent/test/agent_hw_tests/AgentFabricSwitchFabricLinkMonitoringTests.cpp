// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/test/agent_hw_tests/AgentFabricSwitchFabricLinkMonitoringTests.h"

#include "fboss/agent/AgentFeatures.h"
#include "fboss/agent/test/utils/DsfConfigUtils.h"

namespace facebook::fboss {

void AgentFabricSwitchFabricLinkMonitoringTest::SetUp() {
  AgentHwTest::SetUp();
  if (!IsSkipped()) {
    // Verify we are running on a fabric switch
    ASSERT_TRUE(
        std::any_of(getAsics().begin(), getAsics().end(), [](auto& iter) {
          return iter.second->getSwitchType() == cfg::SwitchType::FABRIC;
        }));
    // Verify we have fabric ports for at least one fabric switch
    // Use getSwitchInfoTable().getSwitchIdsOfType() to get switch IDs that
    // match the scope resolver's mapping (from config), not HwAsicTable (from
    // hardware)
    auto fabricSwitchIds = getSw()->getSwitchInfoTable().getSwitchIdsOfType(
        cfg::SwitchType::FABRIC);
    ASSERT_FALSE(fabricSwitchIds.empty()) << "No fabric switch IDs found";
    bool hasFabricPorts = false;
    for (const auto& switchId : fabricSwitchIds) {
      if (!getAgentEnsemble()->masterLogicalFabricPortIds(switchId).empty()) {
        hasFabricPorts = true;
        break;
      }
    }
    ASSERT_TRUE(hasFabricPorts)
        << "No fabric ports found for any of the fabric switch IDs";
  }
}

void AgentFabricSwitchFabricLinkMonitoringTest::setCmdLineFlagOverrides()
    const {
  AgentHwTest::setCmdLineFlagOverrides();
  FLAGS_hide_fabric_ports = false;
  FLAGS_enable_fabric_link_monitoring = true;
}

} // namespace facebook::fboss
