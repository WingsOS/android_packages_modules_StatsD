/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unordered_map>

#include "anomaly/AlarmMonitor.h"
#include "anomaly/AlarmTracker.h"
#include "anomaly/AnomalyTracker.h"
#include "condition/ConditionTracker.h"
#include "config/ConfigKey.h"
#include "external/StatsPullerManager.h"
#include "guardrail/StatsdStats.h"
#include "logd/LogEvent.h"
#include "matchers/AtomMatchingTracker.h"
#include "metrics/MetricProducer.h"
#include "packages/UidMap.h"
#include "src/statsd_config.pb.h"
#include "src/statsd_metadata.pb.h"

namespace android {
namespace os {
namespace statsd {

// A MetricsManager is responsible for managing metrics from one single config source.
class MetricsManager : public virtual RefBase, public virtual PullUidProvider {
public:
    MetricsManager(const ConfigKey& configKey, const StatsdConfig& config, int64_t timeBaseNs,
                   const int64_t currentTimeNs, const sp<UidMap>& uidMap,
                   const sp<StatsPullerManager>& pullerManager,
                   const sp<AlarmMonitor>& anomalyAlarmMonitor,
                   const sp<AlarmMonitor>& periodicAlarmMonitor);

    virtual ~MetricsManager();

    bool updateConfig(const StatsdConfig& config, int64_t timeBaseNs, const int64_t currentTimeNs,
                      const sp<AlarmMonitor>& anomalyAlarmMonitor,
                      const sp<AlarmMonitor>& periodicAlarmMonitor);

    // Return whether the configuration is valid.
    bool isConfigValid() const;

    bool checkLogCredentials(const LogEvent& event);

    virtual void onLogEvent(const LogEvent& event);

    void onAnomalyAlarmFired(
            int64_t timestampNs,
            unordered_set<sp<const InternalAlarm>, SpHash<InternalAlarm>>& alarmSet);

    void onPeriodicAlarmFired(
            int64_t timestampNs,
            unordered_set<sp<const InternalAlarm>, SpHash<InternalAlarm>>& alarmSet);

    void notifyAppUpgrade(int64_t eventTimeNs, const string& apk, int uid, int64_t version);

    void notifyAppRemoved(int64_t eventTimeNs, const string& apk, int uid);

    void onUidMapReceived(int64_t eventTimeNs);

    void onStatsdInitCompleted(int64_t elapsedTimeNs);

    void init();

    vector<int32_t> getPullAtomUids(int32_t atomId) override;

    bool shouldWriteToDisk() const {
        return mNoReportMetricIds.size() != mAllMetricProducers.size();
    }

    bool shouldPersistLocalHistory() const {
        return mShouldPersistHistory;
    }

    void dumpStates(int out, bool verbose);

    inline bool isInTtl(const int64_t timestampNs) const {
        return mTtlNs <= 0 || timestampNs < mTtlEndNs;
    };

    inline bool hashStringInReport() const {
        return mHashStringsInReport;
    };

    inline bool versionStringsInReport() const {
        return mVersionStringsInReport;
    };

    inline bool installerInReport() const {
        return mInstallerInReport;
    };

    inline uint8_t packageCertificateHashSizeBytes() const {
        return mPackageCertificateHashSizeBytes;
    }

    void refreshTtl(const int64_t currentTimestampNs) {
        if (mTtlNs > 0) {
            mTtlEndNs = currentTimestampNs + mTtlNs;
        }
    };

    // Returns the elapsed realtime when this metric manager last reported metrics. If this config
    // has not yet dumped any reports, this is the time the metricsmanager was initialized.
    inline int64_t getLastReportTimeNs() const {
        return mLastReportTimeNs;
    };

    inline int64_t getLastReportWallClockNs() const {
        return mLastReportWallClockNs;
    };

    inline size_t getNumMetrics() const {
        return mAllMetricProducers.size();
    }

