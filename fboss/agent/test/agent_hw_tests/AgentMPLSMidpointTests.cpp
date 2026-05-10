// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <boost/container/flat_set.hpp>
#include <folly/Conv.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>

#include <array>
#include <memory>
#include <type_traits>

#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/TxPacket.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/if/gen-cpp2/common_types.h"
#include "fboss/agent/packet/EthFrame.h"
#include "fboss/agent/state/PortDescriptor.h"
#include "fboss/agent/test/AgentHwTest.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/test/TrunkUtils.h"
#include "fboss/agent/test/utils/PortStatsTestUtils.h"
#include "fboss/agent/types.h"

#include <gtest/gtest.h>

namespace {

const facebook::fboss::Label kTopLabel{1101};

using MplsMidpointPortTypes =
    ::testing::Types<facebook::fboss::PortID, facebook::fboss::AggregatePortID>;

} // namespace

namespace facebook::fboss {

enum class MplsPayloadIpVersion {
  V4,
  V6,
};

enum class MplsPacketInjectionType {
  FrontPanel,
  Cpu,
};

const char* name(MplsPayloadIpVersion ipVersion) {
  switch (ipVersion) {
    case MplsPayloadIpVersion::V4:
      return "IPv4";
    case MplsPayloadIpVersion::V6:
      return "IPv6";
  }
}

const char* name(MplsPacketInjectionType injectionType) {
  switch (injectionType) {
    case MplsPacketInjectionType::FrontPanel:
      return "front-panel";
    case MplsPacketInjectionType::Cpu:
      return "cpu";
  }
}

template <typename PortType>
class AgentMPLSMidpointTest : public AgentHwTest {
 protected:
  static constexpr bool kIsTrunk = std::is_same_v<PortType, AggregatePortID>;
  using EcmpSetupHelper =
      utility::MplsEcmpSetupTargetedPorts<folly::IPAddressV6>;

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto config = utility::onePortPerInterfaceConfig(
        ensemble.getSw(),
        ensemble.masterLogicalPortIds(),
        true /* interfaceHasSubnet */);

    if constexpr (kIsTrunk) {
      utility::addAggPort(1, {ensemble.masterLogicalPortIds()[0]}, &config);
    }

