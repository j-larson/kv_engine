/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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

#include "ep_bucket.h"

#include "bgfetcher.h"
#include "checkpoint.h"
#include "ep_engine.h"
#include "ep_time.h"
#include "ep_vb.h"
#include "failover-table.h"
#include "flusher.h"
#include "persistence_callback.h"
#include "replicationthrottle.h"
#include "tasks.h"

/**
 * Callback class used by EpStore, for adding relevant keys
 * to bloomfilter during compaction.
 */
class BloomFilterCallback : public Callback<uint16_t&, const DocKey&, bool&> {
public:
    BloomFilterCallback(KVBucket& eps) : store(eps) {
    }

    void callback(uint16_t& vbucketId, const DocKey& key, bool& isDeleted) {
        VBucketPtr vb = store.getVBucket(vbucketId);
        if (vb) {
            /* Check if a temporary filter has been initialized. If not,
             * initialize it. If initialization fails, throw an exception
             * to the caller and let the caller deal with it.
             */
            bool tempFilterInitialized = vb->isTempFilterAvailable();
            if (!tempFilterInitialized) {
                tempFilterInitialized = initTempFilter(vbucketId);
            }

            if (!tempFilterInitialized) {
                throw std::runtime_error(
                        "BloomFilterCallback::callback: Failed "
                        "to initialize temporary filter for vbucket: " +
                        std::to_string(vbucketId));
            }

            if (store.getItemEvictionPolicy() == VALUE_ONLY) {
                /**
                 * VALUE-ONLY EVICTION POLICY
                 * Consider deleted items only.
                 */
                if (isDeleted) {
                    vb->addToTempFilter(key);
                }
            } else {
                /**
                 * FULL EVICTION POLICY
                 * If vbucket's resident ratio is found to be less than
                 * the residency threshold, consider all items, otherwise
                 * consider deleted and non-resident items only.
                 */
                bool residentRatioLessThanThreshold =
                        vb->isResidentRatioUnderThreshold(
                                store.getBfiltersResidencyThreshold());
                if (residentRatioLessThanThreshold) {
                    vb->addToTempFilter(key);
                } else {
                    if (isDeleted || !store.isMetaDataResident(vb, key)) {
                        vb->addToTempFilter(key);
                    }
                }
            }
        }
    }

private:
    bool initTempFilter(uint16_t vbucketId);
    KVBucket& store;
};

bool BloomFilterCallback::initTempFilter(uint16_t vbucketId) {
    Configuration& config = store.getEPEngine().getConfiguration();
    VBucketPtr vb = store.getVBucket(vbucketId);
    if (!vb) {
        return false;
    }

    size_t initial_estimation = config.getBfilterKeyCount();
    size_t estimated_count;
    size_t num_deletes =
            store.getROUnderlying(vbucketId)->getNumPersistedDeletes(vbucketId);
    item_eviction_policy_t eviction_policy = store.getItemEvictionPolicy();
    if (eviction_policy == VALUE_ONLY) {
        /**
         * VALUE-ONLY EVICTION POLICY
         * Obtain number of persisted deletes from underlying kvstore.
         * Bloomfilter's estimated_key_count = 1.25 * deletes
         */
        estimated_count = round(1.25 * num_deletes);
    } else {
        /**
         * FULL EVICTION POLICY
         * First determine if the resident ratio of vbucket is less than
         * the threshold from configuration.
         */
        bool residentRatioAlert = vb->isResidentRatioUnderThreshold(
                store.getBfiltersResidencyThreshold());

        /**
         * Based on resident ratio against threshold, estimate count.
         *
         * 1. If resident ratio is greater than the threshold:
         * Obtain number of persisted deletes from underlying kvstore.
         * Obtain number of non-resident-items for vbucket.
         * Bloomfilter's estimated_key_count =
         *                              1.25 * (deletes + non-resident)
         *
         * 2. Otherwise:
         * Obtain number of items for vbucket.
         * Bloomfilter's estimated_key_count =
         *                              1.25 * (num_items)
         */

        if (residentRatioAlert) {
            estimated_count = round(1.25 * vb->getNumItems());
        } else {
            estimated_count =
                    round(1.25 * (num_deletes + vb->getNumNonResidentItems()));
        }
    }

    if (estimated_count < initial_estimation) {
        estimated_count = initial_estimation;
    }

    vb->initTempFilter(estimated_count, config.getBfilterFpProb());

    return true;
}

