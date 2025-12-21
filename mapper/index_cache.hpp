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
    
    CacheEntry(ReleaseRecordingIndex *d) : data(d), ref_count(0), last_accessed(0) {}
};

class IndexCache {
    private:
        map<unsigned int, CacheEntry*>  index;
        mutex                           mtx;
        thread                         *cleaner_thread;
        long                            baseline;         // memory usage before cache starts
        int                             max_memory_usage; // in MB
        int                             cleaning_target;  // in MB
        atomic<bool>                    stop;

    public:

        // Memory usage is specified in MB
        IndexCache(int max_memory_usage_) { 
            stop = false;
            cleaner_thread = nullptr;
            baseline = 0;
            max_memory_usage = max_memory_usage_;
            cleaning_target = (int)(max_memory_usage * CLEANING_TARGET_RATIO);
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
        save_baseline() {
            baseline = get_memory_footprint();  // Capture just before we start serving
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
        
        long get_memory_footprint() {
            std::ifstream status_file("/proc/self/status");
            std::string line;
            while (std::getline(status_file, line)) {
                if (line.rfind("VmRSS:", 0) == 0) {
                    std::string value;
                    std::istringstream iss(line);
                    std::string key, unit;
                    long rss_kb;
                    iss >> key >> rss_kb >> unit;
                    return rss_kb / 1024; // return a value in MB
                }
            }
            assert(false);
        }
        
        void
        trim() {
            long start_use = get_memory_footprint();
            long start_cache_use = start_use - baseline;
            lb_log("Cache trim starting: %ldMB total (%ldMB cache), target %dMB cache", 
                   start_use, start_cache_use, cleaning_target);
            
            // Phase 1: Build candidate list - lock briefly for each item
            // This is a snapshot - entries may change state before we try to delete them
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
            // Add small delay between deletions to let other threads work
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
                
                // Check memory outside the lock - compare cache usage (relative to baseline)
                long current_use = get_memory_footprint();
                long current_cache_use = current_use - baseline;
                if (current_cache_use <= cleaning_target) {
                    lb_log("Cache trim complete: %ldMB cache (freed %ldMB, deleted %d entries)", 
                           current_cache_use, start_cache_use - current_cache_use, deleted_count);
                    return;
                }
            }
            
            long end_use = get_memory_footprint();
            long end_cache_use = end_use - baseline;
            lb_log("Cache trim finished: %ldMB cache (freed %ldMB, deleted %d entries, target was %dMB)", 
                   end_cache_use, start_cache_use - end_cache_use, deleted_count, cleaning_target);
        }
        
        // Cache takes ownership of data. Returns the cached pointer (which may differ from input
        // if another thread already added this entry). Returned pointer has ref_count incremented.
        // Caller MUST call release() when done.
        ReleaseRecordingIndex *
        add(unsigned int artist_credit_id, ReleaseRecordingIndex *data) {
            
            if (baseline == 0)
                save_baseline();
                
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
            lb_log("Cache cleaner started");

            while(!stop) {
                for(int i = 0; i < SLEEP_DELAY && !stop; i++)
                    this_thread::sleep_for(chrono::seconds(1));
                
                long current = get_memory_footprint();
                long cache_use = current - baseline;
                if (cache_use >= max_memory_usage) 
                    trim();
            }
        }
        
        void start() {
            cleaner_thread = new thread(&IndexCache::cache_cleaner, this);
        }
};