    return config;
  }

  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    if constexpr (kIsTrunk) {
      return {ProductionFeature::MPLS_MIDPOINT, ProductionFeature::LAG};
    }
    return {ProductionFeature::MPLS_MIDPOINT};
  }

  PortID egressPort() const {
    return masterLogicalInterfacePortIds()[0];
  }

  PortDescriptor egressPortDescriptor() const {
    if constexpr (kIsTrunk) {
      return PortDescriptor(AggregatePortID(1));
    }
    return PortDescriptor(egressPort());
  }

  PortID ingressPort() const {
    return masterLogicalInterfacePortIds()[1];
  }

  std::unique_ptr<EcmpSetupHelper> setupECMPHelper() const {
    return std::make_unique<EcmpSetupHelper>(
        getProgrammedState(),
        getSw()->needL2EntryForNeighbor(),
        kTopLabel,
        LabelForwardingAction::LabelForwardingType::PUSH);
  }

  void configureStaticMplsPushRoute(cfg::SwitchConfig& config) const {
    config.staticMplsRoutesWithNhops()->resize(1);
    auto& route = config.staticMplsRoutesWithNhops()[0];
    route.ingressLabel() = kTopLabel.value();

    auto helper = setupECMPHelper();
    auto nhop = helper->nhop(egressPortDescriptor());

    NextHopThrift nextHop;
    nextHop.address() = network::toBinaryAddress(nhop.ip);
    nextHop.address()->ifName() = folly::to<std::string>("fboss", nhop.intf);
    nextHop.mplsAction() = nhop.action.toThrift();
    route.nexthops()->push_back(nextHop);
  }

  void resolveNextHop() {
    applyNewState(
        [this](const std::shared_ptr<SwitchState>& state) {
          auto helper = EcmpSetupHelper(
              state,
              getSw()->needL2EntryForNeighbor(),
              kTopLabel,
              LabelForwardingAction::LabelForwardingType::PUSH);
          return helper.resolveNextHops(
              state,
              boost::container::flat_set<PortDescriptor>{
                  egressPortDescriptor()});
        },
        "resolve midpoint MPLS nexthop");
  }

  void applyConfigAndEnableTrunks(const cfg::SwitchConfig& config) {
    applyNewConfig(config);
    if constexpr (kIsTrunk) {
      applyNewState(
          [](const std::shared_ptr<SwitchState>& state) {
            return utility::enableTrunkPorts(state);
          },
          "enable trunk ports");
    }
  }

  std::unique_ptr<TxPacket> makeMplsIngressPacket(
      MplsPayloadIpVersion ipVersion) const {
    auto vlan = getVlanIDForTx();
    CHECK(vlan.has_value());

    MPLSHdr::Label mplsLabel{
        static_cast<uint32_t>(kTopLabel.value()), 0, true, 128};
    std::unique_ptr<TxPacket> pkt;
    if (ipVersion == MplsPayloadIpVersion::V4) {
      auto frame = utility::getEthFrame(
          utility::kLocalCpuMac(),
          utility::kLocalCpuMac(),
          {mplsLabel},
          folly::IPAddressV4{"100.1.1.1"},
          folly::IPAddressV4{"200.1.1.1"},
          10000,
          20000,
          *vlan);
      pkt = frame.getTxPacket(
          [sw = getSw()](uint32_t size) { return sw->allocatePacket(size); });
    } else {
      auto frame = utility::getEthFrame(
          utility::kLocalCpuMac(),
          utility::kLocalCpuMac(),
          {mplsLabel},
          folly::IPAddressV6{"1001::1"},
          folly::IPAddressV6{"2001::1"},
          10000,
          20000,
          *vlan);
      pkt = frame.getTxPacket(
          [sw = getSw()](uint32_t size) { return sw->allocatePacket(size); });
    }
    return pkt;
  }

  void sendMplsIngressPacket(
      MplsPayloadIpVersion ipVersion,
      MplsPacketInjectionType injectionType) {
    auto pkt = makeMplsIngressPacket(ipVersion);
    switch (injectionType) {
      case MplsPacketInjectionType::FrontPanel:
        EXPECT_TRUE(
            getAgentEnsemble()->ensureSendPacketOutOfPort(
                std::move(pkt), ingressPort()));
        break;
      case MplsPacketInjectionType::Cpu:
        EXPECT_TRUE(
            getAgentEnsemble()->ensureSendPacketSwitched(std::move(pkt)));
        break;
    }
  }

  void verifyMplsPushForwarding(
      MplsPayloadIpVersion ipVersion,
      MplsPacketInjectionType injectionType) {
    SCOPED_TRACE(
        folly::to<std::string>(
            "ipVersion=",
            name(ipVersion),
            " injectionType=",
            name(injectionType)));

    auto outPktsBefore =
        utility::getPortOutPkts(getLatestPortStats(egressPort()));

    sendMplsIngressPacket(ipVersion, injectionType);

    WITH_RETRIES({
      auto outPktsAfter =
          utility::getPortOutPkts(getLatestPortStats(egressPort()));
      EXPECT_EVENTUALLY_EQ(1, outPktsAfter - outPktsBefore);
    });
  }
};

TYPED_TEST_SUITE(AgentMPLSMidpointTest, MplsMidpointPortTypes);

TYPED_TEST(AgentMPLSMidpointTest, StaticMplsRoutePush) {
  auto setup = [this]() {
    auto config = this->initialConfig(*this->getAgentEnsemble());
    this->configureStaticMplsPushRoute(config);
    this->applyConfigAndEnableTrunks(config);
    this->resolveNextHop();
  };

  auto verify = [this]() {
    constexpr std::array kIpVersions{
        MplsPayloadIpVersion::V4,
        MplsPayloadIpVersion::V6,
    };
    constexpr std::array kInjectionTypes{
        MplsPacketInjectionType::FrontPanel,
        MplsPacketInjectionType::Cpu,
    };
    for (auto ipVersion : kIpVersions) {
      for (auto injectionType : kInjectionTypes) {
        this->verifyMplsPushForwarding(ipVersion, injectionType);
      }
    }
  };

  this->verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
