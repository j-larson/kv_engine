/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "checkpoint_config.h"

#include "checkpoint.h"

#include "configuration.h"
#include "ep_engine.h"

/**
 * A listener class to update checkpoint related configs at runtime.
 */
class CheckpointConfig::ChangeListener : public ValueChangedListener {
public:
    ChangeListener(CheckpointConfig& c) : config(c) {
    }

    virtual void sizeValueChanged(const std::string& key, size_t value) {
        if (key.compare("chk_period") == 0) {
            config.setCheckpointPeriod(value);
        } else if (key.compare("chk_max_items") == 0) {
            config.setCheckpointMaxItems(value);
        } else if (key.compare("max_checkpoints") == 0) {
            config.setMaxCheckpoints(value);
        }
    }

    virtual void booleanValueChanged(const std::string& key, bool value) {
        if (key.compare("item_num_based_new_chk") == 0) {
            config.allowItemNumBasedNewCheckpoint(value);
        } else if (key.compare("keep_closed_chks") == 0) {
            config.allowKeepClosedCheckpoints(value);
        } else if (key.compare("enable_chk_merge") == 0) {
            config.allowCheckpointMerge(value);
        }
    }

private:
    CheckpointConfig& config;
};

CheckpointConfig::CheckpointConfig()
    : checkpointPeriod(DEFAULT_CHECKPOINT_PERIOD),
      checkpointMaxItems(DEFAULT_CHECKPOINT_ITEMS),
      maxCheckpoints(DEFAULT_MAX_CHECKPOINTS),
      itemNumBasedNewCheckpoint(true),
      keepClosedCheckpoints(false),
      enableChkMerge(false),
      persistenceEnabled(true) { /* empty */
}

CheckpointConfig::CheckpointConfig(rel_time_t period,
                                   size_t max_items,
                                   size_t max_ckpts,
                                   bool item_based_new_ckpt,
                                   bool keep_closed_ckpts,
                                   bool enable_ckpt_merge,
                                   bool persistence_enabled)
    : checkpointPeriod(period),
      checkpointMaxItems(max_items),
      maxCheckpoints(max_ckpts),
      itemNumBasedNewCheckpoint(item_based_new_ckpt),
      keepClosedCheckpoints(keep_closed_ckpts),
      enableChkMerge(enable_ckpt_merge),
      persistenceEnabled(persistence_enabled) {
}

CheckpointConfig::CheckpointConfig(EventuallyPersistentEngine& e) {
    Configuration& config = e.getConfiguration();
    checkpointPeriod = config.getChkPeriod();
    checkpointMaxItems = config.getChkMaxItems();
    maxCheckpoints = config.getMaxCheckpoints();
    itemNumBasedNewCheckpoint = config.isItemNumBasedNewChk();
    keepClosedCheckpoints = config.isKeepClosedChks();
    enableChkMerge = config.isEnableChkMerge();
    persistenceEnabled = config.getBucketType() == "persistent";
}

void CheckpointConfig::addConfigChangeListener(
        EventuallyPersistentEngine& engine) {
    Configuration& configuration = engine.getConfiguration();
    configuration.addValueChangedListener(
            "chk_period", new ChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener(
            "chk_max_items", new ChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener(
            "max_checkpoints",
            new ChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener(
            "item_num_based_new_chk",
            new ChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener(
            "keep_closed_chks",
            new ChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener(
            "enable_chk_merge",
            new ChangeListener(engine.getCheckpointConfig()));
}

bool CheckpointConfig::validateCheckpointMaxItemsParam(
        size_t checkpoint_max_items) {
    if (checkpoint_max_items < MIN_CHECKPOINT_ITEMS ||
        checkpoint_max_items > MAX_CHECKPOINT_ITEMS) {
        std::stringstream ss;
        ss << "New checkpoint_max_items param value " << checkpoint_max_items
           << " is not ranged between the min allowed value "
           << MIN_CHECKPOINT_ITEMS << " and max value " << MAX_CHECKPOINT_ITEMS;
        LOG(EXTENSION_LOG_WARNING, "%s", ss.str().c_str());
        return false;
    }
    return true;
}

bool CheckpointConfig::validateCheckpointPeriodParam(size_t checkpoint_period) {
    if (checkpoint_period < MIN_CHECKPOINT_PERIOD ||
        checkpoint_period > MAX_CHECKPOINT_PERIOD) {
        std::stringstream ss;
        ss << "New checkpoint_period param value " << checkpoint_period
           << " is not ranged between the min allowed value "
           << MIN_CHECKPOINT_PERIOD << " and max value "
           << MAX_CHECKPOINT_PERIOD;
        LOG(EXTENSION_LOG_WARNING, "%s", ss.str().c_str());
        return false;
    }
    return true;
}

bool CheckpointConfig::validateMaxCheckpointsParam(size_t max_checkpoints) {
    if (max_checkpoints < DEFAULT_MAX_CHECKPOINTS ||
        max_checkpoints > MAX_CHECKPOINTS_UPPER_BOUND) {
        std::stringstream ss;
        ss << "New max_checkpoints param value " << max_checkpoints
           << " is not ranged between the min allowed value "
           << DEFAULT_MAX_CHECKPOINTS << " and max value "
           << MAX_CHECKPOINTS_UPPER_BOUND;
        LOG(EXTENSION_LOG_WARNING, "%s", ss.str().c_str());
        return false;
    }
    return true;
}

void CheckpointConfig::setCheckpointPeriod(size_t value) {
    if (!validateCheckpointPeriodParam(value)) {
        value = DEFAULT_CHECKPOINT_PERIOD;
    }
    checkpointPeriod = static_cast<rel_time_t>(value);
}

void CheckpointConfig::setCheckpointMaxItems(size_t value) {
    if (!validateCheckpointMaxItemsParam(value)) {
        value = DEFAULT_CHECKPOINT_ITEMS;
    }
    checkpointMaxItems = value;
}

void CheckpointConfig::setMaxCheckpoints(size_t value) {
    if (!validateMaxCheckpointsParam(value)) {
        value = DEFAULT_MAX_CHECKPOINTS;
    }
    maxCheckpoints = value;
}
