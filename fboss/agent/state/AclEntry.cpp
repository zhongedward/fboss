/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/state/AclEntry.h"
#include <folly/Conv.h>
#include <folly/MacAddress.h>
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/state/AclMap.h"
#include "fboss/agent/state/NodeBase-defs.h"
#include "fboss/agent/state/SwitchState.h"
#include "folly/IPAddress.h"

using apache::thrift::TEnumTraits;
using folly::IPAddress;

namespace {} // namespace

namespace facebook::fboss {

state::AclTtl AclTtl::toThrift() const {
  auto aclTtl = state::AclTtl();
  aclTtl.value() = value_;
  aclTtl.mask() = mask_;
  return aclTtl;
}

AclTtl AclTtl::fromThrift(state::AclTtl const& ttl) {
  return AclTtl(
      folly::copy(ttl.value().value()), folly::copy(ttl.mask().value()));
}

std::set<cfg::AclTableQualifier> AclEntry::getRequiredAclTableQualifiers()
    const {
  std::set<cfg::AclTableQualifier> qualifiers{};
  auto setIpQualifier = [&qualifiers](
                            auto cidr, auto v4Qualifier, auto v6Qualifier) {
    if (cidr == folly::CIDRNetwork(folly::IPAddress(), 0)) {
      return;
    }
    if (cidr.first.isV4()) {
      qualifiers.insert(v4Qualifier);
    } else {
      qualifiers.insert(v6Qualifier);
    }
  };
  setIpQualifier(
      getSrcIp(),
      cfg::AclTableQualifier::SRC_IPV4,
      cfg::AclTableQualifier::SRC_IPV6);
  setIpQualifier(
      getDstIp(),
      cfg::AclTableQualifier::DST_IPV4,
      cfg::AclTableQualifier::DST_IPV6);

  if (getProto()) {
    qualifiers.insert(cfg::AclTableQualifier::IP_PROTOCOL_NUMBER);
  }
  if (getTcpFlagsBitMap()) {
    qualifiers.insert(cfg::AclTableQualifier::TCP_FLAGS);
  }
  if (getSrcPort()) {
    qualifiers.insert(cfg::AclTableQualifier::SRC_PORT);
  }
  if (getDstPort()) {
    qualifiers.insert(cfg::AclTableQualifier::OUT_PORT);
  }
  if (getIpFrag()) {
    qualifiers.insert(cfg::AclTableQualifier::IP_FRAG);
  }
  if (getIcmpType() && getProto()) {
    auto proto = getProto().value();
    if (proto == 1) {
      qualifiers.insert(cfg::AclTableQualifier::ICMPV4_TYPE);
    } else {
      qualifiers.insert(cfg::AclTableQualifier::ICMPV6_TYPE);
    }
  }
  if (getDscp()) {
    qualifiers.insert(cfg::AclTableQualifier::DSCP);
  }
  if (getIpType()) {
    qualifiers.insert(cfg::AclTableQualifier::IP_TYPE);
  }
  if (getTtl()) {
    qualifiers.insert(cfg::AclTableQualifier::TTL);
  }
  if (getDstMac()) {
    qualifiers.insert(cfg::AclTableQualifier::DST_MAC);
  }
  if (getL4SrcPort()) {
    qualifiers.insert(cfg::AclTableQualifier::L4_SRC_PORT);
  }
  if (getL4DstPort()) {
    qualifiers.insert(cfg::AclTableQualifier::L4_DST_PORT);
  }
  if (getLookupClassL2()) {
    qualifiers.insert(cfg::AclTableQualifier::LOOKUP_CLASS_L2);
  }
  if (getLookupClassNeighbor()) {
    qualifiers.insert(cfg::AclTableQualifier::LOOKUP_CLASS_NEIGHBOR);
  }
  if (getLookupClassRoute()) {
    qualifiers.insert(cfg::AclTableQualifier::LOOKUP_CLASS_ROUTE);
  }
  if (getPacketLookupResult()) {
    // TODO: add qualifier in AclTableQualifier enum
  }
  if (getEtherType()) {
    qualifiers.insert(cfg::AclTableQualifier::ETHER_TYPE);
  }
  if (getUdfGroups()) {
    qualifiers.insert(cfg::AclTableQualifier::UDF);
  }
  return qualifiers;
}

AclEntry* AclEntry::modify(
    std::shared_ptr<SwitchState>* state,
    const HwSwitchMatcher& matcher) {
  if (!isPublished()) {
    CHECK(!(*state)->isPublished());
    return this;
  }

  MultiSwitchAclMap* acls = (*state)->getAcls()->modify(state);
  auto newEntry = clone();
  auto* ptr = newEntry.get();
  acls->updateNode(std::move(newEntry), matcher);
  return ptr;
}

AclEntry::AclEntry(int priority, const std::string& name) {
  set<switch_state_tags::priority>(priority);
  set<switch_state_tags::name>(name);
}

AclEntry::AclEntry(int priority, std::string&& name) {
  set<switch_state_tags::priority>(priority);
  set<switch_state_tags::name>(std::move(name));
}

template struct ThriftStructNode<AclEntry, state::AclEntryFields>;

} // namespace facebook::fboss
