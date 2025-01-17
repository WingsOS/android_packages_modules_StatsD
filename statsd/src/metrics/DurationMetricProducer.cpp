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

#define STATSD_DEBUG false

#include "Log.h"

#include "DurationMetricProducer.h"

#include <limits.h>
#include <stdlib.h>

#include "guardrail/StatsdStats.h"
#include "metrics/parsing_utils/metrics_manager_util.h"
#include "stats_log_util.h"
#include "stats_util.h"

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::FIELD_TYPE_STRING;
using android::util::ProtoOutputStream;
using std::string;
using std::unordered_map;
using std::vector;
using std::shared_ptr;

namespace android {
namespace os {
namespace statsd {

// for StatsLogReport
const int FIELD_ID_ID = 1;
const int FIELD_ID_DURATION_METRICS = 6;
const int FIELD_ID_TIME_BASE = 9;
const int FIELD_ID_BUCKET_SIZE = 10;
const int FIELD_ID_DIMENSION_PATH_IN_WHAT = 11;
const int FIELD_ID_IS_ACTIVE = 14;
const int FIELD_ID_DIMENSION_GUARDRAIL_HIT = 17;
// for DurationMetricDataWrapper
const int FIELD_ID_DATA = 1;
// for DurationMetricData
const int FIELD_ID_DIMENSION_IN_WHAT = 1;
const int FIELD_ID_BUCKET_INFO = 3;
const int FIELD_ID_DIMENSION_LEAF_IN_WHAT = 4;
const int FIELD_ID_SLICE_BY_STATE = 6;
// for DurationBucketInfo
const int FIELD_ID_DURATION = 3;
const int FIELD_ID_BUCKET_NUM = 4;
const int FIELD_ID_START_BUCKET_ELAPSED_MILLIS = 5;
const int FIELD_ID_END_BUCKET_ELAPSED_MILLIS = 6;
const int FIELD_ID_CONDITION_TRUE_NS = 7;

DurationMetricProducer::DurationMetricProducer(
        const ConfigKey& key, const DurationMetric& metric, const int conditionIndex,
        const vector<ConditionState>& initialConditionCache, const int whatIndex,
        const int startIndex, const int stopIndex, const int stopAllIndex, const bool nesting,
        const sp<ConditionWizard>& wizard, const uint64_t protoHash,
        const FieldMatcher& internalDimensions, const int64_t timeBaseNs, const int64_t startTimeNs,
        const unordered_map<int, shared_ptr<Activation>>& eventActivationMap,
        const unordered_map<int, vector<shared_ptr<Activation>>>& eventDeactivationMap,
        const vector<int>& slicedStateAtoms,
        const unordered_map<int, unordered_map<int, int64_t>>& stateGroupMap)
    : MetricProducer(metric.id(), key, timeBaseNs, conditionIndex, initialConditionCache, wizard,
                     protoHash, eventActivationMap, eventDeactivationMap, slicedStateAtoms,
                     stateGroupMap, getAppUpgradeBucketSplit(metric)),
      mAggregationType(metric.aggregation_type()),
      mStartIndex(startIndex),
      mStopIndex(stopIndex),
      mStopAllIndex(stopAllIndex),
      mNested(nesting),
      mContainANYPositionInInternalDimensions(false),
      mDimensionHardLimit(
              StatsdStats::clampDimensionKeySizeLimit(metric.max_dimensions_per_bucket())) {
    if (metric.has_bucket()) {
        mBucketSizeNs =
                TimeUnitToBucketSizeInMillisGuardrailed(key.GetUid(), metric.bucket()) * 1000000;
    } else {
        mBucketSizeNs = LLONG_MAX;
    }

    if (metric.has_threshold()) {
        mUploadThreshold = metric.threshold();
    }

    if (metric.has_dimensions_in_what()) {
        translateFieldMatcher(metric.dimensions_in_what(), &mDimensionsInWhat);
        mContainANYPositionInDimensionsInWhat = HasPositionANY(metric.dimensions_in_what());
    }

    if (internalDimensions.has_field()) {
        translateFieldMatcher(internalDimensions, &mInternalDimensions);
        mContainANYPositionInInternalDimensions = HasPositionANY(internalDimensions);
    }
    if (mContainANYPositionInInternalDimensions) {
        ALOGE("Position ANY in internal dimension not supported.");
    }
    if (mContainANYPositionInDimensionsInWhat) {
        ALOGE("Position ANY in dimension_in_what not supported.");
    }

    // Dimensions in what must be subset of internal dimensions
    if (!subsetDimensions(mDimensionsInWhat, mInternalDimensions)) {
        ALOGE("Dimensions in what must be a subset of the internal dimensions");
        // TODO: Add invalidConfigReason
        mValid = false;
    }

    mShouldUseNestedDimensions = ShouldUseNestedDimensions(metric.dimensions_in_what());

    if (metric.links().size() > 0) {
        for (const auto& link : metric.links()) {
            Metric2Condition mc;
            mc.conditionId = link.condition();
            translateFieldMatcher(link.fields_in_what(), &mc.metricFields);
            translateFieldMatcher(link.fields_in_condition(), &mc.conditionFields);
            if (!subsetDimensions(mc.metricFields, mInternalDimensions)) {
                ALOGE(("Condition links must be a subset of the internal dimensions"));
                // TODO: Add invalidConfigReason
                mValid = false;
            }
            mMetric2ConditionLinks.push_back(mc);
        }
        mConditionSliced = true;
    }
    mUnSlicedPartCondition = ConditionState::kUnknown;

    for (const auto& stateLink : metric.state_link()) {
        Metric2State ms;
        ms.stateAtomId = stateLink.state_atom_id();
        translateFieldMatcher(stateLink.fields_in_what(), &ms.metricFields);
        translateFieldMatcher(stateLink.fields_in_state(), &ms.stateFields);
        if (!subsetDimensions(ms.metricFields, mInternalDimensions)) {
            ALOGE(("State links must be a subset of the dimensions in what  internal dimensions"));
            // TODO: Add invalidConfigReason
            mValid = false;
        }
        mMetric2StateLinks.push_back(ms);
    }

    mUseWhatDimensionAsInternalDimension = equalDimensions(mDimensionsInWhat, mInternalDimensions);
    if (mWizard != nullptr && mConditionTrackerIndex >= 0 &&
            mMetric2ConditionLinks.size() == 1) {
        mHasLinksToAllConditionDimensionsInTracker = mWizard->equalOutputDimensions(
                mConditionTrackerIndex, mMetric2ConditionLinks.begin()->conditionFields);
    }
    flushIfNeededLocked(startTimeNs);
    // Adjust start for partial bucket
    mCurrentBucketStartTimeNs = startTimeNs;
    VLOG("metric %lld created. bucket size %lld start_time: %lld", (long long)mMetricId,
         (long long)mBucketSizeNs, (long long)mTimeBaseNs);

    initTrueDimensions(whatIndex, startTimeNs);
    mConditionTimer.newBucketStart(mCurrentBucketStartTimeNs, mCurrentBucketStartTimeNs);
    mConditionTimer.onConditionChanged(mIsActive && mCondition == ConditionState::kTrue,
                                       mCurrentBucketStartTimeNs);
}

DurationMetricProducer::~DurationMetricProducer() {
    VLOG("~DurationMetric() called");
}

optional<InvalidConfigReason> DurationMetricProducer::onConfigUpdatedLocked(
        const StatsdConfig& config, const int configIndex, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const sp<EventMatcherWizard>& matcherWizard,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& conditionTrackerMap, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation) {
    optional<InvalidConfigReason> invalidConfigReason = MetricProducer::onConfigUpdatedLocked(
            config, configIndex, metricIndex, allAtomMatchingTrackers, oldAtomMatchingTrackerMap,
            newAtomMatchingTrackerMap, matcherWizard, allConditionTrackers, conditionTrackerMap,
            wizard, metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
            activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
            metricsWithActivation);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    const DurationMetric& metric = config.duration_metric(configIndex);
    const auto& what_it = conditionTrackerMap.find(metric.what());
    if (what_it == conditionTrackerMap.end()) {
        ALOGE("DurationMetric's \"what\" is not present in the config");
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_DURATION_METRIC_WHAT_NOT_FOUND, mMetricId, metric.what());
    }

