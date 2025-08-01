/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/lib/CommonThriftUtils.h"
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include "fboss/lib/thrift_service_client/ConnectionOptions.h"

namespace facebook::fboss {

ReconnectingThriftClient::ReconnectingThriftClient(
    const std::string& clientId,
    folly::EventBase* streamEvb,
    folly::EventBase* connRetryEvb,
    const std::string& counterPrefix,
    const std::string& aggCounterPrefix,
    StreamStateChangeCb stateChangeCb,
    int initialBackoffReconnectMs,
    int maxBackoffReconnectMs)
    : clientId_(clientId),
      streamEvb_(streamEvb),
      connRetryEvb_(connRetryEvb),
      counterPrefix_(counterPrefix),
      disconnectEvents_(
          getCounterPrefix() + ".disconnects",
          fb303::SUM,
          fb303::RATE),
      aggDisconnectEvents_(
          aggCounterPrefix + ".disconnects",
          fb303::SUM,
          fb303::RATE),
      stateChangeCb_(stateChangeCb),
      backoff_(initialBackoffReconnectMs, maxBackoffReconnectMs),
      timer_(folly::AsyncTimeout::make(*connRetryEvb, [this]() noexcept {
        timeoutExpired();
      })) {
  if (!streamEvb_ || !connRetryEvb_) {
    throw std::runtime_error(
        "Must pass valid stream, connRetry evbs to ctor, but passed null");
  }
  fb303::fbData->setCounter(getConnectedCounterName(), 0);
}

void ReconnectingThriftClient::scheduleTimeout() {
  connRetryEvb_->runInEventBaseThread(
      [this] { timer_->scheduleTimeout(backoff_.getCurTimeout()); });
}

void ReconnectingThriftClient::setState(State state) {
  State oldState;
  {
    auto stateLocked = state_.wlock();
    oldState = *stateLocked;
    if (oldState == state) {
      STREAM_XLOG(INFO) << "State not changing, skipping";
      return;
    } else if (oldState == State::CANCELLED) {
      STREAM_XLOG(INFO) << "Old state is CANCELLED, will not try to reconnect";
      return;
    }
    *stateLocked = state;
  }
  if (state == State::CONNECTING) {
#if FOLLY_HAS_COROUTINES
    serviceLoopScope_.add(co_withExecutor(streamEvb_, serviceLoopWrapper()));
#endif
  } else if (state == State::CONNECTED) {
    fb303::fbData->setCounter(getConnectedCounterName(), 1);
  } else if (state == State::CANCELLED) {
#if FOLLY_HAS_COROUTINES
    onCancellation();
    if (isGracefulServiceLoopCompletionRequested()) {
      folly::coro::blockingWait(serviceLoopScope_.joinAsync());
    } else {
      folly::coro::blockingWait(serviceLoopScope_.cancelAndJoinAsync());
    }
#endif
    fb303::fbData->setCounter(getConnectedCounterName(), 0);
  } else if (state == State::DISCONNECTED) {
    disconnectEvents_.add(1);
    aggDisconnectEvents_.add(1);
    fb303::fbData->setCounter(getConnectedCounterName(), 0);
  }
  stateChangeCb_(oldState, state);
}

void ReconnectingThriftClient::setConnectionOptions(
    utils::ConnectionOptions options,
    bool allowReset) {
  if (!allowReset && *connectionOptions_.rlock()) {
    throw std::runtime_error("Cannot reset server address");
  }
  connectionLogStr_ = fmt::format("{}->{}", clientId(), options.getHostname());
  *connectionOptions_.wlock() = std::move(options);
}

void ReconnectingThriftClient::cancel() {
  STREAM_XLOG(DBG2) << "Canceling StreamClient";

  // already disconnected;
  if (getState() == State::CANCELLED) {
    STREAM_XLOG(WARNING) << "Already cancelled";
    return;
  }
  connectionOptions_.wlock()->reset();
  connRetryEvb_->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this] { timer_->cancelTimeout(); });
  setState(State::CANCELLED);
  streamEvb_->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this] { resetClient(); });
  STREAM_XLOG(DBG2) << "Cancelled";
}

void ReconnectingThriftClient::timeoutExpired() noexcept {
  auto serverOptions = *connectionOptions_.rlock();
  if (getState() == State::DISCONNECTED) {
    backoff_.reportError();
    if (serverOptions) {
      connectToServer(*serverOptions);
    }
  } else {
    backoff_.reportSuccess();
  }
  timer_->scheduleTimeout(backoff_.getCurTimeout());
}

bool ReconnectingThriftClient::isCancelled() const {
  return (getState() == State::CANCELLED);
}

bool ReconnectingThriftClient::isConnectedToServer() const {
  return (getState() == State::CONNECTED);
}

} // namespace facebook::fboss