class ExpiredItemsCallback : public Callback<Item&, time_t&> {
public:
    ExpiredItemsCallback(KVBucket& store) : epstore(store) {
    }

    void callback(Item& it, time_t& startTime) {
        if (epstore.compactionCanExpireItems()) {
            epstore.deleteExpiredItem(it, startTime, ExpireBy::Compactor);
        }
    }

private:
    KVBucket& epstore;
};

EPBucket::EPBucket(EventuallyPersistentEngine& theEngine)
    : KVBucket(theEngine) {
    const std::string& policy =
            engine.getConfiguration().getItemEvictionPolicy();
    if (policy.compare("value_only") == 0) {
        eviction_policy = VALUE_ONLY;
    } else {
        eviction_policy = FULL_EVICTION;
    }
    replicationThrottle = std::make_unique<ReplicationThrottle>(
            engine.getConfiguration(), stats);
}

bool EPBucket::initialize() {
    KVBucket::initialize();

    enableItemPager();

    if (!startBgFetcher()) {
        LOG(EXTENSION_LOG_FATAL,
           "EPBucket::initialize: Failed to create and start bgFetchers");
        return false;
    }
    startFlusher();

    return true;
}

void EPBucket::deinitialize() {
    stopFlusher();
    stopBgFetcher();

    KVBucket::deinitialize();
}

void EPBucket::reset() {
    KVBucket::reset();

    // Need to additionally update disk state
    bool inverse = true;
    deleteAllTaskCtx.delay.compare_exchange_strong(inverse, false);
    // Waking up (notifying) one flusher is good enough for diskDeleteAll
    vbMap.getShard(EP_PRIMARY_SHARD)->getFlusher()->notifyFlushEvent();
}

