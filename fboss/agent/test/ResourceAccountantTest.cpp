/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <gtest/gtest.h>

#include "fboss/agent/HwAsicTable.h"
#include "fboss/agent/ResourceAccountant.h"
#include "fboss/agent/state/RouteNextHopEntry.h"
#include "fboss/agent/test/TestUtils.h"

namespace facebook::fboss {

class ResourceAccountantTest : public ::testing::Test {
 public:
  void SetUp() override {
    std::map<int64_t, cfg::SwitchInfo> switchIdToSwitchInfo{
        {0, createSwitchInfo(cfg::SwitchType::NPU)}};
    asicTable_ =
        std::make_unique<HwAsicTable>(switchIdToSwitchInfo, std::nullopt);
    resourceAccountant_ =
        std::make_unique<ResourceAccountant>(asicTable_.get());
  }

  const std::shared_ptr<Route<folly::IPAddressV6>> makeV6Route(
      const RoutePrefix<folly::IPAddressV6>& prefix,
      const RouteNextHopEntry& entry) {
    auto route = std::make_shared<Route<folly::IPAddressV6>>(prefix);
    route->setResolved(entry);
    return route;
  }

  uint64_t getMaxEcmpGroups() {
    auto maxEcmpGroups = asicTable_->getHwAsic(SwitchID(0))->getMaxEcmpGroups();
    CHECK(maxEcmpGroups.has_value());
    return maxEcmpGroups.value();
  }

  std::unique_ptr<ResourceAccountant> resourceAccountant_;
  std::unique_ptr<HwAsicTable> asicTable_;
};

TEST_F(ResourceAccountantTest, getMemberCountForEcmpGroup) {
  constexpr auto ecmpWeight = 1;
  constexpr auto ecmpWidth = 36;

  RouteNextHopSet ecmpNexthops;
  for (int i = 0; i < ecmpWidth; i++) {
    ecmpNexthops.insert(ResolvedNextHop(
        folly::IPAddress(folly::to<std::string>("1.1.1.", i + 1)),
        InterfaceID(i + 1),
        ecmpWeight));
  }
  RouteNextHopEntry ecmpNextHopEntry =
      RouteNextHopEntry(ecmpNexthops, AdminDistance::EBGP);
  EXPECT_EQ(
      ecmpWidth,
      this->resourceAccountant_->getMemberCountForEcmpGroup(ecmpNextHopEntry));

  RouteNextHopSet ucmpNexthops;
  for (int i = 0; i < ecmpWidth; i++) {
    ucmpNexthops.insert(ResolvedNextHop(
        folly::IPAddress(folly::to<std::string>("1.1.1.", i + 1)),
        InterfaceID(i + 1),
        i + 1));
  }
  RouteNextHopEntry ucmpNextHopEntry =
      RouteNextHopEntry(ucmpNexthops, AdminDistance::EBGP);
  auto expectedTotalMember = 0;
  for (const auto& nhop : ucmpNextHopEntry.normalizedNextHops()) {
    expectedTotalMember += nhop.weight();
  }
  EXPECT_EQ(
      expectedTotalMember,
      this->resourceAccountant_->getMemberCountForEcmpGroup(ucmpNextHopEntry));
}

TEST_F(ResourceAccountantTest, checkEcmpResource) {
  // MockAsic is configured to support 4 group and 64 members.
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      true /* intermediateState */));
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      false /* intermediateState */));

  // Add one ECMP group with 36 members
  constexpr auto ecmpWeight = 1;
  constexpr auto ecmpWidth = 36;
  RouteNextHopSet ecmpNexthops0;
  for (int i = 0; i < ecmpWidth; i++) {
    ecmpNexthops0.insert(ResolvedNextHop(
        folly::IPAddress(folly::to<std::string>("1.1.1.", i + 1)),
        InterfaceID(i + 1),
        ecmpWeight));
  }
  this->resourceAccountant_->ecmpGroupRefMap_[ecmpNexthops0] = 1;
  this->resourceAccountant_->ecmpMemberUsage_ += ecmpWidth;
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      true /* intermediateState */));
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      false /* intermediateState */));

  // Add another ECMP group with 36 members
  RouteNextHopSet ecmpNexthops1;
  for (int i = 0; i < ecmpWidth; i++) {
    ecmpNexthops1.insert(ResolvedNextHop(
        folly::IPAddress(folly::to<std::string>("2.1.1.", i + 1)),
        InterfaceID(i + 1),
        ecmpWeight));
  }
  this->resourceAccountant_->ecmpGroupRefMap_[ecmpNexthops1] = 1;
  this->resourceAccountant_->ecmpMemberUsage_ += ecmpWidth;
  EXPECT_FALSE(this->resourceAccountant_->checkEcmpResource(
      true /* intermediateState */));
  EXPECT_FALSE(this->resourceAccountant_->checkEcmpResource(
      false /* intermediateState */));

  // Remove ecmpGroup1
  this->resourceAccountant_->ecmpGroupRefMap_.erase(ecmpNexthops1);
  this->resourceAccountant_->ecmpMemberUsage_ -= ecmpWidth;
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      true /* intermediateState */));
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      false /* intermediateState */));

  // Add three groups (with width = 2 each) and remove ecmpGroup0
  std::vector<RouteNextHopSet> ecmpNexthopsList;
  for (int i = 0; i < getMaxEcmpGroups() - 1; i++) {
    ecmpNexthopsList.push_back(RouteNextHopSet{
        ResolvedNextHop(
            folly::IPAddress(folly::to<std::string>("1.1.1.", i + 1)),
            InterfaceID(i + 1),
            ecmpWeight),
        ResolvedNextHop(
            folly::IPAddress(folly::to<std::string>("1.1.1.", i + 2)),
            InterfaceID(i + 2),
            ecmpWeight)});
  }
  for (const auto& nhopSet : ecmpNexthopsList) {
    this->resourceAccountant_->ecmpGroupRefMap_[nhopSet] = 1;
    this->resourceAccountant_->ecmpMemberUsage_ += 2;
  }
  this->resourceAccountant_->ecmpGroupRefMap_.erase(ecmpNexthops0);
  this->resourceAccountant_->ecmpMemberUsage_ -= ecmpWidth;
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      true /* intermediateState */));
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      false /* intermediateState */));

  // Add one more group to exceed group limit
  RouteNextHopSet ecmpNexthops2 = RouteNextHopSet{
      ResolvedNextHop(
          folly::IPAddress(folly::to<std::string>("3.1.1.1")),
          InterfaceID(1),
          ecmpWeight),
      ResolvedNextHop(
          folly::IPAddress(folly::to<std::string>("3.1.1.2")),
          InterfaceID(2),
          ecmpWeight)};
  this->resourceAccountant_->ecmpGroupRefMap_[ecmpNexthops2] = 1;
  this->resourceAccountant_->ecmpMemberUsage_ += 2;
  EXPECT_TRUE(this->resourceAccountant_->checkEcmpResource(
      true /* intermediateState */));
  EXPECT_FALSE(this->resourceAccountant_->checkEcmpResource(
      false /* intermediateState */));
}