    virtual void dropData(const int64_t dropTimeNs);

    virtual void onDumpReport(const int64_t dumpTimeNs, int64_t wallClockNs,
                              const bool include_current_partial_bucket, const bool erase_data,
                              const DumpLatency dumpLatency, std::set<string>* str_set,
                              android::util::ProtoOutputStream* protoOutput);

    // Computes the total byte size of all metrics managed by a single config source.
    // Does not change the state.
    virtual size_t byteSize();

    // Returns whether or not this config is active.
    // The config is active if any metric in the config is active.
    inline bool isActive() const {
        return mIsActive;
    }

    void loadActiveConfig(const ActiveConfig& config, int64_t currentTimeNs);

    void writeActiveConfigToProtoOutputStream(
            int64_t currentTimeNs, const DumpReportReason reason, ProtoOutputStream* proto);

    // Returns true if at least one piece of metadata is written.
    bool writeMetadataToProto(int64_t currentWallClockTimeNs,
                              int64_t systemElapsedTimeNs,
                              metadata::StatsMetadata* statsMetadata);

    void loadMetadata(const metadata::StatsMetadata& metadata,
                      int64_t currentWallClockTimeNs,
                      int64_t systemElapsedTimeNs);

    inline bool hasRestrictedMetricsDelegate() const {
        return mRestrictedMetricsDelegatePackageName.has_value();
    }

    inline string getRestrictedMetricsDelegate() const {
        return hasRestrictedMetricsDelegate() ? mRestrictedMetricsDelegatePackageName.value() : "";
    }

    inline ConfigKey getConfigKey() const {
        return mConfigKey;
    }

    void enforceRestrictedDataTtls(const int64_t wallClockNs);

    bool validateRestrictedMetricsDelegate(int32_t callingUid);

    virtual void flushRestrictedData();

    // Slow, should not be called in a hotpath.
    vector<int64_t> getAllMetricIds() const;

    // Adds all atom ids referenced by matchers in the MetricsManager's config
    void addAllAtomIds(LogEventFilter::AtomIdSet& allIds) const;

    // Gets the memory limit for the MetricsManager's config
    inline size_t getMaxMetricsBytes() const {
        return mMaxMetricsBytes;
    }

    inline size_t getTriggerGetDataBytes() const {
        return mTriggerGetDataBytes;
    }

private:
    // For test only.
    inline int64_t getTtlEndNs() const { return mTtlEndNs; }

    const ConfigKey mConfigKey;

    sp<UidMap> mUidMap;

    bool mHashStringsInReport = false;
    bool mVersionStringsInReport = false;
    bool mInstallerInReport = false;
    uint8_t mPackageCertificateHashSizeBytes;

    int64_t mTtlNs;
    int64_t mTtlEndNs;

    int64_t mLastReportTimeNs;
    int64_t mLastReportWallClockNs;

    optional<InvalidConfigReason> mInvalidConfigReason;

    sp<StatsPullerManager> mPullerManager;

    // The uid log sources from StatsdConfig.
    std::vector<int32_t> mAllowedUid;

    // The pkg log sources from StatsdConfig.
    std::vector<std::string> mAllowedPkg;

    // The combined uid sources (after translating pkg name to uid).
    // Logs from uids that are not in the list will be ignored to avoid spamming.
    std::set<int32_t> mAllowedLogSources;

    // To guard access to mAllowedLogSources
    mutable std::mutex mAllowedLogSourcesMutex;

    std::set<int32_t> mWhitelistedAtomIds;

    // We can pull any atom from these uids.
    std::set<int32_t> mDefaultPullUids;

    // Uids that specific atoms can pull from.
    // This is a map<atom id, set<uids>>
    std::map<int32_t, std::set<int32_t>> mPullAtomUids;

    // Packages that specific atoms can be pulled from.
    std::map<int32_t, std::set<std::string>> mPullAtomPackages;