int EPBucket::flushVBucket(uint16_t vbid) {
    KVShard *shard = vbMap.getShardByVbId(vbid);
    if (diskDeleteAll && !deleteAllTaskCtx.delay) {
        if (shard->getId() == EP_PRIMARY_SHARD) {
            flushOneDeleteAll();
        } else {
            // disk flush is pending just return
            return 0;
        }
    }

    int items_flushed = 0;
    const auto flush_start = ProcessClock::now();

    auto vb = getLockedVBucket(vbid, std::try_to_lock);
    if (!vb.owns_lock()) { // Try another bucket if this one is locked
        return RETRY_FLUSH_VBUCKET; // to avoid blocking flusher
    }
    if (vb) {
        std::vector<queued_item> items;
        KVStore *rwUnderlying = getRWUnderlying(vbid);

        while (!vb->rejectQueue.empty()) {
            items.push_back(vb->rejectQueue.front());
            vb->rejectQueue.pop();
        }

        // Append any 'backfill' items (mutations added by a DCP stream).
        vb->getBackfillItems(items);

        // Append all items outstanding for the persistence cursor.
        snapshot_range_t range;
        auto _begin_ = ProcessClock::now();
        range = vb->checkpointManager->getAllItemsForCursor(
                CheckpointManager::pCursorName, items);
        stats.persistenceCursorGetItemsHisto.add(
                std::chrono::duration_cast<std::chrono::microseconds>(
                        ProcessClock::now() - _begin_));

        if (!items.empty()) {
            while (!rwUnderlying->begin(
                    std::make_unique<EPTransactionContext>(stats, *vb))) {
                ++stats.beginFailed;
                LOG(EXTENSION_LOG_WARNING, "Failed to start a transaction!!! "
                    "Retry in 1 sec ...");
                sleep(1);
            }
            rwUnderlying->optimizeWrites(items);

            Item *prev = NULL;
            auto vbstate = vb->getVBucketState();
            uint64_t maxSeqno = 0;
            range.start = std::max(range.start, vbstate.lastSnapStart);

            bool mustCheckpointVBState = false;
            auto& pcbs = rwUnderlying->getPersistenceCbList();

            SystemEventFlush sef;

            for (const auto& item : items) {

                if (!item->shouldPersist()) {
                    continue;
                }

                // Pass the Item through the SystemEventFlush which may filter
                // the item away (return Skip).
                if (sef.process(item) == ProcessStatus::Skip) {
                    // The item has no further flushing actions i.e. we've
                    // absorbed it in the process function.
                    // Update stats and carry-on
                    --stats.diskQueueSize;
                    vb->doStatsForFlushing(*item, item->size());
                    continue;
                }

                if (item->getOperation() == queue_op::set_vbucket_state) {
                    // No actual item explicitly persisted to (this op exists
                    // to ensure a commit occurs with the current vbstate);
                    // flag that we must trigger a snapshot even if there are
                    // no 'real' items in the checkpoint.
                    mustCheckpointVBState = true;

                    // Update queuing stats how this item has logically been
                    // processed.
                    --stats.diskQueueSize;
                    vb->doStatsForFlushing(*item, item->size());

                } else if (!prev || prev->getKey() != item->getKey()) {
                    prev = item.get();
                    ++items_flushed;
                    auto cb = flushOneDelOrSet(item, vb.getVB());
                    if (cb) {
                        pcbs.emplace_back(std::move(cb));
                    }

                    maxSeqno = std::max(maxSeqno, (uint64_t)item->getBySeqno());
                    vbstate.maxCas = std::max(vbstate.maxCas, item->getCas());
                    if (item->isDeleted()) {
                        vbstate.maxDeletedSeqno =
                                std::max(vbstate.maxDeletedSeqno,
                                         item->getRevSeqno());
                    }
                    ++stats.flusher_todo;

                } else {
                    // Item is the same key as the previous[1] one - don't need
                    // to flush to disk.
                    // [1] Previous here really means 'next' - optimizeWrites()
                    //     above has actually re-ordered items such that items
                    //     with the same key are ordered from high->low seqno.
                    //     This means we only write the highest (i.e. newest)
                    //     item for a given key, and discard any duplicate,
                    //     older items.
                    --stats.diskQueueSize;
                    vb->doStatsForFlushing(*item, item->size());
                }
            }


            {
                ReaderLockHolder rlh(vb->getStateLock());
                if (vb->getState() == vbucket_state_active) {
                    if (maxSeqno) {
                        range.start = maxSeqno;
                        range.end = maxSeqno;
                    }
                }

                // Update VBstate based on the changes we have just made,
                // then tell the rwUnderlying the 'new' state
                // (which will persisted as part of the commit() below).
                vbstate.lastSnapStart = range.start;
                vbstate.lastSnapEnd = range.end;

                // Track the lowest seqno written in spock and record it as
                // the HLC epoch, a seqno which we can be sure the value has a
                // HLC CAS.
                vbstate.hlcCasEpochSeqno = vb->getHLCEpochSeqno();
                if (vbstate.hlcCasEpochSeqno == HlcCasSeqnoUninitialised) {
                    vbstate.hlcCasEpochSeqno = range.start;
                    vb->setHLCEpochSeqno(range.start);
                }

                // Track if the VB has xattrs present
                vbstate.mightContainXattrs = vb->mightContainXattrs();

                // Do we need to trigger a persist of the state?
                // If there are no "real" items to flush, and we encountered
                // a set_vbucket_state meta-item.
                auto options = VBStatePersist::VBSTATE_CACHE_UPDATE_ONLY;
                if ((items_flushed == 0) && mustCheckpointVBState) {
                    options = VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT;
                }

                if (rwUnderlying->snapshotVBucket(vb->getId(), vbstate,
                                                  options) != true) {
                    return RETRY_FLUSH_VBUCKET;
                }

                if (vb->setBucketCreation(false)) {
                    LOG(EXTENSION_LOG_INFO, "VBucket %" PRIu16 " created", vbid);
                }
            }

            /* Perform an explicit commit to disk if the commit
             * interval reaches zero and if there is a non-zero number
             * of items to flush.
             * Or if there is a manifest item
             */
            if (items_flushed > 0 || sef.getCollectionsManifestItem()) {
                commit(*rwUnderlying, sef.getCollectionsManifestItem());

                // Now the commit is complete, vBucket file must exist.
                if (vb->setBucketCreation(false)) {
                    LOG(EXTENSION_LOG_INFO, "VBucket %" PRIu16 " created", vbid);
                }
            }

            if (vb->rejectQueue.empty()) {
                vb->setPersistedSnapshot(range.start, range.end);
                uint64_t highSeqno = rwUnderlying->getLastPersistedSeqno(vbid);
                if (highSeqno > 0 && highSeqno != vb->getPersistenceSeqno()) {
                    vb->setPersistenceSeqno(highSeqno);
                }
            }

            auto flush_end = ProcessClock::now();
            uint64_t trans_time =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                            flush_end - flush_start)
                            .count();

            lastTransTimePerItem.store((items_flushed == 0) ? 0 :
                                       static_cast<double>(trans_time) /
                                       static_cast<double>(items_flushed));
            stats.cumulativeFlushTime.fetch_add(trans_time);
            stats.flusher_todo.store(0);
            stats.totalPersistVBState++;
        }

        rwUnderlying->pendingTasks();

        if (vb->checkpointManager->getNumCheckpoints() > 1) {
            wakeUpCheckpointRemover();
        }

        if (vb->rejectQueue.empty()) {
            vb->checkpointManager->itemsPersisted();
            uint64_t seqno = vb->getPersistenceSeqno();
            uint64_t chkid =
                    vb->checkpointManager->getPersistenceCursorPreChkId();
            vb->notifyHighPriorityRequests(
                    engine, seqno, HighPriorityVBNotify::Seqno);
            vb->notifyHighPriorityRequests(
                    engine, chkid, HighPriorityVBNotify::ChkPersistence);
            if (chkid > 0 && chkid != vb->getPersistenceCheckpointId()) {
                vb->setPersistenceCheckpointId(chkid);
            }
        } else {
            return RETRY_FLUSH_VBUCKET;
        }
    }

    return items_flushed;
}

