// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "fboss/agent/test/AgentHwTest.h"

namespace facebook::fboss {

// Test class for fabric link monitoring on L1 (FDSW) fabric switches.
// Tests the FDSW -> SDSW direction of fabric link monitoring packets.
// Requires DUAL_STAGE_L1 fabric node role for FabricLinkMonitoringManager.
class AgentFabricSwitchFabricLinkMonitoringTest : public AgentHwTest {
 public:
  void SetUp() override;

 protected:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::FABRIC};
  }

 private:
  void setCmdLineFlagOverrides() const override;
};

} // namespace facebook::fboss