    const Predicate& durationWhat = config.predicate(what_it->second);
    if (durationWhat.contents_case() != Predicate::ContentsCase::kSimplePredicate) {
        ALOGE("DurationMetric's \"what\" must be a simple condition");
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_DURATION_METRIC_WHAT_NOT_SIMPLE, mMetricId, metric.what());
    }

    const SimplePredicate& simplePredicate = durationWhat.simple_predicate();

    // Update indices: mStartIndex, mStopIndex, mStopAllIndex, mConditionIndex and MetricsManager
    // maps.
    invalidConfigReason = handleMetricWithAtomMatchingTrackers(
            simplePredicate.start(), mMetricId, metricIndex, metric.has_dimensions_in_what(),
            allAtomMatchingTrackers, newAtomMatchingTrackerMap, trackerToMetricMap, mStartIndex);
    if (invalidConfigReason.has_value()) {
        ALOGE("Duration metrics must specify a valid start event matcher");
        return invalidConfigReason;
    }

    if (simplePredicate.has_stop()) {
        invalidConfigReason = handleMetricWithAtomMatchingTrackers(
                simplePredicate.stop(), mMetricId, metricIndex, metric.has_dimensions_in_what(),
                allAtomMatchingTrackers, newAtomMatchingTrackerMap, trackerToMetricMap, mStopIndex);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    if (simplePredicate.has_stop_all()) {
        invalidConfigReason = handleMetricWithAtomMatchingTrackers(
                simplePredicate.stop_all(), mMetricId, metricIndex, metric.has_dimensions_in_what(),
                allAtomMatchingTrackers, newAtomMatchingTrackerMap, trackerToMetricMap,
                mStopAllIndex);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    if (metric.has_condition()) {
        invalidConfigReason = handleMetricWithConditions(
                metric.condition(), mMetricId, metricIndex, conditionTrackerMap, metric.links(),
                allConditionTrackers, mConditionTrackerIndex, conditionToMetricMap);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    for (const auto& it : mCurrentSlicedDurationTrackerMap) {
        it.second->onConfigUpdated(wizard, mConditionTrackerIndex);
    }

    return nullopt;
}

void DurationMetricProducer::initTrueDimensions(const int whatIndex, const int64_t startTimeNs) {
    std::lock_guard<std::mutex> lock(mMutex);
    // Currently whatIndex will only be -1 in tests. In the future, we might want to avoid creating
    // a ConditionTracker if the condition is only used in the "what" of a duration metric. In that
    // scenario, -1 can also be passed.
    if (whatIndex == -1) {
        return;
    }
    const map<HashableDimensionKey, int>* slicedWhatMap = mWizard->getSlicedDimensionMap(whatIndex);
    for (const auto& [internalDimKey, count] : *slicedWhatMap) {
        for (int i = 0; i < count; i++) {
            // Fake start events.
            handleMatchedLogEventValuesLocked(mStartIndex, internalDimKey.getValues(), startTimeNs);
        }
    }
}

sp<AnomalyTracker> DurationMetricProducer::addAnomalyTracker(
        const Alert& alert, const sp<AlarmMonitor>& anomalyAlarmMonitor,
        const UpdateStatus& updateStatus, const int64_t updateTimeNs) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mAggregationType == DurationMetric_AggregationType_SUM) {
        if (alert.trigger_if_sum_gt() > alert.num_buckets() * mBucketSizeNs) {
            ALOGW("invalid alert for SUM: threshold (%f) > possible recordable value (%d x %lld)",
                  alert.trigger_if_sum_gt(), alert.num_buckets(), (long long)mBucketSizeNs);
            return nullptr;
        }
    }
    sp<AnomalyTracker> anomalyTracker =
            new DurationAnomalyTracker(alert, mConfigKey, anomalyAlarmMonitor);
    // The update status is either new or replaced.
    addAnomalyTrackerLocked(anomalyTracker, updateStatus, updateTimeNs);
    return anomalyTracker;
}

// Adds an AnomalyTracker that has already been created.
// Note: this gets called on config updates, and will only get called if the metric and the
// associated alert are preserved, which means the AnomalyTracker must be a DurationAnomalyTracker.
void DurationMetricProducer::addAnomalyTracker(sp<AnomalyTracker>& anomalyTracker,
                                               const int64_t updateTimeNs) {
    std::lock_guard<std::mutex> lock(mMutex);
    addAnomalyTrackerLocked(anomalyTracker, UpdateStatus::UPDATE_PRESERVE, updateTimeNs);
}

void DurationMetricProducer::addAnomalyTrackerLocked(sp<AnomalyTracker>& anomalyTracker,
                                                     const UpdateStatus& updateStatus,
                                                     const int64_t updateTimeNs) {
    mAnomalyTrackers.push_back(anomalyTracker);
    for (const auto& [_, durationTracker] : mCurrentSlicedDurationTrackerMap) {
        durationTracker->addAnomalyTracker(anomalyTracker, updateStatus, updateTimeNs);
    }
}
void DurationMetricProducer::onStateChanged(const int64_t eventTimeNs, const int32_t atomId,
                                            const HashableDimensionKey& primaryKey,
                                            const FieldValue& oldState,
                                            const FieldValue& newState) {
    // Check if this metric has a StateMap. If so, map the new state value to
    // the correct state group id.
    FieldValue newStateCopy = newState;
    mapStateValue(atomId, &newStateCopy);

    flushIfNeededLocked(eventTimeNs);

    // Each duration tracker is mapped to a different whatKey (a set of values from the
    // dimensionsInWhat fields). We notify all trackers iff the primaryKey field values from the
    // state change event are a subset of the tracker's whatKey field values.
    //
    // Ex. For a duration metric dimensioned on uid and tag:
    // DurationTracker1 whatKey = uid: 1001, tag: 1
    // DurationTracker2 whatKey = uid: 1002, tag 1
    //
    // If the state change primaryKey = uid: 1001, we only notify DurationTracker1 of a state
    // change.
    for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
        if (!containsLinkedStateValues(whatIt.first, primaryKey, mMetric2StateLinks, atomId)) {
            continue;
        }
        whatIt.second->onStateChanged(eventTimeNs, atomId, newStateCopy);
    }
}

unique_ptr<DurationTracker> DurationMetricProducer::createDurationTracker(
        const MetricDimensionKey& eventKey) const {
    switch (mAggregationType) {
        case DurationMetric_AggregationType_SUM:
            return make_unique<OringDurationTracker>(
                    mConfigKey, mMetricId, eventKey, mWizard, mConditionTrackerIndex, mNested,
                    mCurrentBucketStartTimeNs, mCurrentBucketNum, mTimeBaseNs, mBucketSizeNs,
                    mConditionSliced, mHasLinksToAllConditionDimensionsInTracker, mAnomalyTrackers);
        case DurationMetric_AggregationType_MAX_SPARSE:
            return make_unique<MaxDurationTracker>(
                    mConfigKey, mMetricId, eventKey, mWizard, mConditionTrackerIndex, mNested,
                    mCurrentBucketStartTimeNs, mCurrentBucketNum, mTimeBaseNs, mBucketSizeNs,
                    mConditionSliced, mHasLinksToAllConditionDimensionsInTracker, mAnomalyTrackers);
    }
}

// SlicedConditionChange optimization case 1:
// 1. If combination condition, logical operation is AND, only one sliced child predicate.
// 2. The links covers all dimension fields in the sliced child condition predicate.
void DurationMetricProducer::onSlicedConditionMayChangeLocked_opt1(const int64_t eventTime) {
    if (mMetric2ConditionLinks.size() != 1 ||
        !mHasLinksToAllConditionDimensionsInTracker) {
        return;
    }

    bool  currentUnSlicedPartCondition = true;
    if (!mWizard->IsSimpleCondition(mConditionTrackerIndex)) {
        ConditionState unslicedPartState =
            mWizard->getUnSlicedPartConditionState(mConditionTrackerIndex);
        // When the unsliced part is still false, return directly.
        if (mUnSlicedPartCondition == ConditionState::kFalse &&
            unslicedPartState == ConditionState::kFalse) {
            return;
        }
        mUnSlicedPartCondition = unslicedPartState;
        currentUnSlicedPartCondition = mUnSlicedPartCondition > 0;
    }

    auto dimensionsChangedToTrue = mWizard->getChangedToTrueDimensions(mConditionTrackerIndex);
    auto dimensionsChangedToFalse = mWizard->getChangedToFalseDimensions(mConditionTrackerIndex);

    // The condition change is from the unsliced predicates.
    // We need to find out the true dimensions from the sliced predicate and flip their condition
    // state based on the new unsliced condition state.
    if (dimensionsChangedToTrue == nullptr || dimensionsChangedToFalse == nullptr ||
        (dimensionsChangedToTrue->empty() && dimensionsChangedToFalse->empty())) {
        const map<HashableDimensionKey, int>* slicedConditionMap =
                mWizard->getSlicedDimensionMap(mConditionTrackerIndex);
        for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
            HashableDimensionKey linkedConditionDimensionKey;
            getDimensionForCondition(whatIt.first.getValues(), mMetric2ConditionLinks[0],
                                     &linkedConditionDimensionKey);
            const auto& slicedConditionIt = slicedConditionMap->find(linkedConditionDimensionKey);
            if (slicedConditionIt != slicedConditionMap->end() && slicedConditionIt->second > 0) {
                whatIt.second->onConditionChanged(currentUnSlicedPartCondition, eventTime);
            }
        }
    } else {
        // Handle the condition change from the sliced predicate.
        if (currentUnSlicedPartCondition) {
            for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
                HashableDimensionKey linkedConditionDimensionKey;
                getDimensionForCondition(whatIt.first.getValues(), mMetric2ConditionLinks[0],
                                         &linkedConditionDimensionKey);
                if (dimensionsChangedToTrue->find(linkedConditionDimensionKey) !=
                        dimensionsChangedToTrue->end()) {
                    whatIt.second->onConditionChanged(true, eventTime);
                }
                if (dimensionsChangedToFalse->find(linkedConditionDimensionKey) !=
                        dimensionsChangedToFalse->end()) {
                    whatIt.second->onConditionChanged(false, eventTime);
                }
            }
        }
    }
}

