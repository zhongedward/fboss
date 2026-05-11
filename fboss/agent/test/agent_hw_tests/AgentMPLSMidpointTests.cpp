// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <boost/container/flat_set.hpp>
#include <folly/Conv.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/logging/xlog.h>

#include <array>
#include <memory>
#include <type_traits>
#include <utility>

#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/AgentFeatures.h"
#include "fboss/agent/TxPacket.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/if/gen-cpp2/common_types.h"
#include "fboss/agent/packet/EthFrame.h"
#include "fboss/agent/state/PortDescriptor.h"
#include "fboss/agent/test/AgentHwTest.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/test/TrunkUtils.h"
#include "fboss/agent/test/utils/CoppTestUtils.h"
#include "fboss/agent/test/utils/PacketSnooper.h"
#include "fboss/agent/test/utils/PortStatsTestUtils.h"
#include "fboss/agent/test/utils/TrapPacketUtils.h"
#include "fboss/agent/types.h"

#include <gtest/gtest.h>

namespace {

const facebook::fboss::Label kTopLabel{1101};
const facebook::fboss::LabelForwardingAction::LabelStack kPushedLabelStack{101};
const facebook::fboss::LabelForwardingAction::Label kSwapLabel{201};
constexpr auto kGetQueueOutPktsRetryTimes = 5;

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

enum class MplsTrapPacketMechanism {
  SrcPortAcl,
  TtlExpiry,
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

const char* name(MplsTrapPacketMechanism mechanism) {
  switch (mechanism) {
    case MplsTrapPacketMechanism::SrcPortAcl:
      return "src-port-acl";
    case MplsTrapPacketMechanism::TtlExpiry:
      return "ttl-expiry";
  }
}

template <typename PortType>
class AgentMPLSMidpointTest : public AgentHwTest {
 protected:
  static constexpr bool kIsTrunk = std::is_same_v<PortType, AggregatePortID>;
  using EcmpSetupHelper =
      utility::MplsEcmpSetupTargetedPorts<folly::IPAddressV6>;

