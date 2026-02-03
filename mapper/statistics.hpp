#pragma once

#include <mutex>
#include <string>
#include "SQLiteCpp.h"
#include "utils.hpp"

class Statistics {
    private:
        std::mutex mtx;
        unsigned long num_200_requests;
        unsigned long num_400_requests;
        unsigned long num_404_requests;
        unsigned long num_500_requests;
        unsigned long num_503_requests;
        unsigned long cache_items;
        std::string last_updated_timestamp;

    public:
        Statistics() : num_200_requests(0), num_400_requests(0), num_404_requests(0), num_500_requests(0), num_503_requests(0), cache_items(0) {}

        void update(int delta_200, int delta_400, int delta_404, int delta_500, int delta_503) {
            std::lock_guard<std::mutex> lock(mtx);
            num_200_requests += delta_200;
            num_400_requests += delta_400;
            num_404_requests += delta_404;
            num_500_requests += delta_500;
            num_503_requests += delta_503;
        }
       
        void update_cache_items(unsigned long _cache_items) {
            cache_items = _cache_items;
        }

        void update_last_updated(const std::string& db_path) {
            try {
                SQLite::Database db(db_path, SQLite::OPEN_READONLY);
                SQLite::Statement query(db, "SELECT value FROM update_metadata WHERE key = 'last_updated'");
                
                std::lock_guard<std::mutex> lock(mtx);
                if (query.executeStep()) {
                    last_updated_timestamp = query.getColumn(0).getText();
                } else {
                    last_updated_timestamp = "";
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mtx);
                last_updated_timestamp = "";
            }
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

        unsigned long get_500_count() {
            std::lock_guard<std::mutex> lock(mtx);
            return num_500_requests;
        }

        unsigned long get_503_count() {
            std::lock_guard<std::mutex> lock(mtx);
            return num_503_requests;
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
            data += string("lbmapper_requests_handled{status=\"500\"} ") + to_string(num_500_requests) + "\n";
            data += string("lbmapper_requests_handled{status=\"503\"} ") + to_string(num_503_requests) + "\n";
            data += string("# HELP lbmapper_rss The current resident set size, measured in megabytes.\n");
            data += string("# TYPE lbmapper_rss gauge\n");
            data += string("lbmapper_rss ") + to_string(rss_size) + "\n";
            data += string("# HELP lbmapper_cache_items The current number of items in index cache.\n");
            data += string("# TYPE lbmapper_cache_items gauge\n");
            data += string("lbmapper_cache_items ") + to_string(cache_items) + "\n";
            
            if (!last_updated_timestamp.empty()) {
                data += string("# HELP lbmapper_last_updated The timestamp when the mapping database was last updated.\n");
                data += string("# TYPE lbmapper_last_updated gauge\n");
                
                // Convert ISO timestamp to epoch
                std::tm tm = {};
                std::istringstream ss(last_updated_timestamp);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                std::time_t epoch = std::mktime(&tm);
                
                data += string("lbmapper_last_updated ") + to_string(epoch) + string("\n");
            }
            
            return data;
        }

        void reset() {
            std::lock_guard<std::mutex> lock(mtx);
            num_200_requests = 0;
            num_400_requests = 0;
            num_404_requests = 0;
            num_500_requests = 0;
            num_503_requests = 0;
        }
        
};