void DurationMetricProducer::onSlicedConditionMayChangeInternalLocked(const int64_t eventTimeNs) {
    bool changeDimTrackable = mWizard->IsChangedDimensionTrackable(mConditionTrackerIndex);
    if (changeDimTrackable && mHasLinksToAllConditionDimensionsInTracker) {
        onSlicedConditionMayChangeLocked_opt1(eventTimeNs);
        return;
    }

    // Now for each of the on-going event, check if the condition has changed for them.
    for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
        whatIt.second->onSlicedConditionMayChange(eventTimeNs);
    }
}

void DurationMetricProducer::onSlicedConditionMayChangeLocked(bool overallCondition,
                                                              const int64_t eventTime) {
    VLOG("Metric %lld onSlicedConditionMayChange", (long long)mMetricId);

    if (!mIsActive) {
        return;
    }

    flushIfNeededLocked(eventTime);

    if (!mConditionSliced) {
        return;
    }

    onSlicedConditionMayChangeInternalLocked(eventTime);
}

void DurationMetricProducer::onActiveStateChangedLocked(const int64_t eventTimeNs,
                                                        const bool isActive) {
    MetricProducer::onActiveStateChangedLocked(eventTimeNs, isActive);

    if (!mConditionSliced) {
        if (ConditionState::kTrue != mCondition) {
            return;
        }

        if (isActive) {
            flushIfNeededLocked(eventTimeNs);
        }

        for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
            whatIt.second->onConditionChanged(isActive, eventTimeNs);
        }
        mConditionTimer.onConditionChanged(isActive, eventTimeNs);
    } else if (isActive) {
        flushIfNeededLocked(eventTimeNs);
        onSlicedConditionMayChangeInternalLocked(eventTimeNs);
    } else {  // mConditionSliced == true && !isActive
        for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
            whatIt.second->onConditionChanged(isActive, eventTimeNs);
        }
    }
}