  void setCmdLineFlagOverrides() const override {
    AgentHwTest::setCmdLineFlagOverrides();
    FLAGS_observe_rx_packets_without_interface = true;
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto config = utility::onePortPerInterfaceConfig(
        ensemble.getSw(),
        ensemble.masterLogicalPortIds(),
        true /* interfaceHasSubnet */);

    if constexpr (kIsTrunk) {
      utility::addAggPort(1, {ensemble.masterLogicalPortIds()[0]}, &config);
    }

    utility::setDefaultCpuTrafficPolicyConfig(
        config, ensemble.getL3Asics(), ensemble.isSai());
    utility::addCpuQueueConfig(config, ensemble.getL3Asics(), ensemble.isSai());
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

  PortID secondPassEgressPort() const {
    return masterLogicalInterfacePortIds()[2];
  }

  MplsTrapPacketMechanism trapPacketMechanism() const {
    auto asic = checkSameAndGetAsicForTesting(getAgentEnsemble()->getL3Asics());
    return asic->isSupported(HwAsic::Feature::SAI_ACL_ENTRY_SRC_PORT_QUALIFIER)
        ? MplsTrapPacketMechanism::SrcPortAcl
        : MplsTrapPacketMechanism::TtlExpiry;
  }

  std::unique_ptr<EcmpSetupHelper> setupECMPHelper(
      Label topLabel,
      LabelForwardingAction::LabelForwardingType actionType) const {
    return std::make_unique<EcmpSetupHelper>(
        getProgrammedState(),
        getSw()->needL2EntryForNeighbor(),
        topLabel,
        actionType);
  }

  Label pushedTopLabel() const {
    CHECK(!kPushedLabelStack.empty());
    return kPushedLabelStack.back();
  }

  folly::MacAddress routerMac() const {
    return getMacForFirstInterfaceWithPortsForTesting(getProgrammedState());
  }

  void configureStaticMplsRoute(
      cfg::SwitchConfig& config,
      Label ingressLabel,
      const LabelForwardingAction& action,
      PortDescriptor nextHop) const {
    config.staticMplsRoutesWithNhops()->emplace_back();
    auto& route = config.staticMplsRoutesWithNhops()->back();
    route.ingressLabel() = ingressLabel.value();

    auto helper = setupECMPHelper(ingressLabel, action.type());
    auto nhop = helper->nhop(std::move(nextHop));

    NextHopThrift nextHopThrift;
    nextHopThrift.address() = network::toBinaryAddress(nhop.ip);
    nextHopThrift.address()->ifName() =
        folly::to<std::string>("fboss", nhop.intf);
    nextHopThrift.mplsAction() = action.toThrift();
    route.nexthops()->push_back(nextHopThrift);
  }

  void configureStaticMplsPushRoute(cfg::SwitchConfig& config) const {
    configureStaticMplsRoute(
        config,
        kTopLabel,
        LabelForwardingAction(
            LabelForwardingAction::LabelForwardingType::PUSH,
            kPushedLabelStack),
        egressPortDescriptor());
  }

  void configureStaticMplsSwapRoute(
      cfg::SwitchConfig& config,
      Label ingressLabel,
      LabelForwardingAction::Label swapLabel,
      PortDescriptor nextHop) const {
    configureStaticMplsRoute(
        config,
        ingressLabel,
        LabelForwardingAction(
            LabelForwardingAction::LabelForwardingType::SWAP, swapLabel),
        std::move(nextHop));
  }

  void configureTrapPacketMechanism(
      cfg::SwitchConfig& config,
      MplsTrapPacketMechanism mechanism) const {
    switch (mechanism) {
      case MplsTrapPacketMechanism::SrcPortAcl: {
        auto asic =
            checkSameAndGetAsicForTesting(getAgentEnsemble()->getL3Asics());
        utility::addTrapPacketAcl(asic, &config, egressPort());
        break;
      }
      case MplsTrapPacketMechanism::TtlExpiry:
        configureStaticMplsSwapRoute(
            config,
            pushedTopLabel(),
            kSwapLabel,
            PortDescriptor(secondPassEgressPort()));
        break;
    }
  }

  void resolveNextHopForPort(
      const PortDescriptor& nextHop,
      Label topLabel,
      LabelForwardingAction::LabelForwardingType actionType) {
    applyNewState(
        [this, nextHop, topLabel, actionType](
            const std::shared_ptr<SwitchState>& state) {
          auto helper = EcmpSetupHelper(
              state, getSw()->needL2EntryForNeighbor(), topLabel, actionType);
          return helper.resolveNextHops(
              state, boost::container::flat_set<PortDescriptor>{nextHop});
        },
        "resolve midpoint MPLS nexthop");
  }

  void resolveNextHop() {
    resolveNextHopForPort(
        egressPortDescriptor(),
        kTopLabel,
        LabelForwardingAction::LabelForwardingType::PUSH);
  }

  void resolveNextHopForPortWithMac(
      const PortDescriptor& nextHop,
      folly::MacAddress nextHopMac) {
    applyNewState(
        [this, nextHop, nextHopMac](const std::shared_ptr<SwitchState>& state) {
          utility::EcmpSetupTargetedPorts6 helper(
              state, getSw()->needL2EntryForNeighbor(), nextHopMac);
          return helper.resolveNextHops(
              state, boost::container::flat_set<PortDescriptor>{nextHop});
        },
        "resolve midpoint MPLS nexthop with explicit MAC");
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
      Label label,
      uint8_t ttl,
      MplsPayloadIpVersion ipVersion) const {
    auto vlan = getVlanIDForTx();
    CHECK(vlan.has_value());

    MPLSHdr::Label mplsLabel{
        static_cast<uint32_t>(label.value()), 0, true, ttl};
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
      Label label,
      uint8_t ttl,
      MplsPayloadIpVersion ipVersion,
      MplsPacketInjectionType injectionType) {
    auto pkt = makeMplsIngressPacket(label, ttl, ipVersion);
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

  void setupStaticMplsRoutePush() {
    auto mechanism = trapPacketMechanism();
    auto config = initialConfig(*getAgentEnsemble());
    configureStaticMplsPushRoute(config);
    configureTrapPacketMechanism(config, mechanism);
    applyConfigAndEnableTrunks(config);

    resolveNextHopForPortWithMac(egressPortDescriptor(), routerMac());
    if (mechanism == MplsTrapPacketMechanism::TtlExpiry) {
      resolveNextHopForPort(
          PortDescriptor(secondPassEgressPort()),
          pushedTopLabel(),
          LabelForwardingAction::LabelForwardingType::SWAP);
    }
  }

  void verifyMplsPushAndTrapPacket(
      MplsPayloadIpVersion ipVersion,
      MplsPacketInjectionType injectionType) {
    auto mechanism = trapPacketMechanism();
    SCOPED_TRACE(
        folly::to<std::string>(
            "ipVersion=",
            name(ipVersion),
            " injectionType=",
            name(injectionType),
            " trapMechanism=",
            name(mechanism),
            " isTrunk=",
            kIsTrunk));

    utility::SwSwitchPacketSnooper snooper(
        getSw(),
        "mpls-midpoint-push-verifier",
        std::nullopt,
        std::nullopt,
        std::nullopt,
        utility::packetSnooperReceivePacketType::PACKET_TYPE_ALL);
    snooper.ignoreUnclaimedRxPkts();

    auto cpuQueueOutPktsBefore = utility::getQueueOutPacketsWithRetry(
        getSw(),
        switchIdForPort(egressPort()),
        utility::kCoppLowPriQueueId,
        0 /* retryTimes */,
        0 /* expectedNumPkts */);
    auto outPktsBefore =
        utility::getPortOutPkts(getLatestPortStats(egressPort()));

    auto ttl = mechanism == MplsTrapPacketMechanism::TtlExpiry ? 2 : 128;
    sendMplsIngressPacket(kTopLabel, ttl, ipVersion, injectionType);

    WITH_RETRIES({
      auto outPktsAfter =
          utility::getPortOutPkts(getLatestPortStats(egressPort()));
      EXPECT_EVENTUALLY_EQ(1, outPktsAfter - outPktsBefore);

      if (mechanism == MplsTrapPacketMechanism::TtlExpiry) {
        auto cpuQueueOutPktsAfter = utility::getQueueOutPacketsWithRetry(
            getSw(),
            switchIdForPort(egressPort()),
            utility::kCoppLowPriQueueId,
            kGetQueueOutPktsRetryTimes,
            cpuQueueOutPktsBefore + 1);
        EXPECT_EVENTUALLY_EQ(1, cpuQueueOutPktsAfter - cpuQueueOutPktsBefore);
      }
    });

    auto pktBuf = snooper.waitForPacket(10);
    ASSERT_TRUE(pktBuf.has_value());
    ASSERT_TRUE(*pktBuf);

    folly::io::Cursor cursor((*pktBuf).get());
    utility::EthFrame frame(cursor);

    auto mplsPayload = frame.mplsPayLoad();
    ASSERT_TRUE(mplsPayload.has_value());

    const auto& mplsHeader = mplsPayload->header();
    const auto& labelStack = mplsHeader.stack();
    XLOG(INFO) << "MPLS midpoint PUSH captured header " << mplsHeader;

    ASSERT_EQ(labelStack.size(), 1);
    EXPECT_EQ(
        labelStack[0].getLabelValue(),
        static_cast<uint32_t>(pushedTopLabel().value()));
    EXPECT_TRUE(labelStack[0].isbottomOfStack());
  }
};

TYPED_TEST_SUITE(AgentMPLSMidpointTest, MplsMidpointPortTypes);

TYPED_TEST(AgentMPLSMidpointTest, StaticMplsRoutePush) {
  auto setup = [this]() { this->setupStaticMplsRoutePush(); };

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
        this->verifyMplsPushAndTrapPacket(ipVersion, injectionType);
      }
    }
  };

  this->verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