    // All uids to pull for this atom. NOTE: Does not include the default uids for memory.
    std::map<int32_t, std::set<int32_t>> mCombinedPullAtomUids;

    // Contains the annotations passed in with StatsdConfig.
    std::list<std::pair<const int64_t, const int32_t>> mAnnotations;

    bool mShouldPersistHistory;

    // All event tags that are interesting to config metrics matchers.
    std::unordered_map<int, std::vector<int>> mTagIdsToMatchersMap;

    // We only store the sp of AtomMatchingTracker, MetricProducer, and ConditionTracker in
    // MetricsManager. There are relationships between them, and the relationships are denoted by
    // index instead of pointers. The reasons for this are: (1) the relationship between them are
    // complicated, so storing index instead of pointers reduces the risk that A holds B's sp, and B
    // holds A's sp. (2) When we evaluate matcher results, or condition results, we can quickly get
    // the related results from a cache using the index.

    // Hold all the atom matchers from the config.
    std::vector<sp<AtomMatchingTracker>> mAllAtomMatchingTrackers;

    // Hold all the conditions from the config.
    std::vector<sp<ConditionTracker>> mAllConditionTrackers;

    // Hold all metrics from the config.
    std::vector<sp<MetricProducer>> mAllMetricProducers;

    // Hold all alert trackers.
    std::vector<sp<AnomalyTracker>> mAllAnomalyTrackers;

    // Hold all periodic alarm trackers.
    std::vector<sp<AlarmTracker>> mAllPeriodicAlarmTrackers;

    // To make updating configs faster, we map the id of a AtomMatchingTracker, MetricProducer, and
    // ConditionTracker to its index in the corresponding vector.

    // Maps the id of an atom matching tracker to its index in mAllAtomMatchingTrackers.
    std::unordered_map<int64_t, int> mAtomMatchingTrackerMap;

    // Maps the id of a condition tracker to its index in mAllConditionTrackers.
    std::unordered_map<int64_t, int> mConditionTrackerMap;

    // Maps the id of a metric producer to its index in mAllMetricProducers.
    std::unordered_map<int64_t, int> mMetricProducerMap;

    // To make the log processing more efficient, we want to do as much filtering as possible
    // before we go into individual trackers and conditions to match.

    // 1st filter: check if the event tag id is in mTagIdsToMatchersMap.
    // 2nd filter: if it is, we parse the event because there is at least one member is interested.
    //             then pass to all AtomMatchingTrackers (itself also filter events by ids).
    // 3nd filter: for AtomMatchingTrackers that matched this event, we pass this event to the
    //             ConditionTrackers and MetricProducers that use this matcher.
    // 4th filter: for ConditionTrackers that changed value due to this event, we pass
    //             new conditions to  metrics that use this condition.

    // The following map is initialized from the statsd_config.

    // Maps from the index of the AtomMatchingTracker to index of MetricProducer.
    std::unordered_map<int, std::vector<int>> mTrackerToMetricMap;

    // Maps from AtomMatchingTracker to ConditionTracker
    std::unordered_map<int, std::vector<int>> mTrackerToConditionMap;

    // Maps from ConditionTracker to MetricProducer
    std::unordered_map<int, std::vector<int>> mConditionToMetricMap;

    // Maps from life span triggering event to MetricProducers.
    std::unordered_map<int, std::vector<int>> mActivationAtomTrackerToMetricMap;

    // Maps deactivation triggering event to MetricProducers.
    std::unordered_map<int, std::vector<int>> mDeactivationAtomTrackerToMetricMap;

    // Maps AlertIds to the index of the corresponding AnomalyTracker stored in mAllAnomalyTrackers.
    // The map is used in LoadMetadata to more efficiently lookup AnomalyTrackers from an AlertId.
    std::unordered_map<int64_t, int> mAlertTrackerMap;

    std::vector<int> mMetricIndexesWithActivation;

    void initAllowedLogSources();