void DurationMetricProducer::onConditionChangedLocked(const bool conditionMet,
                                                      const int64_t eventTime) {
    VLOG("Metric %lld onConditionChanged", (long long)mMetricId);
    mCondition = conditionMet ? ConditionState::kTrue : ConditionState::kFalse;

    if (!mIsActive) {
        return;
    }

    flushIfNeededLocked(eventTime);
    for (auto& whatIt : mCurrentSlicedDurationTrackerMap) {
        whatIt.second->onConditionChanged(conditionMet, eventTime);
    }

    mConditionTimer.onConditionChanged(mCondition, eventTime);
}

void DurationMetricProducer::dropDataLocked(const int64_t dropTimeNs) {
    flushIfNeededLocked(dropTimeNs);
    StatsdStats::getInstance().noteBucketDropped(mMetricId);
    mPastBuckets.clear();
}

void DurationMetricProducer::clearPastBucketsLocked(const int64_t dumpTimeNs) {
    flushIfNeededLocked(dumpTimeNs);
    mPastBuckets.clear();
}

void DurationMetricProducer::onDumpReportLocked(
        const int64_t dumpTimeNs, const bool include_current_partial_bucket, const bool erase_data,
        const DumpLatency dumpLatency, std::set<string>* str_set, ProtoOutputStream* protoOutput) {
    if (include_current_partial_bucket) {
        flushLocked(dumpTimeNs);
    } else {
        flushIfNeededLocked(dumpTimeNs);
    }

    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_ID, (long long)mMetricId);
    protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_IS_ACTIVE, isActiveLocked());

    if (mPastBuckets.empty()) {
        VLOG(" Duration metric, empty return");
        return;
    }

    if (StatsdStats::getInstance().hasHitDimensionGuardrail(mMetricId)) {
        protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_DIMENSION_GUARDRAIL_HIT, true);
    }

    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_TIME_BASE, (long long)mTimeBaseNs);
    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_BUCKET_SIZE, (long long)mBucketSizeNs);

    if (!mShouldUseNestedDimensions) {
        if (!mDimensionsInWhat.empty()) {
            uint64_t dimenPathToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_ID_DIMENSION_PATH_IN_WHAT);
            writeDimensionPathToProto(mDimensionsInWhat, protoOutput);
            protoOutput->end(dimenPathToken);
        }
    }

    uint64_t protoToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_DURATION_METRICS);

    VLOG("Duration metric %lld dump report now...", (long long)mMetricId);

    for (const auto& pair : mPastBuckets) {
        const MetricDimensionKey& dimensionKey = pair.first;
        VLOG("  dimension key %s", dimensionKey.toString().c_str());

        uint64_t wrapperToken =
                protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DATA);

        // First fill dimension.
        if (mShouldUseNestedDimensions) {
            uint64_t dimensionToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_ID_DIMENSION_IN_WHAT);
            writeDimensionToProto(dimensionKey.getDimensionKeyInWhat(), str_set, protoOutput);
            protoOutput->end(dimensionToken);
        } else {
            writeDimensionLeafNodesToProto(dimensionKey.getDimensionKeyInWhat(),
                                           FIELD_ID_DIMENSION_LEAF_IN_WHAT, str_set, protoOutput);
        }
        // Then fill slice_by_state.
        for (auto state : dimensionKey.getStateValuesKey().getValues()) {
            uint64_t stateToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED |
                                                     FIELD_ID_SLICE_BY_STATE);
            writeStateToProto(state, protoOutput);
            protoOutput->end(stateToken);
        }
        // Then fill bucket_info (DurationBucketInfo).
        for (const auto& bucket : pair.second) {
            uint64_t bucketInfoToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_BUCKET_INFO);
            if (bucket.mBucketEndNs - bucket.mBucketStartNs != mBucketSizeNs) {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_START_BUCKET_ELAPSED_MILLIS,
                                   (long long)NanoToMillis(bucket.mBucketStartNs));
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_END_BUCKET_ELAPSED_MILLIS,
                                   (long long)NanoToMillis(bucket.mBucketEndNs));
            } else {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_BUCKET_NUM,
                                   (long long)(getBucketNumFromEndTimeNs(bucket.mBucketEndNs)));
            }
            protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_DURATION, (long long)bucket.mDuration);

            // We only write the condition timer value if the metric has a
            // condition and isn't sliced by state or condition.
            // TODO(b/268531762): Slice the condition timer by state and condition
            if (mConditionTrackerIndex >= 0 && mSlicedStateAtoms.empty() && !mConditionSliced) {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_CONDITION_TRUE_NS,
                                   (long long)bucket.mConditionTrueNs);
            }

            protoOutput->end(bucketInfoToken);
            VLOG("\t bucket [%lld - %lld] duration: %lld", (long long)bucket.mBucketStartNs,
                 (long long)bucket.mBucketEndNs, (long long)bucket.mDuration);
        }

        protoOutput->end(wrapperToken);
    }

    protoOutput->end(protoToken);
    if (erase_data) {
        mPastBuckets.clear();
    }
}

