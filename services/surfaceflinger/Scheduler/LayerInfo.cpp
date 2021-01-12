/*
 * Copyright 2020 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"

// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "LayerInfo.h"

#include <algorithm>
#include <utility>

#include <cutils/compiler.h>
#include <cutils/trace.h>

#undef LOG_TAG
#define LOG_TAG "LayerInfo"

namespace android::scheduler {

const RefreshRateConfigs* LayerInfo::sRefreshRateConfigs = nullptr;
bool LayerInfo::sTraceEnabled = false;

LayerInfo::LayerInfo(const std::string& name, LayerHistory::LayerVoteType defaultVote)
      : mName(name),
        mDefaultVote(defaultVote),
        mLayerVote({defaultVote, Fps(0.0f)}),
        mRefreshRateHistory(name) {}

void LayerInfo::setLastPresentTime(nsecs_t lastPresentTime, nsecs_t now, LayerUpdateType updateType,
                                   bool pendingConfigChange) {
    lastPresentTime = std::max(lastPresentTime, static_cast<nsecs_t>(0));

    mLastUpdatedTime = std::max(lastPresentTime, now);
    switch (updateType) {
        case LayerUpdateType::AnimationTX:
            mLastAnimationTime = std::max(lastPresentTime, now);
            break;
        case LayerUpdateType::SetFrameRate:
        case LayerUpdateType::Buffer:
            FrameTimeData frameTime = {.presetTime = lastPresentTime,
                                       .queueTime = mLastUpdatedTime,
                                       .pendingConfigChange = pendingConfigChange};
            mFrameTimes.push_back(frameTime);
            if (mFrameTimes.size() > HISTORY_SIZE) {
                mFrameTimes.pop_front();
            }
            break;
    }
}

bool LayerInfo::isFrameTimeValid(const FrameTimeData& frameTime) const {
    return frameTime.queueTime >= std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          mFrameTimeValidSince.time_since_epoch())
                                          .count();
}

bool LayerInfo::isFrequent(nsecs_t now) const {
    // If we know nothing about this layer we consider it as frequent as it might be the start
    // of an animation.
    if (mFrameTimes.size() < FREQUENT_LAYER_WINDOW_SIZE) {
        return true;
    }

    // Find the first active frame
    auto it = mFrameTimes.begin();
    for (; it != mFrameTimes.end(); ++it) {
        if (it->queueTime >= getActiveLayerThreshold(now)) {
            break;
        }
    }

    const auto numFrames = std::distance(it, mFrameTimes.end());
    if (numFrames < FREQUENT_LAYER_WINDOW_SIZE) {
        return false;
    }

    // Layer is considered frequent if the average frame rate is higher than the threshold
    const auto totalTime = mFrameTimes.back().queueTime - it->queueTime;
    return Fps::fromPeriodNsecs(totalTime / (numFrames - 1))
            .greaterThanOrEqualWithMargin(MIN_FPS_FOR_FREQUENT_LAYER);
}

bool LayerInfo::isAnimating(nsecs_t now) const {
    return mLastAnimationTime >= getActiveLayerThreshold(now);
}

bool LayerInfo::hasEnoughDataForHeuristic() const {
    // The layer had to publish at least HISTORY_SIZE or HISTORY_DURATION of updates
    if (mFrameTimes.size() < 2) {
        ALOGV("fewer than 2 frames recorded: %zu", mFrameTimes.size());
        return false;
    }

    if (!isFrameTimeValid(mFrameTimes.front())) {
        ALOGV("stale frames still captured");
        return false;
    }

    const auto totalDuration = mFrameTimes.back().queueTime - mFrameTimes.front().queueTime;
    if (mFrameTimes.size() < HISTORY_SIZE && totalDuration < HISTORY_DURATION.count()) {
        ALOGV("not enough frames captured: %zu | %.2f seconds", mFrameTimes.size(),
              totalDuration / 1e9f);
        return false;
    }

    return true;
}

std::optional<nsecs_t> LayerInfo::calculateAverageFrameTime() const {
    nsecs_t totalPresentTimeDeltas = 0;
    nsecs_t totalQueueTimeDeltas = 0;
    bool missingPresentTime = false;
    int numFrames = 0;
    for (auto it = mFrameTimes.begin(); it != mFrameTimes.end() - 1; ++it) {
        // Ignore frames captured during a config change
        if (it->pendingConfigChange || (it + 1)->pendingConfigChange) {
            return std::nullopt;
        }

        totalQueueTimeDeltas +=
                std::max(((it + 1)->queueTime - it->queueTime), kMinPeriodBetweenFrames);
        numFrames++;

        if (!missingPresentTime && (it->presetTime == 0 || (it + 1)->presetTime == 0)) {
            missingPresentTime = true;
            // If there are no presentation timestamps and we haven't calculated
            // one in the past then we can't calculate the refresh rate
            if (!mLastRefreshRate.reported.isValid()) {
                return std::nullopt;
            }
            continue;
        }

        totalPresentTimeDeltas +=
                std::max(((it + 1)->presetTime - it->presetTime), kMinPeriodBetweenFrames);
    }

    // Calculate the average frame time based on presentation timestamps. If those
    // doesn't exist, we look at the time the buffer was queued only. We can do that only if
    // we calculated a refresh rate based on presentation timestamps in the past. The reason
    // we look at the queue time is to handle cases where hwui attaches presentation timestamps
    // when implementing render ahead for specific refresh rates. When hwui no longer provides
    // presentation timestamps we look at the queue time to see if the current refresh rate still
    // matches the content.

    const auto averageFrameTime =
            static_cast<float>(missingPresentTime ? totalQueueTimeDeltas : totalPresentTimeDeltas) /
            numFrames;
    return static_cast<nsecs_t>(averageFrameTime);
}

std::optional<Fps> LayerInfo::calculateRefreshRateIfPossible(nsecs_t now) {
    static constexpr float MARGIN = 1.0f; // 1Hz
    if (!hasEnoughDataForHeuristic()) {
        ALOGV("Not enough data");
        return std::nullopt;
    }

    const auto averageFrameTime = calculateAverageFrameTime();
    if (averageFrameTime.has_value()) {
        const auto refreshRate = Fps::fromPeriodNsecs(*averageFrameTime);
        const bool refreshRateConsistent = mRefreshRateHistory.add(refreshRate, now);
        if (refreshRateConsistent) {
            const auto knownRefreshRate =
                    sRefreshRateConfigs->findClosestKnownFrameRate(refreshRate);

            // To avoid oscillation, use the last calculated refresh rate if it is
            // close enough
            if (std::abs(mLastRefreshRate.calculated.getValue() - refreshRate.getValue()) >
                        MARGIN &&
                !mLastRefreshRate.reported.equalsWithMargin(knownRefreshRate)) {
                mLastRefreshRate.calculated = refreshRate;
                mLastRefreshRate.reported = knownRefreshRate;
            }

            ALOGV("%s %s rounded to nearest known frame rate %s", mName.c_str(),
                  to_string(refreshRate).c_str(), to_string(mLastRefreshRate.reported).c_str());
        } else {
            ALOGV("%s Not stable (%s) returning last known frame rate %s", mName.c_str(),
                  to_string(refreshRate).c_str(), to_string(mLastRefreshRate.reported).c_str());
        }
    }

    return mLastRefreshRate.reported.isValid() ? std::make_optional(mLastRefreshRate.reported)
                                               : std::nullopt;
}

LayerInfo::LayerVote LayerInfo::getRefreshRateVote(nsecs_t now) {
    if (mLayerVote.type != LayerHistory::LayerVoteType::Heuristic) {
        ALOGV("%s voted %d ", mName.c_str(), static_cast<int>(mLayerVote.type));
        return mLayerVote;
    }

    if (isAnimating(now)) {
        ALOGV("%s is animating", mName.c_str());
        mLastRefreshRate.animatingOrInfrequent = true;
        return {LayerHistory::LayerVoteType::Max, Fps(0.0f)};
    }

    if (!isFrequent(now)) {
        ALOGV("%s is infrequent", mName.c_str());
        mLastRefreshRate.animatingOrInfrequent = true;
        return {LayerHistory::LayerVoteType::Min, Fps(0.0f)};
    }

    // If the layer was previously tagged as animating or infrequent, we clear
    // the history as it is likely the layer just changed its behavior
    // and we should not look at stale data
    if (mLastRefreshRate.animatingOrInfrequent) {
        clearHistory(now);
    }

    auto refreshRate = calculateRefreshRateIfPossible(now);
    if (refreshRate.has_value()) {
        ALOGV("%s calculated refresh rate: %s", mName.c_str(), to_string(*refreshRate).c_str());
        return {LayerHistory::LayerVoteType::Heuristic, refreshRate.value()};
    }

    ALOGV("%s Max (can't resolve refresh rate)", mName.c_str());
    return {LayerHistory::LayerVoteType::Max, Fps(0.0f)};
}

const char* LayerInfo::getTraceTag(android::scheduler::LayerHistory::LayerVoteType type) const {
    if (mTraceTags.count(type) == 0) {
        const auto tag = "LFPS " + mName + " " + RefreshRateConfigs::layerVoteTypeString(type);
        mTraceTags.emplace(type, tag);
    }

    return mTraceTags.at(type).c_str();
}

LayerInfo::RefreshRateHistory::HeuristicTraceTagData
LayerInfo::RefreshRateHistory::makeHeuristicTraceTagData() const {
    const std::string prefix = "LFPS ";
    const std::string suffix = "Heuristic ";
    return {.min = prefix + mName + suffix + "min",
            .max = prefix + mName + suffix + "max",
            .consistent = prefix + mName + suffix + "consistent",
            .average = prefix + mName + suffix + "average"};
}

void LayerInfo::RefreshRateHistory::clear() {
    mRefreshRates.clear();
}

bool LayerInfo::RefreshRateHistory::add(Fps refreshRate, nsecs_t now) {
    mRefreshRates.push_back({refreshRate, now});
    while (mRefreshRates.size() >= HISTORY_SIZE ||
           now - mRefreshRates.front().timestamp > HISTORY_DURATION.count()) {
        mRefreshRates.pop_front();
    }

    if (CC_UNLIKELY(sTraceEnabled)) {
        if (!mHeuristicTraceTagData.has_value()) {
            mHeuristicTraceTagData = makeHeuristicTraceTagData();
        }

        ATRACE_INT(mHeuristicTraceTagData->average.c_str(), refreshRate.getIntValue());
    }

    return isConsistent();
}

bool LayerInfo::RefreshRateHistory::isConsistent() const {
    if (mRefreshRates.empty()) return true;

    const auto max = std::max_element(mRefreshRates.begin(), mRefreshRates.end());
    const auto min = std::min_element(mRefreshRates.begin(), mRefreshRates.end());
    const auto consistent =
            max->refreshRate.getValue() - min->refreshRate.getValue() < MARGIN_CONSISTENT_FPS;

    if (CC_UNLIKELY(sTraceEnabled)) {
        if (!mHeuristicTraceTagData.has_value()) {
            mHeuristicTraceTagData = makeHeuristicTraceTagData();
        }

        ATRACE_INT(mHeuristicTraceTagData->max.c_str(), max->refreshRate.getIntValue());
        ATRACE_INT(mHeuristicTraceTagData->min.c_str(), min->refreshRate.getIntValue());
        ATRACE_INT(mHeuristicTraceTagData->consistent.c_str(), consistent);
    }

    return consistent;
}

} // namespace android::scheduler

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wextra"