    void initPullAtomSources();

    // Only called on config creation/update to initialize log sources from the config.
    // Calls initAllowedLogSources and initPullAtomSources. Sets up mInvalidConfigReason on
    // error.
    void createAllLogSourcesFromConfig(const StatsdConfig& config);

    // Verifies the config meets guardrails and updates statsdStats.
    // Sets up mInvalidConfigReason on error. Should be called on config creation/update
    void verifyGuardrailsAndUpdateStatsdStats();

    // Initializes mIsAlwaysActive and mIsActive.
    // Should be called on config creation/update.
    void initializeConfigActiveStatus();

    // The metrics that don't need to be uploaded or even reported.
    std::set<int64_t> mNoReportMetricIds;

   // The config is active if any metric in the config is active.
    bool mIsActive;

    // The config is always active if any metric in the config does not have an activation signal.
    bool mIsAlwaysActive;

    // Hashes of the States used in this config, keyed by the state id, used in config updates.
    std::map<int64_t, uint64_t> mStateProtoHashes;

    // Optional package name of the delegate that processes restricted metrics
    // If set, restricted metrics are only uploaded to the delegate.
    optional<string> mRestrictedMetricsDelegatePackageName = nullopt;

    // Only called on config creation/update. Sets the memory limit in bytes for storing metrics.
    void setMaxMetricsBytesFromConfig(const StatsdConfig& config);

    // Only called on config creation/update. Sets the soft memory limit in bytes for storing
    // metrics.
    void setTriggerGetDataBytesFromConfig(const StatsdConfig& config);

    // The memory limit in bytes for storing metrics
    size_t mMaxMetricsBytes;

    // The memory limit in bytes for triggering get data.
    size_t mTriggerGetDataBytes;