void DurationMetricProducer::flushIfNeededLocked(const int64_t eventTimeNs) {
    int64_t currentBucketEndTimeNs = getCurrentBucketEndTimeNs();

    if (currentBucketEndTimeNs > eventTimeNs) {
        return;
    }
    VLOG("flushing...........");
    int numBucketsForward = 1 + (eventTimeNs - currentBucketEndTimeNs) / mBucketSizeNs;
    int64_t nextBucketNs = currentBucketEndTimeNs + (numBucketsForward - 1) * mBucketSizeNs;
    flushCurrentBucketLocked(eventTimeNs, nextBucketNs);

    mCurrentBucketNum += numBucketsForward;
}

void DurationMetricProducer::flushCurrentBucketLocked(const int64_t eventTimeNs,
                                                      const int64_t nextBucketStartTimeNs) {
    const auto [globalConditionTrueNs, globalConditionCorrectionNs] =
            mConditionTimer.newBucketStart(eventTimeNs, nextBucketStartTimeNs);

    for (auto whatIt = mCurrentSlicedDurationTrackerMap.begin();
            whatIt != mCurrentSlicedDurationTrackerMap.end();) {
        if (whatIt->second->flushCurrentBucket(eventTimeNs, mUploadThreshold, globalConditionTrueNs,
                                               &mPastBuckets)) {
            VLOG("erase bucket for key %s", whatIt->first.toString().c_str());
            whatIt = mCurrentSlicedDurationTrackerMap.erase(whatIt);
        } else {
            ++whatIt;
        }
    }

    StatsdStats::getInstance().noteBucketCount(mMetricId);
    mCurrentBucketStartTimeNs = nextBucketStartTimeNs;
    // Reset mHasHitGuardrail boolean since bucket was reset
    mHasHitGuardrail = false;
}

