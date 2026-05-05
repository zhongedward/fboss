// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <folly/IPAddress.h>

#include "fboss/agent/TxPacket.h"
#include "fboss/agent/packet/PktFactory.h"
#include "fboss/agent/test/AgentHwTest.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/test/gen-cpp2/production_features_types.h"
#include "fboss/agent/test/utils/CoppTestUtils.h"
#include "fboss/agent/test/utils/LoadBalancerTestUtils.h"
#include "fboss/agent/test/utils/OlympicTestUtils.h"
#include "fboss/agent/test/utils/QueueTestUtils.h"

namespace facebook::fboss {

class AgentLinkLocalForwardingTest : public AgentHwTest {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::L3_FORWARDING};
  }

 protected:
  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentHwTest::initialConfig(ensemble);
    utility::addOlympicQosMaps(cfg, ensemble.getL3Asics());
    utility::setDefaultCpuTrafficPolicyConfig(
        cfg, ensemble.getL3Asics(), ensemble.isSai());
    utility::addCpuQueueConfig(cfg, ensemble.getL3Asics(), ensemble.isSai());
    cfg.loadBalancers()->push_back(
        utility::getEcmpFullHashConfig(ensemble.getL3Asics()));
    return cfg;
  }

  std::unique_ptr<TxPacket> makeTxPacket(
      const folly::IPAddress& dstIp,
      uint16_t l4DstPort) const {
    auto vlanId = getVlanIDForTx();
    auto intfMac =
        getMacForFirstInterfaceWithPortsForTesting(getProgrammedState());
    auto srcMac = utility::MacAddressGenerator().get(intfMac.u64HBO() + 1);
    auto srcIp = folly::IPAddress("100::1");
    return utility::makeUDPTxPacket(
        getSw(),
        vlanId,
        srcMac,
        intfMac,
        srcIp,
        dstIp,
        8000 /* l4 src port */,
        l4DstPort);
  }
};

// Resolve a link-local NDP entry on a single port and verify that a packet
// with dst = the link-local neighbor address is forwarded out (not trapped to
// CPU). Uses a non-OpenR L4 port so the recently-added
// cpuPolicing-high-openr-linklocal-acl (which matches fe80::/10 + UDP/16818)
// cannot match.
TEST_F(AgentLinkLocalForwardingTest, SingleLinkLocalNeighbor) {
  auto setup = [this]() {
    auto portIds = masterLogicalInterfacePortIds();
    CHECK(!portIds.empty());
    utility::EcmpSetupTargetedPorts<folly::IPAddressV6> ecmpHelper(
        getProgrammedState(), getSw()->needL2EntryForNeighbor());
    boost::container::flat_set<PortDescriptor> ports{
        PortDescriptor(portIds[0])};
    applyNewState([&](const std::shared_ptr<SwitchState>& in) {
      return ecmpHelper.resolveNextHops(in, ports, /*useLinkLocal=*/true);
    });
  };
  auto verify = [this]() {
    auto portIds = masterLogicalInterfacePortIds();
    utility::EcmpSetupTargetedPorts<folly::IPAddressV6> ecmpHelper(
        getProgrammedState(), getSw()->needL2EntryForNeighbor());
    // Read the dst from the same nhop the setup resolved against — guarantees
    // we send to the IP the helper installed in the NDP table, no matter what
    // convention the helper uses internally for link-local addresses.
    auto linkLocalDst =
        *ecmpHelper.nhop(PortDescriptor(portIds[0])).linkLocalNhopIp;
    getAgentEnsemble()->ensureSendPacketSwitched(
        makeTxPacket(folly::IPAddress(linkLocalDst), 8001 /* l4 dst port */));
  };
  verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
