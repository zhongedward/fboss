// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/test/agent_hw_tests/AgentMirrorOnDropStatelessTest.h"

#include <gtest/gtest.h>

#include "fboss/agent/AsicUtils.h"
#include "fboss/agent/FbossError.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/test/utils/PfcTestUtils.h"
#include "fboss/lib/CommonUtils.h"

namespace facebook::fboss {

namespace {

// Pack an ingress-pipeline drop code (default-route, ACL) into the wire
// format's two-slot drop-reason struct.
MirrorOnDropDropReasonCodes ingressOnly(uint8_t code) {
  return {.ingressDropReason = code, .egressDropReason = 0};
}

// Pack an MMU/egress drop code into the wire format's two-slot drop-reason
// struct.
MirrorOnDropDropReasonCodes egressOnly(uint8_t code) {
  return {.ingressDropReason = 0, .egressDropReason = code};
}

} // namespace

using namespace ::testing;

// Factory: ONE switch — adding a new ASIC means adding one case here.
// Concrete MirrorOnDropImpl subclasses are registered via follow-on diffs
// (see AgentMirrorOnDrop<Vendor>Impl.{h,cpp}).
std::unique_ptr<MirrorOnDropImpl> createMirrorOnDropImpl(cfg::AsicType type) {
  throw FbossError(
      "createMirrorOnDropImpl: no MirrorOnDropImpl for AsicType ",
      static_cast<int>(type));
}

MirrorOnDropImpl* AgentMirrorOnDropStatelessTest::impl() {
  if (!impl_) {
    auto* asic =
        checkSameAndGetAsicForTesting(getAgentEnsemble()->getL3Asics());
    impl_ = createMirrorOnDropImpl(asic->getAsicType());
  }
  return impl_.get();
}

std::vector<ProductionFeature>
AgentMirrorOnDropStatelessTest::getProductionFeaturesVerified() const {
  // The umbrella gate for any ASIC with a stateless MirrorOnDrop impl. We do
  // NOT call impl()->getProductionFeature() here: gtest invokes this during
  // test discovery on every platform, and the factory throws FbossError on
  // unsupported AsicTypes — calling it here would crash the test runner on
  // every non-stateless ASIC.
  return {
      ProductionFeature::MIRROR_ON_DROP,
      ProductionFeature::MIRROR_ON_DROP_STATELESS};
}

cfg::MirrorOnDropReport AgentMirrorOnDropStatelessTest::makeMirrorOnDropReport(
    const std::string& name,
    std::optional<int32_t> samplingRate) {
  return impl()->makeReport(
      name,
      kCollectorIp_,
      kMirrorDstPort,
      kMirrorSrcPort,
      kSwitchIp_,
      samplingRate);
}

MirrorOnDropPacketFields
AgentMirrorOnDropStatelessTest::parseMirrorOnDropPacket(
    const folly::IOBuf* buf) {
  return impl()->parsePacket(buf);
}

void AgentMirrorOnDropStatelessTest::verifyAsicSpecificInvariants(
    const folly::IOBuf* buf) {
  impl()->verifyInvariants(buf);
}

MirrorOnDropDropReasonCodes
AgentMirrorOnDropStatelessTest::getDefaultRouteDropReasons() {
  return ingressOnly(impl()->getDefaultRouteDropReason());
}

MirrorOnDropDropReasonCodes
AgentMirrorOnDropStatelessTest::getAclDropReasons() {
  return ingressOnly(impl()->getAclDropReason());
}

MirrorOnDropDropReasonCodes
AgentMirrorOnDropStatelessTest::getMmuDropReasons() {
  return egressOnly(impl()->getMmuDropReason());
}

void AgentMirrorOnDropStatelessTest::configureMmuDropBuffers(
    cfg::SwitchConfig& config,
    const PortID& injectionPortId,
    int priority) {
  auto hwAsic = checkSameAndGetAsicForTesting(getAgentEnsemble()->getL3Asics());
  utility::setupPfcBuffers(
      getAgentEnsemble(),
      config,
      {injectionPortId},
      {}, // losslessPgIds
      {priority}, // lossyPgIds
      {}, // tcToPgOverride
      utility::PfcBufferParams::getPfcBufferParams(
          hwAsic->getAsicType(), kDefaultGlobalSharedBytes));
}

void AgentMirrorOnDropStatelessTest::configureErspanMirror(
    cfg::SwitchConfig& config,
    const std::string& mirrorName,
    const folly::IPAddressV6& tunnelDstIp,
    const folly::IPAddressV6& tunnelSrcIp,
    const PortID& srcPortId) {
  impl()->configureErspanMirror(
      config, mirrorName, tunnelDstIp, tunnelSrcIp, srcPortId);
}

void AgentMirrorOnDropStatelessTest::validateMirrorOnDropPacket(
    const folly::IOBuf* captured,
    const PortID& injectionPortId,
    const MirrorOnDropDropReasonCodes& expectedReasons,
    std::optional<folly::IPAddressV6> expectedInnerDstIp) {
  verifyAsicSpecificInvariants(captured);
  auto fields = parseMirrorOnDropPacket(captured);

  EXPECT_EQ(fields.outerSrcMac, getLocalMacAddress());
  EXPECT_EQ(fields.outerDstMac, kCollectorNextHopMac_);
  EXPECT_EQ(fields.outerSrcIp, kSwitchIp_);
  EXPECT_EQ(fields.outerDstIp, kCollectorIp_);
  EXPECT_EQ(fields.outerSrcPort, static_cast<uint16_t>(kMirrorSrcPort));
  EXPECT_EQ(fields.outerDstPort, static_cast<uint16_t>(kMirrorDstPort));
  EXPECT_EQ(fields.ingressPort, static_cast<uint16_t>(injectionPortId));
  EXPECT_EQ(fields.dropReasonIngress, expectedReasons.ingressDropReason);
  EXPECT_EQ(fields.dropReasonEgress, expectedReasons.egressDropReason);
  EXPECT_EQ(fields.innerSrcIp, kPacketSrcIp_);

  if (expectedInnerDstIp.has_value()) {
    EXPECT_EQ(fields.innerDstIp, expectedInnerDstIp.value());
  }
}

void AgentMirrorOnDropStatelessTest::waitForStatsToStabilize(
    const std::vector<PortID>& ports) {
  constexpr int kStableIterations = 3;
  int stableCount = 0;
  // Compare against stats from the previous WITH_RETRIES iteration, not the
  // moment before WITH_RETRIES started — back-to-back samples with no time
  // gap can spuriously appear stable even under active traffic.
  std::optional<std::map<PortID, HwPortStats>> prevStats;
  WITH_RETRIES({
    auto curStats = getLatestPortStats(ports);
    if (prevStats.has_value()) {
      const bool unchanged =
          std::all_of(ports.begin(), ports.end(), [&](const auto& portId) {
            // .at() throws on missing port — operator[] would silently
            // default-construct HwPortStats and crash on the dereference of
            // an empty optional outUnicastPkts_(), masking the real cause.
            return *curStats.at(portId).outUnicastPkts_() ==
                *prevStats.value().at(portId).outUnicastPkts_();
          });
      stableCount = unchanged ? stableCount + 1 : 0;
    }
    prevStats = std::move(curStats);
    EXPECT_EVENTUALLY_GE(stableCount, kStableIterations);
  });
}

} // namespace facebook::fboss