void DurationMetricProducer::dumpStatesLocked(int out, bool verbose) const {
    if (mCurrentSlicedDurationTrackerMap.size() == 0) {
        return;
    }

    dprintf(out, "DurationMetric %lld dimension size %lu\n", (long long)mMetricId,
            (unsigned long)mCurrentSlicedDurationTrackerMap.size());
    if (verbose) {
        for (const auto& whatIt : mCurrentSlicedDurationTrackerMap) {
            dprintf(out, "\t(what)%s\n", whatIt.first.toString().c_str());
            whatIt.second->dumpStates(out, verbose);
        }
    }
}

bool DurationMetricProducer::hitGuardRailLocked(const MetricDimensionKey& newKey) const {
    auto whatIt = mCurrentSlicedDurationTrackerMap.find(newKey.getDimensionKeyInWhat());
    if (whatIt == mCurrentSlicedDurationTrackerMap.end()) {
        // 1. Report the tuple count if the tuple count > soft limit
        if (mCurrentSlicedDurationTrackerMap.size() >= StatsdStats::kDimensionKeySizeSoftLimit) {
            size_t newTupleCount = mCurrentSlicedDurationTrackerMap.size() + 1;
            StatsdStats::getInstance().noteMetricDimensionSize(
                    mConfigKey, mMetricId, newTupleCount);
            // 2. Don't add more tuples, we are above the allowed threshold. Drop the data.
            if (newTupleCount > mDimensionHardLimit) {
                if (!mHasHitGuardrail) {
                    ALOGE("DurationMetric %lld dropping data for what dimension key %s",
                          (long long)mMetricId, newKey.getDimensionKeyInWhat().toString().c_str());
                    mHasHitGuardrail = true;
                }
                StatsdStats::getInstance().noteHardDimensionLimitReached(mMetricId);
                return true;
            }
        }
    }
    return false;
}