void EPBucket::commit(KVStore& kvstore, const Item* collectionsManifest) {
    auto& pcbs = kvstore.getPersistenceCbList();
    BlockTimer timer(&stats.diskCommitHisto, "disk_commit", stats.timingLog);
    auto commit_start = ProcessClock::now();

    while (!kvstore.commit(collectionsManifest)) {
        ++stats.commitFailed;
        LOG(EXTENSION_LOG_WARNING,
            "KVBucket::commit: kvstore.commit failed!!! Retry in 1 sec...");
        sleep(1);
    }

    pcbs.clear();
    pcbs.shrink_to_fit();

    ++stats.flusherCommits;
    auto commit_end = ProcessClock::now();
    auto commit_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                               commit_end - commit_start)
                               .count();
    stats.commit_time.store(commit_time);
    stats.cumulativeCommitTime.fetch_add(commit_time);
}

void EPBucket::startFlusher() {
    for (const auto& shard : vbMap.shards) {
        shard->getFlusher()->start();
    }
}

void EPBucket::stopFlusher() {
    for (const auto& shard : vbMap.shards) {
        auto* flusher = shard->getFlusher();
        LOG(EXTENSION_LOG_NOTICE,
            "Attempting to stop the flusher for "
            "shard:%" PRIu16,
            shard->getId());
        bool rv = flusher->stop(stats.forceShutdown);
        if (rv && !stats.forceShutdown) {
            flusher->wait();
        }
    }
}

bool EPBucket::pauseFlusher() {
    bool rv = true;
    for (const auto& shard : vbMap.shards) {
        auto* flusher = shard->getFlusher();
        if (!flusher->pause()) {
            LOG(EXTENSION_LOG_WARNING,
                "Attempted to pause flusher in state "
                "[%s], shard = %d",
                flusher->stateName(),
                shard->getId());
            rv = false;
        }
    }
    return rv;
}

bool EPBucket::resumeFlusher() {
    bool rv = true;
    for (const auto& shard : vbMap.shards) {
        auto* flusher = shard->getFlusher();
        if (!flusher->resume()) {
            LOG(EXTENSION_LOG_WARNING,
                "Attempted to resume flusher in state [%s], "
                "shard = %" PRIu16,
                flusher->stateName(),
                shard->getId());
            rv = false;
        }
    }
    return rv;
}

void EPBucket::wakeUpFlusher() {
    if (stats.diskQueueSize.load() == 0) {
        for (const auto& shard : vbMap.shards) {
            shard->getFlusher()->wake();
        }
    }
}

bool EPBucket::startBgFetcher() {
    for (const auto& shard : vbMap.shards) {
        BgFetcher* bgfetcher = shard->getBgFetcher();
        if (bgfetcher == NULL) {
            LOG(EXTENSION_LOG_WARNING,
                "Failed to start bg fetcher for shard %" PRIu16,
                shard->getId());
            return false;
        }
        bgfetcher->start();
    }
    return true;
}

void EPBucket::stopBgFetcher() {
    for (const auto& shard : vbMap.shards) {
        BgFetcher* bgfetcher = shard->getBgFetcher();
        if (multiBGFetchEnabled() && bgfetcher->pendingJob()) {
            LOG(EXTENSION_LOG_WARNING,
                "Shutting down engine while there are still pending data "
                "read for shard %" PRIu16 " from database storage",
                shard->getId());
        }
        LOG(EXTENSION_LOG_NOTICE,
            "Stopping bg fetcher for shard:%" PRIu16,
            shard->getId());
        bgfetcher->stop();
    }
}