TEST_F(ResourceAccountantTest, checkAndUpdateEcmpResource) {
  // Add one ECMP group with 36 members
  constexpr auto ecmpWeight = 1;
  constexpr auto ecmpWidth = 36;
  RouteNextHopSet ecmpNexthops0;
  for (int i = 0; i < ecmpWidth; i++) {
    ecmpNexthops0.insert(ResolvedNextHop(
        folly::IPAddress(folly::to<std::string>("1::", i + 1)),
        InterfaceID(i + 1),
        ecmpWeight));
  }
  const auto route0 = makeV6Route(
      {folly::IPAddressV6("100::1"), 128},
      {ecmpNexthops0, AdminDistance::EBGP});
  EXPECT_TRUE(
      this->resourceAccountant_->checkAndUpdateEcmpResource<folly::IPAddressV6>(
          route0, true /* add */));

  // Add another ECMP group with 36 members
  RouteNextHopSet ecmpNexthops1;
  for (int i = 0; i < ecmpWidth; i++) {
    ecmpNexthops1.insert(ResolvedNextHop(
        folly::IPAddress(folly::to<std::string>("2.1.1.", i + 1)),
        InterfaceID(i + 1),
        ecmpWeight));
  }
  const auto route1 = makeV6Route(
      {folly::IPAddressV6("200::1"), 128},
      {ecmpNexthops1, AdminDistance::EBGP});
  EXPECT_FALSE(
      this->resourceAccountant_->checkAndUpdateEcmpResource<folly::IPAddressV6>(
          route1, true /* add */));

  // Rmove above route
  EXPECT_TRUE(
      this->resourceAccountant_->checkAndUpdateEcmpResource<folly::IPAddressV6>(
          route1, false /* add */));

  // Add four groups (with width = 2 each) and remove ecmpGroup0
  std::vector<std::shared_ptr<Route<folly::IPAddressV6>>> routes;
  for (int i = 0; i < getMaxEcmpGroups(); i++) {
    auto ecmpNextHops = RouteNextHopSet{
        ResolvedNextHop(
            folly::IPAddress(folly::to<std::string>("1.1.1.", i + 1)),
            InterfaceID(i + 1),
            ecmpWeight),
        ResolvedNextHop(
            folly::IPAddress(folly::to<std::string>("1.1.1.", i + 2)),
            InterfaceID(i + 2),
            ecmpWeight)};
    const auto route = makeV6Route(
        {folly::IPAddressV6(folly::to<std::string>(i + 1, "00::1")), 128},
        {ecmpNextHops, AdminDistance::EBGP});
    if (i < getMaxEcmpGroups() - 1) {
      EXPECT_TRUE(this->resourceAccountant_
                      ->checkAndUpdateEcmpResource<folly::IPAddressV6>(
                          route, true /* add */));
    } else {
      EXPECT_FALSE(this->resourceAccountant_
                       ->checkAndUpdateEcmpResource<folly::IPAddressV6>(
                           route, true /* add */));
    }
  }

  EXPECT_TRUE(
      this->resourceAccountant_->checkAndUpdateEcmpResource<folly::IPAddressV6>(
          route0, false /* add */));
}

} // namespace facebook::fboss