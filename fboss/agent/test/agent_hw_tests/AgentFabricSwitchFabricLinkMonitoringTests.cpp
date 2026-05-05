// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/test/agent_hw_tests/AgentFabricSwitchFabricLinkMonitoringTests.h"

#include "fboss/agent/AgentFeatures.h"
#include "fboss/agent/Utils.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/test/utils/DsfConfigUtils.h"
#include "fboss/agent/test/utils/FabricTestUtils.h"
#include "fboss/agent/test/utils/PortTestUtils.h"

#include <folly/testing/TestUtil.h>

namespace facebook::fboss {

namespace {

// Number of RDSW (VOQ) switches connected to this FDSW (L1 fabric switch)
// Each RDSW has one switch ID visible from this FDSW
// Based on actual fabric connectivity: 128 RDSW devices
constexpr int kNumRdswSwitches = 128;

// Number of SDSW (L2 fabric) switches connected to this FDSW
// Based on actual fabric connectivity: 128 SDSW devices with 4 ASICs each
constexpr int kNumSdswSwitches = 128;
constexpr int kNumSdswAsicsPerSwitch = 4;

// Number of FDSW (L1 fabric) switches in the same cluster
// Based on actual config: fdsw001-fdsw040 with n001.c083.nao5
constexpr int kNumFdswSwitches = 40;
constexpr int kNumFdswAsicsPerSwitch = 2;
constexpr int kNumSwitchIdsPerFdsw = 4;

// Number of fabric ports per ASIC (RAMON3 has 256 fabric ports per chip)
constexpr int kNumFabricPortsPerAsic = 256;

// Switch ID ranges - FDSW starts at 0 so the default test switch IDs
// fall within the FDSW range without requiring switch ID overrides.
// RDSW and SDSW ranges follow after the FDSW range.
// FDSW: 40 switches * 4 IDs = 160 (0-159)
// RDSW: 128 switches * 4 IDs = 512 (160-671)
// SDSW: 128 switches * 4 IDs = 512 (672-1183)
constexpr int64_t kFdswSwitchIdStart = 0;
constexpr int64_t kRdswSwitchIdStart =
    kFdswSwitchIdStart + kNumFdswSwitches * kNumSwitchIdsPerFdsw;
constexpr int64_t kSdswSwitchIdStart =
    kRdswSwitchIdStart + kNumRdswSwitches * kNumSwitchIdsPerFdsw;

// Get the simplified device name
std::string getRdswName(int rdswIndex) {
  return fmt::format("rdsw{:03d}", rdswIndex);
}

std::string getFdswName(int fdswIndex) {
  return fmt::format("fdsw{:03d}", fdswIndex);
}

std::string getSdswName(int sdswIndex) {
  return fmt::format("sdsw{:03d}", sdswIndex);
}

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

cfg::SwitchConfig AgentFabricSwitchFabricLinkMonitoringTest::initialConfig(
    const AgentEnsemble& ensemble) const {
  cfg::SwitchConfig config;

  // Switch settings with hardcoded switch IDs to ensure consistency
  // between cold boot and warm boot.
  config.switchSettings()->switchType() = cfg::SwitchType::FABRIC;
  config.switchSettings()->switchId() = 0;

  std::map<int64_t, cfg::SwitchInfo> switchIdToSwitchInfo;

  cfg::SwitchInfo switchInfo0;
  switchInfo0.switchType() = cfg::SwitchType::FABRIC;
  switchInfo0.asicType() = cfg::AsicType::ASIC_TYPE_RAMON3;
  switchInfo0.switchIndex() = 0;
  cfg::Range64 portIdRange0;
  portIdRange0.minimum() = 0;
  portIdRange0.maximum() = 2047;
  switchInfo0.portIdRange() = portIdRange0;
  switchInfo0.connectionHandle() = "15:00";
  cfg::SystemPortRanges sysPortRanges0;
  switchInfo0.systemPortRanges() = sysPortRanges0;
  switchIdToSwitchInfo[0] = switchInfo0;

  cfg::SwitchInfo switchInfo1;
  switchInfo1.switchType() = cfg::SwitchType::FABRIC;
  switchInfo1.asicType() = cfg::AsicType::ASIC_TYPE_RAMON3;
  switchInfo1.switchIndex() = 1;
  cfg::Range64 portIdRange1;
  portIdRange1.minimum() = 2048;
  portIdRange1.maximum() = 4095;
  switchInfo1.portIdRange() = portIdRange1;
  switchInfo1.connectionHandle() = "16:00";
  cfg::SystemPortRanges sysPortRanges1;
  switchInfo1.systemPortRanges() = sysPortRanges1;
  switchIdToSwitchInfo[2] = switchInfo1;

  config.switchSettings()->switchIdToSwitchInfo() = switchIdToSwitchInfo;

  addFabricPorts(config, ensemble);
  addDsfNodes(config, switchIdToSwitchInfo, ensemble);
  configureExpectedNeighborReachability(config);

  XLOG(DBG2) << "Configured DSF nodes: " << kNumRdswSwitches << " RDSWs, "
             << kNumFdswSwitches << " FDSWs, " << kNumSdswSwitches << " SDSWs";

  return config;
}

void AgentFabricSwitchFabricLinkMonitoringTest::addFabricPorts(
    cfg::SwitchConfig& config,
    const AgentEnsemble& ensemble) const {
  const auto* platformMapping = ensemble.getPlatformMapping();
  auto switchIds = ensemble.getHwAsicTable()->getSwitchIDs();
  CHECK(!switchIds.empty()) << "No switch IDs found";
  auto asic = ensemble.getHwAsicTable()->getHwAsic(*switchIds.begin());

  const auto& platformPorts = platformMapping->getPlatformPorts();
  const auto& portToDefaultProfileID = utility::getPortToDefaultProfileIDMap();
  auto desiredLoopbackMode = cfg::PortLoopbackMode::NONE;
  if (auto iter = asic->desiredLoopbackModes().find(cfg::PortType::FABRIC_PORT);
      iter != asic->desiredLoopbackModes().end()) {
    desiredLoopbackMode = iter->second;
  }

  for (const auto& [portIdInt, platformPort] : platformPorts) {
    if (*platformPort.mapping()->portType() != cfg::PortType::FABRIC_PORT) {
      continue;
    }
    auto profileIter = portToDefaultProfileID.find(PortID(portIdInt));
    if (profileIter == portToDefaultProfileID.end()) {
      continue;
    }
    auto profileId = profileIter->second;

    cfg::Port portCfg;
    portCfg.logicalID() = portIdInt;
    portCfg.name() = *platformPort.mapping()->name();
    portCfg.portType() = cfg::PortType::FABRIC_PORT;
    portCfg.state() = cfg::PortState::ENABLED;
    portCfg.loopbackMode() = desiredLoopbackMode;
    portCfg.profileID() = profileId;
    portCfg.speed() = utility::getSpeed(profileId);
    portCfg.ingressVlan() = 0;
    portCfg.maxFrameSize() = 0;
    portCfg.routable() = true;
    portCfg.parserType() = cfg::ParserType::L3;

    config.ports()->push_back(portCfg);
  }
}

void AgentFabricSwitchFabricLinkMonitoringTest::addDsfNodes(
    cfg::SwitchConfig& config,
    const std::map<int64_t, cfg::SwitchInfo>& switchIdToSwitchInfo,
    const AgentEnsemble& ensemble) const {
  auto fdswAsicType = cfg::AsicType::ASIC_TYPE_RAMON3;
  auto fdswPlatformType = PlatformType::PLATFORM_MERU800BFA;
  auto sdswAsicType = cfg::AsicType::ASIC_TYPE_RAMON3;
  auto sdswPlatformType = PlatformType::PLATFORM_MERU800BFA;
  auto rdswAsicType = cfg::AsicType::ASIC_TYPE_JERICHO3;
  auto rdswPlatformType = PlatformType::PLATFORM_MERU800BIA;

  config.dsfNodes()->clear();

  // Add local FDSW (L1 fabric switch) DSF nodes - one for each ASIC
  auto fabricAsics = ensemble.getHwAsicTable()->getFabricAsics();
  auto localAsicType = fabricAsics.empty()
      ? cfg::AsicType::ASIC_TYPE_RAMON3
      : (*fabricAsics.begin())->getAsicType();
  for (const auto& [localSwitchId, switchInfo] : switchIdToSwitchInfo) {
    utility::addFabricDsfNode(
        config,
        localSwitchId,
        getFdswName(1), // Local switch is fdsw001
        utility::kL1FabricLevel,
        fdswPlatformType,
        localAsicType);
  }

  // Add RDSW (VOQ) nodes
  for (int i = 0; i < kNumRdswSwitches; i++) {
    int64_t switchId = kRdswSwitchIdStart + (i * kNumSwitchIdsPerFdsw);
    cfg::DsfNode node;
    node.switchId() = switchId;
    node.name() = getRdswName(i + 1);
    node.type() = cfg::DsfNodeType::INTERFACE_NODE;
    node.asicType() = rdswAsicType;
    node.platformType() = rdswPlatformType;
    int64_t baseOffset = i * 1024;
    node.localSystemPortOffset() = baseOffset;
    node.globalSystemPortOffset() = baseOffset;
    cfg::SystemPortRanges sysPortRanges;
    cfg::Range64 range;
    range.minimum() = baseOffset;
    range.maximum() = baseOffset + 1023;
    sysPortRanges.systemPortRanges()->push_back(range);
    node.systemPortRanges() = sysPortRanges;
    node.inbandPortId() = 1;
    (*config.dsfNodes())[switchId] = node;
  }

  // Add remote FDSW (L1 fabric) nodes — skip i=0 (local switch already added
  // above with hardware-detected localAsicType)
  for (int i = 1; i < kNumFdswSwitches; i++) {
    std::string fdswName = getFdswName(i + 1);
    for (int asicIdx = 0; asicIdx < kNumFdswAsicsPerSwitch; asicIdx++) {
      int64_t switchId =
          kFdswSwitchIdStart + (i * kNumSwitchIdsPerFdsw) + (asicIdx * 2);
      utility::addFabricDsfNode(
          config,
          switchId,
          fdswName,
          utility::kL1FabricLevel,
          fdswPlatformType,
          fdswAsicType);
    }
  }

  // Add SDSW (L2 fabric) nodes
  for (int i = 0; i < kNumSdswSwitches; i++) {
    std::string sdswName = getSdswName(i + 1);
    for (int asicIdx = 0; asicIdx < kNumSdswAsicsPerSwitch; asicIdx++) {
      int64_t switchId =
          kSdswSwitchIdStart + (i * kNumSdswAsicsPerSwitch) + asicIdx;
      utility::addFabricDsfNode(
          config,
          switchId,
          sdswName,
          utility::kL2FabricLevel,
          sdswPlatformType,
          sdswAsicType);
    }
  }
}

void AgentFabricSwitchFabricLinkMonitoringTest::
    configureExpectedNeighborReachability(cfg::SwitchConfig& config) const {
  // Assign expected neighbors sequentially across fabric ports.
  // Each ASIC has 256 fabric ports split into:
  //   - 128 ports → RDSWs (one per RDSW, not monitored by L1)
  //   - 64 ports → SDSWs (one per SDSW, monitored by L1)
  //   - 64 ports → unassigned (no expected neighbor)
  // The SDSW count per ASIC is limited to 64 to match the hardware's
  // cell profile capacity for fabric link monitoring.
  constexpr int kSdswPortsPerAsic = 64;
  int portIndex = 0;

  for (auto& portCfg : *config.ports()) {
    if (*portCfg.portType() != cfg::PortType::FABRIC_PORT) {
      continue;
    }

    // Position within the current ASIC's 256-port block
    int asicPortIndex = portIndex % kNumFabricPortsPerAsic;
    cfg::PortNeighbor neighbor;
    std::string remotePort = fmt::format("fab1/1/{}", (portIndex % 4) + 1);

    if (asicPortIndex < kNumRdswSwitches) {
      // First 128 ports per ASIC → one RDSW each
      neighbor.remoteSystem() = getRdswName(asicPortIndex + 1);
      neighbor.remotePort() = remotePort;
      portCfg.expectedNeighborReachability() = {neighbor};
    } else if (asicPortIndex < kNumRdswSwitches + kSdswPortsPerAsic) {
      // Next 64 ports per ASIC → one SDSW each
      int sdswIndex = asicPortIndex - kNumRdswSwitches;
      neighbor.remoteSystem() = getSdswName(sdswIndex + 1);
      neighbor.remotePort() = remotePort;
      portCfg.expectedNeighborReachability() = {neighbor};
    }
    portIndex++;
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
