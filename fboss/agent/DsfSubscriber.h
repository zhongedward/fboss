// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "fboss/agent/DsfSession.h"
#include "fboss/agent/StateObserver.h"
#include "fboss/fsdb/client/FsdbPubSubManager.h"
#include "folly/Synchronized.h"

#include <gtest/gtest.h>
#include <memory>

DECLARE_bool(dsf_subscriber_skip_hw_writes);
DECLARE_bool(dsf_subscriber_cache_updated_state);

namespace facebook::fboss {
class SwSwitch;
class SwitchState;
class InterfaceMap;
class SystemPortMap;
namespace fsdb {
class FsdbPubSubManager;
}

class DsfSubscriber : public StateObserver {
 public:
  explicit DsfSubscriber(SwSwitch* sw);
  ~DsfSubscriber() override;
  void stateUpdated(const StateDelta& stateDelta) override;

  void stop();

  // Used in tests for asserting on modifications
  // made by DsfSubscriber
  const std::shared_ptr<SwitchState> cachedState() const {
    return cachedState_;
  }

  const std::vector<fsdb::FsdbPubSubManager::SubscriptionInfo>
  getSubscriptionInfo() const {
    if (fsdbPubSubMgr_) {
      return fsdbPubSubMgr_->getSubscriptionInfo();
    }
    return {};
  }

  std::string getClientId() const {
    if (fsdbPubSubMgr_) {
      return fsdbPubSubMgr_->getClientId();
    }
    throw FbossError("DsfSubscriber: fsdbPubSubMgr_ is null");
  }

  std::vector<DsfSessionThrift> getDsfSessionsThrift() const;
  static std::string makeRemoteEndpoint(
      const std::string& remoteNode,
      const folly::IPAddress& remoteIP);

 private:
  void updateWithRollbackProtection(
      const std::string& nodeName,
      const SwitchID& nodeSwitchId,
      const std::map<SwitchID, std::shared_ptr<SystemPortMap>>&
          switchId2SystemPorts,
      const std::map<SwitchID, std::shared_ptr<InterfaceMap>>& switchId2Intfs);
  void handleFsdbSubscriptionStateUpdate(
      const std::string& remoteNodeName,
      const folly::IPAddress& remoteIP,
      const SwitchID& remoteSwitchId,
      fsdb::SubscriptionState oldState,
      fsdb::SubscriptionState newState);
  void handleFsdbUpdate(
      const folly::IPAddress& localIP,
      SwitchID remoteSwitchId,
      const std::string& remoteNodeName,
      const folly::IPAddress& remoteIP,
      fsdb::OperSubPathUnit&& operStateUnit);
  bool isLocal(SwitchID nodeSwitchId) const;
  void processGRHoldTimerExpired(
      const std::string& nodeName,
      const std::set<SwitchID>& allNodeSwitchIDs);

  SwSwitch* sw_;
  std::unique_ptr<fsdb::FsdbPubSubManager> fsdbPubSubMgr_;
  std::shared_ptr<SwitchState> cachedState_;
  std::string localNodeName_;
  folly::Synchronized<std::unordered_map<std::string, DsfSession>> dsfSessions_;

  FRIEND_TEST(DsfSubscriberTest, updateWithRollbackProtection);
  FRIEND_TEST(DsfSubscriberTest, setupNeighbors);
  FRIEND_TEST(DsfSubscriberTest, handleFsdbUpdate);
};

} // namespace facebook::fboss