ENGINE_ERROR_CODE EPBucket::scheduleCompaction(uint16_t vbid,
                                               compaction_ctx c,
                                               const void* cookie) {
    ENGINE_ERROR_CODE errCode = checkForDBExistence(c.db_file_id);
    if (errCode != ENGINE_SUCCESS) {
        return errCode;
    }

    /* Obtain the vbucket so we can get the previous purge seqno */
    VBucketPtr vb = vbMap.getBucket(vbid);
    if (!vb) {
        return ENGINE_NOT_MY_VBUCKET;
    }

    /* Update the compaction ctx with the previous purge seqno */
    c.max_purged_seq[vbid] = vb->getPurgeSeqno();

    LockHolder lh(compactionLock);
    ExTask task = std::make_shared<CompactTask>(*this, c, cookie);
    compactionTasks.push_back(std::make_pair(c.db_file_id, task));
    if (compactionTasks.size() > 1) {
        if ((stats.diskQueueSize > compactionWriteQueueCap &&
             compactionTasks.size() > (vbMap.getNumShards() / 2)) ||
            engine.getWorkLoadPolicy().getWorkLoadPattern() == READ_HEAVY) {
            // Snooze a new compaction task.
            // We will wake it up when one of the existing compaction tasks is
            // done.
            task->snooze(60);
        }
    }

    ExecutorPool::get()->schedule(task);

    LOG(EXTENSION_LOG_DEBUG,
        "Scheduled compaction task %" PRIu64
        " on db %d,"
        "purge_before_ts = %" PRIu64 ", purge_before_seq = %" PRIu64
        ", dropdeletes = %d",
        uint64_t(task->getId()),
        c.db_file_id,
        c.purge_before_ts,
        c.purge_before_seq,
        c.drop_deletes);

    return ENGINE_EWOULDBLOCK;
}

void EPBucket::flushOneDeleteAll() {
    for (VBucketMap::id_type i = 0; i < vbMap.getSize(); ++i) {
        auto vb = getLockedVBucket(i);
        if (!vb) {
            continue;
        }
        // Reset the vBucket if it's non-null and not already in the middle of
        // being created / destroyed.
        if (!(vb->isBucketCreation() || vb->isDeletionDeferred())) {
            getRWUnderlying(vb->getId())->reset(i);
        }
        // Reset disk item count.
        vb->setNumTotalItems(0);
    }

    setDeleteAllComplete();
}

std::unique_ptr<PersistenceCallback> EPBucket::flushOneDelOrSet(
        const queued_item& qi, VBucketPtr& vb) {
    if (!vb) {
        --stats.diskQueueSize;
        return NULL;
    }

    int64_t bySeqno = qi->getBySeqno();
    bool deleted = qi->isDeleted();
    rel_time_t queued(qi->getQueuedTime());

    auto dirtyAge = std::chrono::seconds(ep_current_time() - queued);
    stats.dirtyAgeHisto.add(dirtyAge);
    stats.dirtyAge.store(static_cast<rel_time_t>(dirtyAge.count()));
    stats.dirtyAgeHighWat.store(std::max(stats.dirtyAge.load(),
                                         stats.dirtyAgeHighWat.load()));

    KVStore *rwUnderlying = getRWUnderlying(qi->getVBucketId());
    if (!deleted) {
        // TODO: Need to separate disk_insert from disk_update because
        // bySeqno doesn't give us that information.
        BlockTimer timer(bySeqno == -1 ?
                         &stats.diskInsertHisto : &stats.diskUpdateHisto,
                         bySeqno == -1 ? "disk_insert" : "disk_update",
                         stats.timingLog);
        auto cb = std::make_unique<PersistenceCallback>(qi, qi->getCas());
        rwUnderlying->set(*qi, *cb);
        return cb;
    } else {
        BlockTimer timer(&stats.diskDelHisto, "disk_delete",
                         stats.timingLog);
        auto cb = std::make_unique<PersistenceCallback>(qi, 0);
        rwUnderlying->del(*qi, *cb);
        return cb;
    }
}

