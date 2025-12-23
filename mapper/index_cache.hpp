#pragma once

#include <iostream>
#include <fstream>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <atomic>
#include "defs.hpp"
#include "utils.hpp"

using namespace std;


const float CLEANING_TARGET_RATIO = 0.9;

struct CacheEntry {
    ReleaseRecordingIndex *data;
    atomic<int>            ref_count;
    time_t                 last_accessed;
    
    CacheEntry(ReleaseRecordingIndex *d) 
        : data(d), ref_count(0), last_accessed(0) {}
};

class IndexCache {
    private:
        map<unsigned int, CacheEntry*>  index;
        mutex                           mtx;
        thread                         *cleaner_thread;
        size_t                          max_memory_mb;         // max overall RSS in MB
        size_t                          target_memory_mb;      // target RSS after cleaning
        int                             cleaner_delay_secs;    // seconds between cleaner checks
        atomic<bool>                    stop;

    public:

        // Memory usage limit is specified in MB (applies to overall process RSS)
        // cleaner_delay is seconds between cache cleaner checks
        IndexCache(int _max_memory_mb, int _cleaner_delay_secs = 60) { 
            stop = false;
            cleaner_thread = nullptr;
            max_memory_mb = (size_t)_max_memory_mb;
            target_memory_mb = (size_t)(max_memory_mb * CLEANING_TARGET_RATIO);
            cleaner_delay_secs = _cleaner_delay_secs;
            lb_log("Index cache created: max RSS %zuMB, target %zuMB, cleaner delay %ds", 
                   max_memory_mb, target_memory_mb, cleaner_delay_secs);
        }
        
        ~IndexCache() {
            if (cleaner_thread) {
                stop = true;
                cleaner_thread->join();
                delete cleaner_thread;
            }
            clear();
        }
        
        void
        clear() {
            lock_guard<mutex> lock(mtx);
            for(auto &item : index) {
                delete item.second->data;
                delete item.second;
            }
            index.clear();
        }
        
        size_t get_cache_entry_count() {
            lock_guard<mutex> lock(mtx);
            return index.size();
        }
        
        void
        trim() {
            size_t start_rss = get_current_rss_mb();
            lb_log("Cache trim starting: RSS %zuMB, target %zuMB", start_rss, target_memory_mb);
            
            // Phase 1: Build candidate list - lock briefly
            vector<pair<unsigned int, time_t>> candidates;
            {
                lock_guard<mutex> lock(mtx);
                for (const auto &item : index) {
                    if (item.second->ref_count == 0) {
                        candidates.push_back({item.first, item.second->last_accessed});
                    }
                }
                lb_log("  %zu candidates (ref_count==0) out of %zu total entries", 
                       candidates.size(), index.size());
            }
            
            // Phase 2: Sort candidates by last_accessed (oldest first) - no lock needed
            sort(candidates.begin(), candidates.end(), 
                 [](const auto& a, const auto& b) { return a.second < b.second; });
            
            // Phase 3: Delete items one at a time, acquiring lock briefly for each
            int deleted_count = 0;
            for (const auto &candidate : candidates) {
                {
                    lock_guard<mutex> lock(mtx);
                    auto iter = index.find(candidate.first);
                    // Re-check: entry still exists and ref_count still 0?
                    if (iter != index.end() && iter->second->ref_count == 0) {
                        delete iter->second->data;
                        delete iter->second;
                        index.erase(iter);
                        deleted_count++;
                    }
                }
                
                // Small delay to let other threads use the cache
                this_thread::sleep_for(chrono::milliseconds(1));
                
                // Check if we've freed enough (check RSS periodically, not every iteration)
                if (deleted_count % 10 == 0) {
                    size_t current_rss = get_current_rss_mb();
                    if (current_rss <= target_memory_mb) {
                        lb_log("Cache trim complete: RSS %zuMB (deleted %d entries)", 
                               current_rss, deleted_count);
                        return;
                    }
                }
            }
            
            size_t end_rss = get_current_rss_mb();
            lb_log("Cache trim finished: RSS %zuMB (was %zuMB, deleted %d entries, target was %zuMB)", 
                   end_rss, start_rss, deleted_count, target_memory_mb);
        }
        
        // Cache takes ownership of data. Returns the cached pointer (which may differ from input
        // if another thread already added this entry). Returned pointer has ref_count incremented.
        // Caller MUST call release() when done.
        ReleaseRecordingIndex *
        add(unsigned int artist_credit_id, ReleaseRecordingIndex *data) {
            lock_guard<mutex> lock(mtx);
            
            auto iter = index.find(artist_credit_id);
            if (iter != index.end()) {
                // Already in cache - delete the new one, return existing
                delete data;
                iter->second->ref_count++;
                iter->second->last_accessed = chrono::system_clock::to_time_t(chrono::system_clock::now());
                return iter->second->data;
            } else {
                CacheEntry *entry = new CacheEntry(data);
                entry->ref_count = 1;  // Caller is using it
                entry->last_accessed = chrono::system_clock::to_time_t(chrono::system_clock::now());
                index[artist_credit_id] = entry;
                return data;
            }
        }

        // Returns pointer to cached data with ref_count incremented.
        // Caller MUST call release() when done. Returns nullptr if not in cache.
        ReleaseRecordingIndex *
        get(unsigned int artist_credit_id) {
            lock_guard<mutex> lock(mtx);
            
            auto iter = index.find(artist_credit_id);
            if (iter != index.end()) {
                iter->second->ref_count++;
                iter->second->last_accessed = chrono::system_clock::to_time_t(chrono::system_clock::now());
                return iter->second->data;
            }
            return nullptr;
        }
        
        // Release a reference to a cached index. Call this when done using the pointer.
        void
        release(unsigned int artist_credit_id) {
            lock_guard<mutex> lock(mtx);
            
            auto iter = index.find(artist_credit_id);
            if (iter != index.end()) {
                iter->second->ref_count--;
            }
        }
        
        void cache_cleaner() {
            lb_log("Cache cleaner started: max RSS %zuMB, check every %ds", max_memory_mb, cleaner_delay_secs);

            while(!stop) {
                for(int i = 0; i < cleaner_delay_secs && !stop; i++)
                    this_thread::sleep_for(chrono::seconds(1));
                
                size_t current_rss = get_current_rss_mb();
                if (current_rss >= max_memory_mb) {
                    lb_log("Cache cleaner triggered: RSS %zuMB >= %zuMB max", current_rss, max_memory_mb);
                    trim();
                }
                else
                    lb_log("Cache cleaner not triggered: RSS %zuMB < %zuMB max", current_rss, max_memory_mb);
            }
        }
        
        void start() {
            cleaner_thread = new thread(&IndexCache::cache_cleaner, this);
        }
};
