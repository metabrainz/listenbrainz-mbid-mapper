#pragma once

#include <mutex>
#include <thread>
#include <atomic>
#include "defs.hpp"
#include "utils.hpp"

using namespace std;


// TODO Make this a config item
const int SLEEP_DELAY = 10;
const float CLEANING_TARGET_RATIO = 0.9;

struct CacheEntry {
    ReleaseRecordingIndex *data;
    atomic<int>            ref_count;
    time_t                 last_accessed;
    size_t                 estimated_size;   // estimated memory size in bytes
    
    CacheEntry(ReleaseRecordingIndex *d, size_t size = 0) 
        : data(d), ref_count(0), last_accessed(0), estimated_size(size) {}
};

class IndexCache {
    private:
        map<unsigned int, CacheEntry*>  index;
        mutex                           mtx;
        thread                         *cleaner_thread;
        atomic<size_t>                  total_estimated_size;  // total estimated bytes in cache
        size_t                          max_cache_bytes;       // max cache size in bytes
        size_t                          target_cache_bytes;    // target after cleaning
        atomic<bool>                    stop;

    public:

        // Memory usage is specified in MB
        IndexCache(int max_memory_mb) { 
            stop = false;
            cleaner_thread = nullptr;
            total_estimated_size = 0;
            max_cache_bytes = (size_t)max_memory_mb * 1024 * 1024;
            target_cache_bytes = (size_t)(max_cache_bytes * CLEANING_TARGET_RATIO);
            lb_log("Index cache created: max %zuMB, target %zuMB", 
                   max_cache_bytes / (1024*1024), target_cache_bytes / (1024*1024));
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
            total_estimated_size = 0;
        }
        
        size_t get_estimated_size() const {
            return total_estimated_size.load();
        }
        
        void
        trim() {
            size_t start_size = total_estimated_size.load();
            lb_log("Cache trim starting: %zuMB estimated, target %zuMB", 
                   start_size / (1024*1024), target_cache_bytes / (1024*1024));
            
            // Phase 1: Build candidate list with sizes - lock briefly
            vector<tuple<unsigned int, time_t, size_t>> candidates;
            {
                lock_guard<mutex> lock(mtx);
                for (const auto &item : index) {
                    if (item.second->ref_count == 0) {
                        candidates.push_back({item.first, item.second->last_accessed, item.second->estimated_size});
                    }
                }
                lb_log("  %zu candidates (ref_count==0) out of %zu total entries", 
                       candidates.size(), index.size());
            }
            
            // Phase 2: Sort candidates by last_accessed (oldest first) - no lock needed
            sort(candidates.begin(), candidates.end(), 
                 [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });
            
            // Phase 3: Delete items one at a time, acquiring lock briefly for each
            int deleted_count = 0;
            size_t freed_bytes = 0;
            for (const auto &candidate : candidates) {
                {
                    lock_guard<mutex> lock(mtx);
                    auto iter = index.find(std::get<0>(candidate));
                    // Re-check: entry still exists and ref_count still 0?
                    if (iter != index.end() && iter->second->ref_count == 0) {
                        size_t entry_size = iter->second->estimated_size;
                        delete iter->second->data;
                        delete iter->second;
                        index.erase(iter);
                        total_estimated_size -= entry_size;
                        freed_bytes += entry_size;
                        deleted_count++;
                    }
                }
                
                // Small delay to let other threads use the cache
                this_thread::sleep_for(chrono::milliseconds(1));
                
                // Check if we've freed enough
                if (total_estimated_size.load() <= target_cache_bytes) {
                    lb_log("Cache trim complete: %zuMB estimated (freed %zuMB, deleted %d entries)", 
                           total_estimated_size.load() / (1024*1024), freed_bytes / (1024*1024), deleted_count);
                    return;
                }
            }
            
            lb_log("Cache trim finished: %zuMB estimated (freed %zuMB, deleted %d entries, target was %zuMB)", 
                   total_estimated_size.load() / (1024*1024), freed_bytes / (1024*1024), 
                   deleted_count, target_cache_bytes / (1024*1024));
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
                size_t entry_size = data->estimated_memory_size;
                CacheEntry *entry = new CacheEntry(data, entry_size);
                entry->ref_count = 1;  // Caller is using it
                entry->last_accessed = chrono::system_clock::to_time_t(chrono::system_clock::now());
                index[artist_credit_id] = entry;
                total_estimated_size += entry_size;
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
            lb_log("Cache cleaner started: max %zuMB", max_cache_bytes / (1024*1024));

            while(!stop) {
                for(int i = 0; i < SLEEP_DELAY && !stop; i++)
                    this_thread::sleep_for(chrono::seconds(1));
                
                size_t current = total_estimated_size.load();
                if (current >= max_cache_bytes) {
                    lb_log("Cache cleaner triggered: %zuMB >= %zuMB max", 
                           current / (1024*1024), max_cache_bytes / (1024*1024));
                    trim();
                }
            }
        }
        
        void start() {
            cleaner_thread = new thread(&IndexCache::cache_cleaner, this);
        }
};