    FRIEND_TEST(MetricConditionLinkE2eTest, TestMultiplePredicatesAndLinks);
    FRIEND_TEST(AttributionE2eTest, TestAttributionMatchAndSliceByFirstUid);
    FRIEND_TEST(AttributionE2eTest, TestAttributionMatchAndSliceByChain);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestFirstNSamplesPulledNoTrigger);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestFirstNSamplesPulledNoTriggerWithActivation);
    FRIEND_TEST(GaugeMetricE2ePushedTest, TestMultipleFieldsForPushedEvent);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEvents);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEvent_LateAlarm);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEventsWithActivation);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEventsNoCondition);
    FRIEND_TEST(GaugeMetricE2ePulledTest, TestConditionChangeToTrueSamplePulledEvents);

    FRIEND_TEST(AnomalyCountDetectionE2eTest, TestSlicedCountMetric_single_bucket);
    FRIEND_TEST(AnomalyCountDetectionE2eTest, TestSlicedCountMetric_multiple_buckets);
    FRIEND_TEST(AnomalyCountDetectionE2eTest,
                TestCountMetric_save_refractory_to_disk_no_data_written);
    FRIEND_TEST(AnomalyCountDetectionE2eTest, TestCountMetric_save_refractory_to_disk);
    FRIEND_TEST(AnomalyCountDetectionE2eTest, TestCountMetric_load_refractory_from_disk);
    FRIEND_TEST(AnomalyDurationDetectionE2eTest, TestDurationMetric_SUM_single_bucket);
    FRIEND_TEST(AnomalyDurationDetectionE2eTest, TestDurationMetric_SUM_partial_bucket);
    FRIEND_TEST(AnomalyDurationDetectionE2eTest, TestDurationMetric_SUM_multiple_buckets);
    FRIEND_TEST(AnomalyDurationDetectionE2eTest, TestDurationMetric_SUM_long_refractory_period);

    FRIEND_TEST(AlarmE2eTest, TestMultipleAlarms);
    FRIEND_TEST(ConfigTtlE2eTest, TestCountMetric);
    FRIEND_TEST(ConfigUpdateE2eAbTest, TestConfigTtl);
    FRIEND_TEST(MetricActivationE2eTest, TestCountMetric);
    FRIEND_TEST(MetricActivationE2eTest, TestCountMetricWithOneDeactivation);
    FRIEND_TEST(MetricActivationE2eTest, TestCountMetricWithTwoDeactivations);
    FRIEND_TEST(MetricActivationE2eTest, TestCountMetricWithSameDeactivation);
    FRIEND_TEST(MetricActivationE2eTest, TestCountMetricWithTwoMetricsTwoDeactivations);

    FRIEND_TEST(MetricsManagerTest, TestLogSources);
    FRIEND_TEST(MetricsManagerTest, TestLogSourcesOnConfigUpdate);
    FRIEND_TEST(MetricsManagerTest_SPlus, TestRestrictedMetricsConfig);
    FRIEND_TEST(MetricsManagerTest_SPlus, TestRestrictedMetricsConfigUpdate);
    FRIEND_TEST(MetricsManagerUtilTest, TestSampledMetrics);

    FRIEND_TEST(StatsLogProcessorTest, TestActiveConfigMetricDiskWriteRead);
    FRIEND_TEST(StatsLogProcessorTest, TestActivationOnBoot);
    FRIEND_TEST(StatsLogProcessorTest, TestActivationOnBootMultipleActivations);
    FRIEND_TEST(StatsLogProcessorTest,
            TestActivationOnBootMultipleActivationsDifferentActivationTypes);
    FRIEND_TEST(StatsLogProcessorTest, TestActivationsPersistAcrossSystemServerRestart);

    FRIEND_TEST(CountMetricE2eTest, TestInitialConditionChanges);
    FRIEND_TEST(CountMetricE2eTest, TestSlicedState);
    FRIEND_TEST(CountMetricE2eTest, TestSlicedStateWithMap);
    FRIEND_TEST(CountMetricE2eTest, TestMultipleSlicedStates);
    FRIEND_TEST(CountMetricE2eTest, TestSlicedStateWithPrimaryFields);

    FRIEND_TEST(DurationMetricE2eTest, TestOneBucket);
    FRIEND_TEST(DurationMetricE2eTest, TestTwoBuckets);
    FRIEND_TEST(DurationMetricE2eTest, TestWithActivation);
    FRIEND_TEST(DurationMetricE2eTest, TestWithCondition);
    FRIEND_TEST(DurationMetricE2eTest, TestWithSlicedCondition);
    FRIEND_TEST(DurationMetricE2eTest, TestWithActivationAndSlicedCondition);
    FRIEND_TEST(DurationMetricE2eTest, TestWithSlicedState);
    FRIEND_TEST(DurationMetricE2eTest, TestWithConditionAndSlicedState);
    FRIEND_TEST(DurationMetricE2eTest, TestWithSlicedStateMapped);
    FRIEND_TEST(DurationMetricE2eTest, TestWithSlicedStatePrimaryFieldsSubset);
    FRIEND_TEST(DurationMetricE2eTest, TestUploadThreshold);

    FRIEND_TEST(ValueMetricE2eTest, TestInitialConditionChanges);
    FRIEND_TEST(ValueMetricE2eTest, TestPulledEvents);
    FRIEND_TEST(ValueMetricE2eTest, TestPulledEvents_LateAlarm);
    FRIEND_TEST(ValueMetricE2eTest, TestPulledEvents_WithActivation);
    FRIEND_TEST(ValueMetricE2eTest, TestInitWithSlicedState);
    FRIEND_TEST(ValueMetricE2eTest, TestInitWithSlicedState_WithDimensions);
    FRIEND_TEST(ValueMetricE2eTest, TestInitWithSlicedState_WithIncorrectDimensions);
    FRIEND_TEST(GaugeMetricE2ePushedTest, TestDimensionalSampling);
};

}  // namespace statsd
}  // namespace os
}  // namespace android