void DurationMetricProducer::handleStartEvent(const MetricDimensionKey& eventKey,
                                              const ConditionKey& conditionKeys, bool condition,
                                              const int64_t eventTimeNs,
                                              const vector<FieldValue>& eventValues) {
    const auto& whatKey = eventKey.getDimensionKeyInWhat();
    auto whatIt = mCurrentSlicedDurationTrackerMap.find(whatKey);
    if (whatIt == mCurrentSlicedDurationTrackerMap.end()) {
        if (hitGuardRailLocked(eventKey)) {
            return;
        }
        mCurrentSlicedDurationTrackerMap[whatKey] = createDurationTracker(eventKey);
    }

    auto it = mCurrentSlicedDurationTrackerMap.find(whatKey);
    if (mUseWhatDimensionAsInternalDimension) {
        it->second->noteStart(whatKey, condition, eventTimeNs, conditionKeys, mDimensionHardLimit);
        return;
    }

    if (mInternalDimensions.empty()) {
        it->second->noteStart(DEFAULT_DIMENSION_KEY, condition, eventTimeNs, conditionKeys,
                              mDimensionHardLimit);
    } else {
        HashableDimensionKey dimensionKey = DEFAULT_DIMENSION_KEY;
        filterValues(mInternalDimensions, eventValues, &dimensionKey);
        it->second->noteStart(dimensionKey, condition, eventTimeNs, conditionKeys,
                              mDimensionHardLimit);
    }
}

void DurationMetricProducer::onMatchedLogEventInternalLocked(
        const size_t matcherIndex, const MetricDimensionKey& eventKey,
        const ConditionKey& conditionKeys, bool condition, const LogEvent& event,
        const map<int, HashableDimensionKey>& statePrimaryKeys) {
    ALOGW("Not used in duration tracker.");
}

void DurationMetricProducer::onMatchedLogEventLocked(const size_t matcherIndex,
                                                     const LogEvent& event) {
    handleMatchedLogEventValuesLocked(matcherIndex, event.getValues(),
                                      event.GetElapsedTimestampNs());
}

