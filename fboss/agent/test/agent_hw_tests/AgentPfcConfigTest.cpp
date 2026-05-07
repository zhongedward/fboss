// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/test/AgentHwTest.h"
#include "fboss/agent/test/utils/ConfigUtils.h"
#include "fboss/agent/test/utils/PfcTestUtils.h"
#include "fboss/agent/types.h"

namespace facebook::fboss {

struct PfcWdTestConfigs {
  uint32_t detectionTimeMsecs;
  uint32_t recoveryTimeMsecs;
  cfg::PfcWatchdogRecoveryAction recoveryAction;
  std::string description;
};

class AgentPfcConfigTest : public AgentHwTest {
 protected:
  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    return utility::onePortPerInterfaceConfig(
        ensemble.getSw(), ensemble.masterLogicalPortIds());
  }

  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::PFC};
  }

  // Helper to get switch ID for a port
  SwitchID getSwitchId(const PortID& portId) {
    return getAgentEnsemble()->scopeResolver().scope(portId).switchId();
  }

  // Basic config with 2 L3 interface config
  void setupBaseConfig() {
    applyNewConfig(initialConfig(*getAgentEnsemble()));
  }

  // Initialize a PFC watchdog configuration with passed in params
  void initalizePfcConfigWatchdogValues(
      cfg::PfcWatchdog& watchdog,
      const int detectionTime,
      const int recoveryTime,
      const cfg::PfcWatchdogRecoveryAction recoveryAction) {
    watchdog.recoveryAction() = recoveryAction;
    watchdog.recoveryTimeMsecs() = recoveryTime;
    watchdog.detectionTimeMsecs() = detectionTime;
  }

  /*
   * XXX: This is to be removed in a future commit, consolidating
   * PFC config additions to a single API for migration to
   * utility::addPfcConfig(), which will be integrated in one of
   * the next commits. Both kLosslessPgs and addPfcConfig will
   * need to be replaced with utility:: equivalents.
   */
  std::vector<int> kLosslessPgs() {
    return {0, 7};
  }

  std::vector<PortID> portIdsForTest() {
    if (FLAGS_hyper_port) {
      return getAgentEnsemble()->masterLogicalHyperPortIds();
    }
    return getAgentEnsemble()->masterLogicalInterfacePortIds();
  }

  void addPfcConfig(
      cfg::SwitchConfig& cfg,
      const std::vector<PortID>& ports,
      bool rxEnable = true,
      bool txEnable = true) {
    auto buffer = utility::PfcBufferParams::getPfcBufferParams(
        getAgentEnsemble()->getL3Asics().front()->getAsicType());
    utility::setupPfcBuffers(
        getAgentEnsemble(), cfg, ports, kLosslessPgs(), {}, {}, buffer);

    for (const auto& portID : ports) {
      auto portCfg = utility::findCfgPort(cfg, portID);
      portCfg->pfc()->tx() = txEnable;
      portCfg->pfc()->rx() = rxEnable;
    }
  }

  std::shared_ptr<SwitchState> setupPfcAndPfcWatchdog(
      cfg::SwitchConfig& currentConfig,
      const PortID& portId,
      const cfg::PfcWatchdog& watchdog) {
    addPfcConfig(
        currentConfig, {portId}, true /* RX enable */, true /* TX enable */);
    auto portCfg = utility::findCfgPort(currentConfig, portId);
    if (portCfg->pfc().has_value()) {
      portCfg->pfc()->watchdog() = watchdog;
    } else {
      XLOG(ERR) << "PFC is not enabled on port " << portId
                << " during PFC and watchdog setup!";
    }
    return applyNewConfig(currentConfig);
  }

  // Setup and apply the new config with passed in PFC watchdog config
  void setupPfcWatchdog(
      cfg::SwitchConfig& currentConfig,
      const PortID& portId,
      const cfg::PfcWatchdog& watchdog) {
    auto portCfg = utility::findCfgPort(currentConfig, portId);

    if (portCfg->pfc().has_value()) {
      portCfg->pfc()->watchdog() = watchdog;
    } else {
      XLOG(ERR) << "PFC is not enabled on port " << portId
                << " during PFC watchdog setup!";
    }
    applyNewConfig(currentConfig);
  }

  // Removes the PFC watchdog configuration and applies the same
  void removePfcWatchdogConfig(
      cfg::SwitchConfig& currentConfig,
      const PortID& portId) {
    auto portCfg = utility::findCfgPort(currentConfig, portId);
    if (portCfg->pfc().has_value()) {
      portCfg->pfc()->watchdog().reset();
    } else {
      XLOG(ERR) << "PFC is not enabled on port " << portId
                << " during PFC watchdog removal!";
    }
    applyNewConfig(currentConfig);
  }

  // Cross check PFC watchdog HW programming with SW config
  void runPfcWatchdogTest(const cfg::PfcWatchdog& pfcWatchdogConfig) {
    auto setup = [=, this]() {
      auto cfg = initialConfig(*getAgentEnsemble());
      setupPfcAndPfcWatchdog(cfg, this->portIdsForTest()[0], pfcWatchdogConfig);
    };

    auto verify = [=, this]() {
      auto portId = this->portIdsForTest()[0];
      EXPECT_TRUE(
          getAgentEnsemble()
              ->getHwAgentTestClient(getSwitchId(portId))
              ->sync_pfcWatchdogProgrammingMatchesConfig(
                  static_cast<int32_t>(portId), true, pfcWatchdogConfig));
    };

    verifyAcrossWarmBoots(setup, verify);
  }

  // Test to verify PFC is not configured in HW
  void runPfcNotConfiguredTest(bool rxEnabled, bool txEnabled) {
    auto setup = [=, this]() { setupBaseConfig(); };

    auto verify = [=, this]() {
      auto portId = this->portIdsForTest()[0];
      bool pfcRx = getAgentEnsemble()
                       ->getHwAgentTestClient(getSwitchId(portId))
                       ->sync_getPfcEnabled(static_cast<int32_t>(portId), true);
      bool pfcTx =
          getAgentEnsemble()
              ->getHwAgentTestClient(getSwitchId(portId))
              ->sync_getPfcEnabled(static_cast<int32_t>(portId), false);
      EXPECT_EQ(pfcRx, rxEnabled);
      EXPECT_EQ(pfcTx, txEnabled);
    };

    verifyAcrossWarmBoots(setup, verify);
  }

  // Verify PFC watchdog is not configured in HW
  void verifyPfcWatchdogNotConfigured() {
    auto currentConfig = initialConfig(*getAgentEnsemble());
    setupPfc(currentConfig, this->portIdsForTest()[0], true, true);
    cfg::PfcWatchdog defaultPfcWatchdogConfig{};

    XLOG(DBG0) << "Verify PFC watchdog is disabled by default on enabling PFC";
    auto portId = this->portIdsForTest()[0];
    EXPECT_TRUE(
        getAgentEnsemble()
            ->getHwAgentTestClient(getSwitchId(portId))
            ->sync_pfcWatchdogProgrammingMatchesConfig(
                static_cast<int32_t>(portId), false, defaultPfcWatchdogConfig));
  }

  // Setup and apply the new config with passed in PFC configurations
  void setupPfc(
      cfg::SwitchConfig& currentConfig,
      const PortID& portId,
      const bool pfcRxEnable,
      const bool pfcTxEnable) {
    // setup pfc
    addPfcConfig(currentConfig, {portId}, pfcRxEnable, pfcTxEnable);
    applyNewConfig(currentConfig);
  }

  // Removes PFC configuration for port and applies the config
  void removePfcConfig(cfg::SwitchConfig& currentConfig, const PortID& portId) {
    auto portCfg = utility::findCfgPort(currentConfig, portId);
    portCfg->pfc().reset();
    applyNewConfig(currentConfig);
  }

  // Run the various enabled/disabled combinations of PFC RX/TX
  void runPfcTest(bool rxEnabled, bool txEnabled) {
    auto setup = [=, this]() {
      auto currentConfig = initialConfig(*getAgentEnsemble());
      setupPfc(currentConfig, this->portIdsForTest()[0], rxEnabled, txEnabled);
    };

    auto verify = [=, this]() {
      auto portId = this->portIdsForTest()[0];
      bool pfcRx = getAgentEnsemble()
                       ->getHwAgentTestClient(getSwitchId(portId))
                       ->sync_getPfcEnabled(static_cast<int32_t>(portId), true);
      bool pfcTx =
          getAgentEnsemble()
              ->getHwAgentTestClient(getSwitchId(portId))
              ->sync_getPfcEnabled(static_cast<int32_t>(portId), false);

      EXPECT_EQ(pfcRx, rxEnabled);
      EXPECT_EQ(pfcTx, txEnabled);
    };

    verifyAcrossWarmBoots(setup, verify);
  }

  // Removes PFC configuration for port, but dont apply
  void removePfcConfigSkipApply(
      cfg::SwitchConfig& currentConfig,
      const PortID& portId) {
    auto portCfg = utility::findCfgPort(currentConfig, portId);
    portCfg->pfc().reset();
  }

  void setupPfcWdAndValidateProgramming(
      const PfcWdTestConfigs& wdTestCfg,
      const PortID& portId,
      cfg::SwitchConfig& switchConfig) {
    cfg::PfcWatchdog pfcWatchdogConfig{};
    XLOG(DBG0) << wdTestCfg.description;
    initalizePfcConfigWatchdogValues(
        pfcWatchdogConfig,
        wdTestCfg.detectionTimeMsecs,
        wdTestCfg.recoveryTimeMsecs,
        wdTestCfg.recoveryAction);
    setupPfcAndPfcWatchdog(switchConfig, portId, pfcWatchdogConfig);
    EXPECT_TRUE(
        getAgentEnsemble()
            ->getHwAgentTestClient(getSwitchId(portId))
            ->sync_pfcWatchdogProgrammingMatchesConfig(
                static_cast<int32_t>(portId), true, pfcWatchdogConfig));
  }

  std::vector<PfcWdTestConfigs> getPfcWdGranularityTestParam() {
    std::vector<PfcWdTestConfigs> wdParams;
    auto asicType = getAgentEnsemble()->getL3Asics().front()->getAsicType();
    if (asicType == cfg::AsicType::ASIC_TYPE_TOMAHAWK3 ||
        asicType == cfg::AsicType::ASIC_TYPE_TOMAHAWK4) {
      wdParams.push_back(
          {15,
           20,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer range 0-15msec"});
      wdParams.push_back(
          {16,
           20,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer range 16-159msec"});
      wdParams.push_back(
          {159,
           100,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer 10msec range boundary value 159msec"});
      wdParams.push_back(
          {160,
           600,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer 100msec range boundary value 160msec"});
      wdParams.push_back(
          {1599,
           1000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer 100msec range boundary value 1599msec"});
      wdParams.push_back(
          {1600,
           2000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer outside range with 1600msec"});
    } else if (
        getAgentEnsemble()->getL3Asics().front()->getAsicVendor() ==
        HwAsic::AsicVendor::ASIC_VENDOR_CHENAB) {
      // Chenab ASIC requires at minimum 200ms DLD/ 400ms DLR intervals
      wdParams.push_back(
          {200,
           400,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection boundary value 200ms, recovery boundary value 400ms"});
      wdParams.push_back(
          {1600,
           2000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer longer value"});
    } else if (
        asicType == cfg::AsicType::ASIC_TYPE_YUBA ||
        asicType == cfg::AsicType::ASIC_TYPE_G202X) {
      // YUBA supports a min timer value of 25msec and max of 10sec. In the
      // supported range, timer value as a multiple of 25msec is expected.
      wdParams.push_back(
          {25,
           100,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection value 25ms, recovery value 100ms"});
      wdParams.push_back(
          {25,
           1000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection value 25ms, recovery value 1000ms"});
      wdParams.push_back(
          {100,
           10000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection value 100ms, recovery value 10s"});
      // Production config
      wdParams.push_back(
          {200,
           1000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection value 200ms, recovery value 1000ms"});
    } else {
      // TODO: Param combinations for a granularity of 1msec
      wdParams.push_back(
          {10,
           100,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer 10/100 msecs"});
      wdParams.push_back(
          {100,
           500,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer 100/500 msecs"});
      wdParams.push_back(
          {800,
           1000,
           cfg::PfcWatchdogRecoveryAction::DROP,
           "Verify PFC watchdog deadlock detection timer 800/1000 msecs"});
    }
    return wdParams;
  }
};

TEST_F(AgentPfcConfigTest, PfcDefaultProgramming) {
  runPfcNotConfiguredTest(false, false);
}

TEST_F(AgentPfcConfigTest, PfcRxDisabledTxDisabled) {
  runPfcTest(false, false);
}

TEST_F(AgentPfcConfigTest, PfcRxEnabledTxDisabled) {
  runPfcTest(true, false);
}

TEST_F(AgentPfcConfigTest, PfcRxDisabledTxEnabled) {
  runPfcTest(false, true);
}

} // namespace facebook::fboss