void EPBucket::compactInternal(compaction_ctx* ctx) {
    BloomFilterCBPtr filter(new BloomFilterCallback(*this));
    ctx->bloomFilterCallback = filter;

    ExpiredItemsCBPtr expiry(new ExpiredItemsCallback(*this));
    ctx->expiryCallback = expiry;

    ctx->collectionsEraser = std::bind(&KVBucket::collectionsEraseKey,
                                       this,
                                       uint16_t(ctx->db_file_id),
                                       std::placeholders::_1,
                                       std::placeholders::_2);

    KVShard* shard = vbMap.getShardByVbId(ctx->db_file_id);
    KVStore* store = shard->getRWUnderlying();
    bool result = store->compactDB(ctx);

    Configuration& config = getEPEngine().getConfiguration();
    /* Iterate over all the vbucket ids set in max_purged_seq map. If there is
     * an entry
     * in the map for a vbucket id, then it was involved in compaction and thus
     * can
     * be used to update the associated bloom filters and purge sequence numbers
     */
    for (auto& it : ctx->max_purged_seq) {
        const uint16_t vbid = it.first;
        VBucketPtr vb = getVBucket(vbid);
        if (!vb) {
            continue;
        }

        if (config.isBfilterEnabled() && result) {
            vb->swapFilter();
        } else {
            vb->clearFilter();
        }
        vb->setPurgeSeqno(it.second);
    }

    LOG(EXTENSION_LOG_NOTICE,
        "Compaction of db file id: %d completed (%s). "
        "tombstones_purged:%" PRIu64 ", collection_items_erased:%" PRIu64
        ", pre{size:%" PRIu64 ", items:%" PRIu64 ", deleted_items:%" PRIu64
        ", purge_seqno:%" PRIu64 "}, post{size:%" PRIu64 ", items:%" PRIu64
        ", deleted_items:%" PRIu64 ", purge_seqno:%" PRIu64 "}",
        ctx->db_file_id,
        result ? "ok" : "failed",
        ctx->stats.tombstonesPurged,
        ctx->stats.collectionsItemsPurged,
        ctx->stats.pre.size,
        ctx->stats.pre.items,
        ctx->stats.pre.deletedItems,
        ctx->stats.pre.purgeSeqno,
        ctx->stats.post.size,
        ctx->stats.post.items,
        ctx->stats.post.deletedItems,
        ctx->stats.post.purgeSeqno);
}

bool EPBucket::doCompact(compaction_ctx* ctx, const void* cookie) {
    ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
    StorageProperties storeProp = getStorageProperties();
    bool concWriteCompact = storeProp.hasConcWriteCompact();
    uint16_t vbid = ctx->db_file_id;

    /**
     * Check if the underlying storage engine allows writes concurrently
     * as the database file is being compacted. If not, a lock needs to
     * be held in order to serialize access to the database file between
     * the writer and compactor threads
     */
    if (concWriteCompact == false) {
        auto vb = getLockedVBucket(vbid, std::try_to_lock);
        if (!vb.owns_lock()) {
            // VB currently locked; try again later.
            return true;
        }

        if (!vb) {
            err = ENGINE_NOT_MY_VBUCKET;
            engine.storeEngineSpecific(cookie, NULL);
            /**
             * Decrement session counter here, as memcached thread wouldn't
             * visit the engine interface in case of a NOT_MY_VB notification
             */
            engine.decrementSessionCtr();
        } else {
            compactInternal(ctx);
        }
    } else {
        compactInternal(ctx);
    }

    updateCompactionTasks(ctx->db_file_id);

    if (cookie) {
        engine.notifyIOComplete(cookie, err);
    }
    --stats.pendingCompactions;
    return false;
}

void EPBucket::updateCompactionTasks(DBFileId db_file_id) {
    LockHolder lh(compactionLock);
    bool erased = false, woke = false;
    std::list<CompTaskEntry>::iterator it = compactionTasks.begin();
    while (it != compactionTasks.end()) {
        if ((*it).first == db_file_id) {
            it = compactionTasks.erase(it);
            erased = true;
        } else {
            ExTask& task = (*it).second;
            if (task->getState() == TASK_SNOOZED) {
                ExecutorPool::get()->wake(task->getId());
                woke = true;
            }
            ++it;
        }
        if (erased && woke) {
            break;
        }
    }
}

std::pair<uint64_t, bool> EPBucket::getLastPersistedCheckpointId(uint16_t vb) {
    auto vbucket = vbMap.getBucket(vb);
    if (vbucket) {
        return {vbucket->getPersistenceCheckpointId(), true};
    } else {
        return {0, true};
    }
}