void DurationMetricProducer::handleMatchedLogEventValuesLocked(const size_t matcherIndex,
                                                               const vector<FieldValue>& values,
                                                               const int64_t eventTimeNs) {
    if (eventTimeNs < mTimeBaseNs) {
        return;
    }

    if (mIsActive) {
        flushIfNeededLocked(eventTimeNs);
    }

    // Handles Stopall events.
    if ((int)matcherIndex == mStopAllIndex) {
        for (auto whatIt = mCurrentSlicedDurationTrackerMap.begin();
             whatIt != mCurrentSlicedDurationTrackerMap.end();) {
            whatIt->second->noteStopAll(eventTimeNs);
            if (!whatIt->second->hasAccumulatedDuration()) {
                VLOG("erase bucket for key %s", whatIt->first.toString().c_str());
                whatIt = mCurrentSlicedDurationTrackerMap.erase(whatIt);
            } else {
                whatIt++;
            }
        }
        return;
    }

    if (!passesSampleCheckLocked(values)) {
        return;
    }

    HashableDimensionKey dimensionInWhat = DEFAULT_DIMENSION_KEY;
    if (!mDimensionsInWhat.empty()) {
        filterValues(mDimensionsInWhat, values, &dimensionInWhat);
    }

    // Stores atom id to primary key pairs for each state atom that the metric is
    // sliced by.
    std::map<int, HashableDimensionKey> statePrimaryKeys;

    // For states with primary fields, use MetricStateLinks to get the primary
    // field values from the log event. These values will form a primary key
    // that will be used to query StateTracker for the correct state value.
    for (const auto& stateLink : mMetric2StateLinks) {
        getDimensionForState(values, stateLink, &statePrimaryKeys[stateLink.stateAtomId]);
    }

    // For each sliced state, query StateTracker for the state value using
    // either the primary key from the previous step or the DEFAULT_DIMENSION_KEY.
    //
    // Expected functionality: for any case where the MetricStateLinks are
    // initialized incorrectly (ex. # of state links != # of primary fields, no
    // links are provided for a state with primary fields, links are provided
    // in the wrong order, etc.), StateTracker will simply return kStateUnknown
    // when queried using an incorrect key.
    HashableDimensionKey stateValuesKey = DEFAULT_DIMENSION_KEY;
    for (auto atomId : mSlicedStateAtoms) {
        FieldValue value;
        if (statePrimaryKeys.find(atomId) != statePrimaryKeys.end()) {
            // found a primary key for this state, query using the key
            queryStateValue(atomId, statePrimaryKeys[atomId], &value);
        } else {
            // if no MetricStateLinks exist for this state atom,
            // query using the default dimension key (empty HashableDimensionKey)
            queryStateValue(atomId, DEFAULT_DIMENSION_KEY, &value);
        }
        mapStateValue(atomId, &value);
        stateValuesKey.addValue(value);
    }

    // Handles Stop events.
    if ((int)matcherIndex == mStopIndex) {
        if (mUseWhatDimensionAsInternalDimension) {
            auto whatIt = mCurrentSlicedDurationTrackerMap.find(dimensionInWhat);
            if (whatIt != mCurrentSlicedDurationTrackerMap.end()) {
                whatIt->second->noteStop(dimensionInWhat, eventTimeNs, false);
                if (!whatIt->second->hasAccumulatedDuration()) {
                    VLOG("erase bucket for key %s", whatIt->first.toString().c_str());
                    mCurrentSlicedDurationTrackerMap.erase(whatIt);
                }
            }
            return;
        }

        HashableDimensionKey internalDimensionKey = DEFAULT_DIMENSION_KEY;
        if (!mInternalDimensions.empty()) {
            filterValues(mInternalDimensions, values, &internalDimensionKey);
        }

        auto whatIt = mCurrentSlicedDurationTrackerMap.find(dimensionInWhat);
        if (whatIt != mCurrentSlicedDurationTrackerMap.end()) {
            whatIt->second->noteStop(internalDimensionKey, eventTimeNs, false);
            if (!whatIt->second->hasAccumulatedDuration()) {
                VLOG("erase bucket for key %s", whatIt->first.toString().c_str());
                mCurrentSlicedDurationTrackerMap.erase(whatIt);
            }
        }
        return;
    }

    bool condition;
    ConditionKey conditionKey;
    if (mConditionSliced) {
        for (const auto& link : mMetric2ConditionLinks) {
            getDimensionForCondition(values, link, &conditionKey[link.conditionId]);
        }

        auto conditionState =
            mWizard->query(mConditionTrackerIndex, conditionKey,
                           !mHasLinksToAllConditionDimensionsInTracker);
        condition = conditionState == ConditionState::kTrue;
    } else {
        // TODO: The unknown condition state is not handled here, we should fix it.
        condition = mCondition == ConditionState::kTrue;
    }

    condition = condition && mIsActive;

    handleStartEvent(MetricDimensionKey(dimensionInWhat, stateValuesKey), conditionKey, condition,
                     eventTimeNs, values);
}

size_t DurationMetricProducer::byteSizeLocked() const {
    size_t totalSize = 0;
    for (const auto& pair : mPastBuckets) {
        totalSize += pair.second.size() * kBucketSize;
    }
    return totalSize;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
