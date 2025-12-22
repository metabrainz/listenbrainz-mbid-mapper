#pragma once

#include <mutex>
#include "utils.hpp"

class Statistics {
    private:
        std::mutex mtx;
        unsigned long num_200_requests;
        unsigned long num_400_requests;
        unsigned long num_404_requests;
        unsigned long cache_items;

    public:
        Statistics() : num_200_requests(0), num_400_requests(0), num_404_requests(0), cache_items(0) {}

        void update(int delta_200, int delta_400, int delta_404) {
            std::lock_guard<std::mutex> lock(mtx);
            num_200_requests += delta_200;
            num_400_requests += delta_400;
            num_404_requests += delta_404;
        }
       
        // CALL THIS!
        void update_cache_items(unsigned long _cache_items) {
            cache_items = _cache_items;
        }

        unsigned long get_200_count() {
            std::lock_guard<std::mutex> lock(mtx);
            return num_200_requests;
        }

        unsigned long get_400_count() {
            std::lock_guard<std::mutex> lock(mtx);
            return num_400_requests;
        }

        unsigned long get_404_count() {
            std::lock_guard<std::mutex> lock(mtx);
            return num_404_requests;
        }
        
        string
        get_metrics() {
            std::lock_guard<std::mutex> lock(mtx);
            size_t rss_size = get_current_rss_mb();

            string data;
            data = string("# HELP lbmapper_requests_handled The total number of requests handled.\n");
            data += string("# TYPE lbmapper_requests_handled counter\n");
            data += string("lbmapper_requests_handled{status=\"200\"} ") + to_string(num_200_requests) + "\n";
            data += string("lbmapper_requests_handled{status=\"400\"} ") + to_string(num_400_requests) + "\n";
            data += string("lbmapper_requests_handled{status=\"404\"} ") + to_string(num_404_requests) + "\n";
            data += string("# HELP lbmapper_rss The current resident set size, measured in megabytes.\n");
            data += string("# TYPE lbmapper_rss gauge\n");
            data += string("lbmapper_rss ") + to_string(rss_size) + "\n";
            data += string("# HELP lbmapper_cache_items The current number of items in index cache.\n");
            data += string("# TYPE lbmapper_cache_items gauge\n");
            data += string("lbmapper_cache_items ") + to_string(cache_items) + "\n";
            
            return data;
        }

        void reset() {
            std::lock_guard<std::mutex> lock(mtx);
            num_200_requests = 0;
            num_400_requests = 0;
            num_404_requests = 0;
        }
        
};
