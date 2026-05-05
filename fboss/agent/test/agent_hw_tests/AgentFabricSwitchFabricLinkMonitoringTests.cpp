// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/test/agent_hw_tests/AgentFabricSwitchFabricLinkMonitoringTests.h"

#include "fboss/agent/AgentFeatures.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/test/utils/DsfConfigUtils.h"

namespace facebook::fboss {

namespace {

// Number of RDSW (VOQ) switches connected to this FDSW (L1 fabric switch)
constexpr int kNumRdswSwitches = 128;

// Number of FDSW (L1 fabric) switches in the same cluster
constexpr int kNumFdswSwitches = 40;
constexpr int kNumSwitchIdsPerFdsw = 4;

// Switch ID ranges - FDSW starts at 0 so the default test switch IDs
// fall within the FDSW range without requiring switch ID overrides.
constexpr int64_t kFdswSwitchIdStart = 0;
constexpr int64_t kRdswSwitchIdStart =
    kFdswSwitchIdStart + kNumFdswSwitches * kNumSwitchIdsPerFdsw;
constexpr int64_t kSdswSwitchIdStart =
    kRdswSwitchIdStart + kNumRdswSwitches * kNumSwitchIdsPerFdsw;

} // namespace

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

void AgentFabricSwitchFabricLinkMonitoringTest::overrideTestEnsembleInitInfo(
    TestEnsembleInitInfo& initInfo) const {
  // Override DSF nodes to ensure the system is detected as a dual stage FDSW.
  // The platform config provides the correct switch IDs (0 and 2), but does
  // not include DSF nodes with fabricLevel=2 needed for isDualStage() to
  // return true during platform init. We add a minimal SDSW dummy node here
  // along with local FDSW nodes with fabricLevel=1 for DUAL_STAGE_L1 detection.
  std::map<int64_t, cfg::DsfNode> dsfNodes;
  dsfNodes[0] =
      utility::makeFabricDsfNode(0, "fdsw001", utility::kL1FabricLevel);
  dsfNodes[2] =
      utility::makeFabricDsfNode(2, "fdsw001", utility::kL1FabricLevel);
  // Dummy SDSW node with fabricLevel=2 to make isDualStage() return true
  dsfNodes[kSdswSwitchIdStart] = utility::makeFabricDsfNode(
      kSdswSwitchIdStart, "sdsw001", utility::kL2FabricLevel);
  initInfo.overrideDsfNodes = std::move(dsfNodes);
}

} // namespace facebook::fboss