ENGINE_ERROR_CODE EPBucket::getFileStats(const void* cookie,
                                         ADD_STAT add_stat) {
    const auto numShards = vbMap.getNumShards();
    DBFileInfo totalInfo;

    for (uint16_t shardId = 0; shardId < numShards; shardId++) {
        const auto dbInfo =
                getRWUnderlyingByShard(shardId)->getAggrDbFileInfo();
        totalInfo.spaceUsed += dbInfo.spaceUsed;
        totalInfo.fileSize += dbInfo.fileSize;
    }

    add_casted_stat("ep_db_data_size", totalInfo.spaceUsed, add_stat, cookie);
    add_casted_stat("ep_db_file_size", totalInfo.fileSize, add_stat, cookie);

    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE EPBucket::getPerVBucketDiskStats(const void* cookie,
                                                   ADD_STAT add_stat) {
    class DiskStatVisitor : public VBucketVisitor {
    public:
        DiskStatVisitor(const void* c, ADD_STAT a) : cookie(c), add_stat(a) {
        }

        void visitBucket(VBucketPtr& vb) override {
            char buf[32];
            uint16_t vbid = vb->getId();
            DBFileInfo dbInfo =
                    vb->getShard()->getRWUnderlying()->getDbFileInfo(vbid);

            try {
                checked_snprintf(buf, sizeof(buf), "vb_%d:data_size", vbid);
                add_casted_stat(buf, dbInfo.spaceUsed, add_stat, cookie);
                checked_snprintf(buf, sizeof(buf), "vb_%d:file_size", vbid);
                add_casted_stat(buf, dbInfo.fileSize, add_stat, cookie);
            } catch (std::exception& error) {
                LOG(EXTENSION_LOG_WARNING,
                    "DiskStatVisitor::visitBucket: Failed to build stat: %s",
                    error.what());
            }
        }

    private:
        const void* cookie;
        ADD_STAT add_stat;
    };

    DiskStatVisitor dsv(cookie, add_stat);
    visit(dsv);
    return ENGINE_SUCCESS;
}

VBucketPtr EPBucket::makeVBucket(VBucket::id_type id,
                                 vbucket_state_t state,
                                 KVShard* shard,
                                 std::unique_ptr<FailoverTable> table,
                                 NewSeqnoCallback newSeqnoCb,
                                 vbucket_state_t initState,
                                 int64_t lastSeqno,
                                 uint64_t lastSnapStart,
                                 uint64_t lastSnapEnd,
                                 uint64_t purgeSeqno,
                                 uint64_t maxCas,
                                 int64_t hlcEpochSeqno,
                                 bool mightContainXattrs,
                                 const std::string& collectionsManifest) {
    auto flusherCb = std::make_shared<NotifyFlusherCB>(shard);
    // Not using make_shared or allocate_shared
    // 1. make_shared doesn't accept a Deleter
    // 2. allocate_shared has inconsistencies between platforms in calling
    //    alloc.destroy (libc++ doesn't call it)
    return VBucketPtr(new EPVBucket(id,
                                    state,
                                    stats,
                                    engine.getCheckpointConfig(),
                                    shard,
                                    lastSeqno,
                                    lastSnapStart,
                                    lastSnapEnd,
                                    std::move(table),
                                    flusherCb,
                                    std::move(newSeqnoCb),
                                    engine.getConfiguration(),
                                    eviction_policy,
                                    initState,
                                    purgeSeqno,
                                    maxCas,
                                    hlcEpochSeqno,
                                    mightContainXattrs,
                                    collectionsManifest),
                      VBucket::DeferredDeleter(engine));
}

ENGINE_ERROR_CODE EPBucket::statsVKey(const DocKey& key,
                                      uint16_t vbucket,
                                      const void* cookie) {
    VBucketPtr vb = getVBucket(vbucket);
    if (!vb) {
        return ENGINE_NOT_MY_VBUCKET;
    }

    return vb->statsVKey(key, cookie, engine, bgFetchDelay);
}

void EPBucket::completeStatsVKey(const void* cookie,
                                 const DocKey& key,
                                 uint16_t vbid,
                                 uint64_t bySeqNum) {
    GetValue gcb = getROUnderlying(vbid)->get(key, vbid);

    if (eviction_policy == FULL_EVICTION) {
        VBucketPtr vb = getVBucket(vbid);
        if (vb) {
            vb->completeStatsVKey(key, gcb);
        }
    }

    if (gcb.getStatus() == ENGINE_SUCCESS) {
        engine.addLookupResult(cookie, std::move(gcb.item));
    } else {
        engine.addLookupResult(cookie, NULL);
    }

    --stats.numRemainingBgJobs;
    engine.notifyIOComplete(cookie, ENGINE_SUCCESS);
}

/**
 * Class that handles the disk callback during the rollback.
 * For each mutation/deletion which was discarded as part of the rollback,
 * the callback() method is invoked with the key of the discarded update.
 * It can then lookup the state of that key using dbHandle (which represents the
 * new, rolled-back file) and correct the in-memory view:
 *
 * a) If the key is not present in the Rollback header then delete it from
 *    the HashTable (if either didn't exist yet, or had previously been
 *    deleted in the Rollback header).
 * b) If the key is present in the Rollback header then replace the in-memory
 *    value with the value from the Rollback header.
 */
class EPDiskRollbackCB : public RollbackCB {
public:
    EPDiskRollbackCB(EventuallyPersistentEngine& e) : RollbackCB(), engine(e) {
    }

    void callback(GetValue& val) {
        if (!val.item) {
            throw std::invalid_argument(
                    "EPDiskRollbackCB::callback: val is NULL");
        }
        if (dbHandle == nullptr) {
            throw std::logic_error(
                    "EPDiskRollbackCB::callback: dbHandle is NULL");
        }
        UniqueItemPtr itm(std::move(val.item));
        VBucketPtr vb = engine.getVBucket(itm->getVBucketId());
        GetValue gcb = engine.getKVBucket()
                               ->getROUnderlying(itm->getVBucketId())
                               ->getWithHeader(dbHandle,
                                               itm->getKey(),
                                               itm->getVBucketId(),
                                               GetMetaOnly::No);
        if (gcb.getStatus() == ENGINE_SUCCESS) {
            UniqueItemPtr it(std::move(gcb.item));
            if (it->isDeleted()) {
                removeDeletedDoc(*vb, it->getKey());
            } else {
                MutationStatus mtype = vb->setFromInternal(*it);

                if (mtype == MutationStatus::NoMem) {
                    setStatus(ENGINE_ENOMEM);
                }
            }
        } else if (gcb.getStatus() == ENGINE_KEY_ENOENT) {
            removeDeletedDoc(*vb, itm->getKey());
        } else {
            LOG(EXTENSION_LOG_WARNING,
                "EPDiskRollbackCB::callback:Unexpected Error Status: %d",
                gcb.getStatus());
        }
    }

    /// Remove a deleted-on-disk document from the VBucket's hashtable.
    void removeDeletedDoc(VBucket& vb, const DocKey& key) {
        if (vb.deleteKey(key)) {
            setStatus(ENGINE_SUCCESS);
        } else {
            // Document didn't exist in memory - may have been deleted in since
            // the checkpoint.
            setStatus(ENGINE_KEY_ENOENT);
        }
        // Irrespective of if the in-memory delete succeeded; the document
        // doesn't exist on disk; so decrement the item count.
        vb.decrNumTotalItems();
    }

private:
    EventuallyPersistentEngine& engine;
};

RollbackResult EPBucket::doRollback(uint16_t vbid, uint64_t rollbackSeqno) {
    auto cb = std::make_shared<EPDiskRollbackCB>(engine);
    KVStore* rwUnderlying = vbMap.getShardByVbId(vbid)->getRWUnderlying();
    return rwUnderlying->rollback(vbid, rollbackSeqno, cb);
}

void EPBucket::rollbackUnpersistedItems(VBucket& vb, int64_t rollbackSeqno) {
    std::vector<queued_item> items;
    vb.checkpointManager->getAllItemsForCursor(CheckpointManager::pCursorName,
                                               items);
    for (const auto& item : items) {
        if (item->getBySeqno() > rollbackSeqno &&
            !item->isCheckPointMetaItem()) {
            GetValue gcb = getROUnderlying(vb.getId())
                                   ->get(item->getKey(), vb.getId(), false);

            if (gcb.getStatus() == ENGINE_SUCCESS) {
                vb.setFromInternal(*gcb.item.get());
            } else {
                vb.deleteKey(item->getKey());
            }
        }
    }
}

void EPBucket::notifyNewSeqno(const uint16_t vbid,
                              const VBNotifyCtx& notifyCtx) {
    if (notifyCtx.notifyFlusher) {
        notifyFlusher(vbid);
    }
    if (notifyCtx.notifyReplication) {
        notifyReplication(vbid, notifyCtx.bySeqno);
    }
}
