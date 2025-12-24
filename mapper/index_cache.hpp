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
        size_t                          max_cache_items;
        size_t                          cache_trim_count;
        int                             cleaner_delay_secs;    // seconds between cleaner checks
        atomic<bool>                    stop;

    public:

        // cleaner_delay is seconds between cache cleaner checks
        IndexCache(size_t _max_cache_items = 50000, size_t _cache_trim_count = 10000, int _cleaner_delay_secs = 60) { 
            stop = false;
            cleaner_thread = nullptr;
            max_cache_items = _max_cache_items;
            cache_trim_count = _cache_trim_count;
            cleaner_delay_secs = _cleaner_delay_secs;
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
            size_t start_count;
            
            // Phase 1: Build candidate list - lock briefly
            vector<pair<unsigned int, time_t>> candidates;
            {
                lock_guard<mutex> lock(mtx);
                start_count = index.size();
                for (const auto &item : index) {
                    if (item.second->ref_count == 0) {
                        candidates.push_back({item.first, item.second->last_accessed});
                    }
                }
            }
            
            lb_debug("Cache trim starting: %zu items, %zu candidates (ref_count==0)", 
                   start_count, candidates.size());
            
            // Phase 2: Sort candidates by last_accessed (oldest first) - no lock needed
            sort(candidates.begin(), candidates.end(), 
                 [](const auto& a, const auto& b) { return a.second < b.second; });
            
            // Phase 3: Delete until cache size is below target (MAX - TRIM)
            size_t target_count = max_cache_items - cache_trim_count;
            int deleted_count = 0;
            for (const auto &candidate : candidates) {
                // Check current size
                {
                    lock_guard<mutex> lock(mtx);
                    if (index.size() <= target_count)
                        break;
                }
                    
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
            }
            
            size_t end_count;
            {
                lock_guard<mutex> lock(mtx);
                end_count = index.size();
            }
            lb_debug("Cache trim finished: %zu items (was %zu, deleted %d, target %zu)", 
                   end_count, start_count, deleted_count, target_count);
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
            lb_log("Cache cleaner started: max %zu items, check every %ds", max_cache_items, cleaner_delay_secs);

            while(!stop) {
                for(int i = 0; i < cleaner_delay_secs && !stop; i++)
                    this_thread::sleep_for(chrono::seconds(1));
                
                size_t current_count = get_cache_entry_count();
                if (current_count >= max_cache_items) {
                    lb_debug("Cache cleaner triggered: %zu items >= %zu max", current_count, max_cache_items);
                    trim();
                }
            }
            lb_log("Cache cleaner exit.");

        }
        
        void start() {
            cleaner_thread = new thread(&IndexCache::cache_cleaner, this);
        }
};
