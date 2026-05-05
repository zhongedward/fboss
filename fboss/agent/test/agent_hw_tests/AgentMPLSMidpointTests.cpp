// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <folly/IPAddressV6.h>

#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/test/AgentHwTest.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/types.h"

namespace facebook::fboss {

class AgentMPLSMidpointTest : public AgentHwTest {
 protected:
  using EcmpSetupHelper =
      utility::MplsEcmpSetupTargetedPorts<folly::IPAddressV6>;

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    return utility::onePortPerInterfaceConfig(
        ensemble.getSw(),
        ensemble.masterLogicalPortIds(),
        true /* interfaceHasSubnet */);
  }

  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::MPLS_MIDPOINT};
  }
};

} // namespace facebook::fboss
