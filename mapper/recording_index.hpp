    #pragma once
#include <stdio.h>
#include <ctime>
#include <algorithm>
#include <thread>
#include <sstream>
#include "SQLiteCpp.h"
#include "fuzzy_index.hpp"
#include "encode.hpp"
#include "utils.hpp"
#include "artist_index.hpp"
#include "defs.hpp"
#include "cereal/archives/binary.hpp"

using namespace std;

// THis error message stems from artist who have only one recording that that recording has zero encodable characters.
// this will be fixed by have stupid recording/release indexes.
// artist_credit 4455221: Recording index build error: 'no index data provided.'
// id 0 text 0artist_credit 4455221: release index build error: 'no index data provided.'


// Destructor implementation for ReleaseRecordingIndex (declared in defs.hpp)
// Must be here because FuzzyIndex is only forward-declared in defs.hpp
ReleaseRecordingIndex::~ReleaseRecordingIndex() {
    delete recording_index;
    delete release_index;
    // stupid_recording_index and stupid_release_index are unique_ptr and auto-deleted
}

// Ratio to estimate in-memory size from on-disk (blob) size.
// In-memory structures (vectors, maps, nmslib indexes) are larger than 
// the serialized binary representation. This is an empirical estimate.
const float MEMORY_SIZE_RATIO = 3.0;

// Use UNION instead of OR for better index usage
const char *fetch_query = R"(
      SELECT artist_credit_id   
           , release_id  
           , release_artist_credit_id   
           , release_name   
           , recording_id  
           , recording_name 
           , score AS rank  
        FROM mapping
       WHERE artist_credit_id = ?
       UNION
      SELECT artist_credit_id   
           , release_id  
           , release_artist_credit_id   
           , release_name   
           , recording_id  
           , recording_name 
           , score AS rank  
        FROM mapping
       WHERE release_artist_credit_id = ?
    ORDER BY rank, release_id
)";

const char *fetch_recording_aliases_query = R"(
      SELECT r.id
           , ra.name
        FROM recording r
        JOIN recording_alias ra
          ON ra.recording = r.id
)";

class RecordingIndex {
    private:
        string                         index_dir, db_file; 
        EncodeSearchData               encode;
        map<unsigned int, set<string>> recording_aliases;

    public:

        RecordingIndex(const string &index_dir_) {
            index_dir = index_dir_;
            db_file = index_dir_ + string("/mapping.db");
        }
        
        ~RecordingIndex() {
        }
       
        void
        load_recording_aliases() {
            lb_log("load recording aliases");
            try
            {
                PGconn     *conn;
                PGresult   *res;
                
                const char* db_connect = std::getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
                if (!db_connect || strlen(db_connect) == 0) {
                    throw std::runtime_error("CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
                }
                conn = PQconnectdb(db_connect);
                if (PQstatus(conn) != CONNECTION_OK) {
                    lb_error("Connection to database failed: %s", PQerrorMessage(conn));
                    PQfinish(conn);
                    throw std::runtime_error("PostgreSQL connection failed");
                }
               
                res = PQexec(conn, fetch_recording_aliases_query);
                if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                    std::string error_msg = "Query failed: " + std::string(PQerrorMessage(conn));
                    PQclear(res);
                    PQfinish(conn);
                    throw std::runtime_error(error_msg);
                }

                for (int i = 0; i < PQntuples(res); i++) {
                    unsigned int recording_id = atoi(PQgetvalue(res, i, 0));
                    
                    // TODO: add this to stupid recording index
                    string encoded = encode.encode_string(PQgetvalue(res, i, 1));
                    if (encoded.size())
                        recording_aliases[recording_id].insert(encoded);
                }
                
                // Clear the PGresult object to free memory
                PQclear(res);
                PQfinish(conn);
            }
            catch (exception& e)
            {
                printf("build recording aliases db exception: %s\n", e.what());
            }
        }

        ReleaseRecordingIndex *
        build_recording_release_indexes(unsigned int artist_credit_id, SQLite::Database &db) {
            using namespace std::chrono;
            
            // Thread-local timing accumulators
            thread_local long long total_query_us = 0;
            thread_local long long total_encode_us = 0;
            thread_local long long total_fuzzy_build_us = 0;
            thread_local long long total_calls = 0;
            
            auto t0 = high_resolution_clock::now();

            // Map to track release and recording strings and their indexes 
            map<string, unsigned int>                        release_string_index_map, recording_string_index_map;
            map<string, unsigned int>                        recording_name_to_id_map; // Track first recording_id for each encoded name
            map<string, unsigned int>                        release_name_to_id_map;   // Track first release_id for each encoded name
            map<unsigned int, vector<ReleaseRecordingLink>>  links;
            
            // Data for stupid indexes (items that encode to empty strings)
            map<string, unsigned int>                        stupid_recording_string_index_map;
            map<string, unsigned int>                        stupid_release_string_index_map;
            
            int row_count = 0;
            auto t1 = high_resolution_clock::now();
                
            try
            {
                SQLite::Statement   query(db, fetch_query);
          
                query.bind(1, artist_credit_id);
                query.bind(2, artist_credit_id);
                while (query.executeStep()) {
                    row_count++;
                    unsigned int ac_id = query.getColumn(0);
                    unsigned int release_id = query.getColumn(1);
                    unsigned int release_artist_credit_id = query.getColumn(2);
                    string       release_name = query.getColumn(3);
                    unsigned int recording_id = query.getColumn(4);
                    string       recording_name = query.getColumn(5);
                    unsigned int rank  = query.getColumn(6);
                   
                    // Include rows where either the recording artist_credit_id or release artist_credit_id matches
                    if (artist_credit_id != ac_id && artist_credit_id != release_artist_credit_id)
                        continue;
                    
                    string encoded_release_name = encode.encode_string(release_name);
                    string encoded_recording_name = encode.encode_string(recording_name);
                    
                    // Handle recording that encodes to empty - add to stupid index but still create links
                    bool recording_is_stupid = false;
                    if (encoded_recording_name.size() == 0) {
                        // Try stupid encoding
                        string stupid_encoded = encode.encode_string_keep_non_word(recording_name);
                        if (stupid_encoded.size() > 0) {
                            if (stupid_recording_string_index_map.find(stupid_encoded) == stupid_recording_string_index_map.end()) {
                                stupid_recording_string_index_map[stupid_encoded] = recording_id;
                            }
                            // Use the stupid encoding for the normal index too, so we can create links
                            encoded_recording_name = stupid_encoded;
                            recording_is_stupid = true;
                        } else {
                            continue;  // No encoding possible, skip this recording
                        }
                    }
                    
                    // Handle release that encodes to empty - add to stupid index but still create links
                    bool release_is_stupid = false;
                    if (encoded_release_name.size() == 0) {
                        // Try stupid encoding
                        string stupid_encoded = encode.encode_string_keep_non_word(release_name);
                        if (stupid_encoded.size() > 0) {
                            if (stupid_release_string_index_map.find(stupid_encoded) == stupid_release_string_index_map.end()) {
                                stupid_release_string_index_map[stupid_encoded] = release_id;
                            }
                        }
                        release_is_stupid = true;
                        // Don't continue - we still need to create links, but skip adding to normal release index
                    }
                    
                    unsigned int release_index;
                    if (!release_is_stupid) {
                        try {
                            release_index = release_string_index_map.at(encoded_release_name);
                        } catch (const std::out_of_range& e) {
                            release_index = release_string_index_map.size();
                            release_string_index_map[encoded_release_name] = release_index;
                            // Store the first release_id we encounter for this encoded name
                            release_name_to_id_map[encoded_release_name] = release_id;
                        }
                    } else {
                        // For stupid releases, use a dummy index value (we won't use it for searching)
                        release_index = 0;
                    }

                    unsigned int recording_index;
                    try {
                        recording_index = recording_string_index_map.at(encoded_recording_name);
                    } catch (const std::out_of_range& e) {
                        recording_index = recording_string_index_map.size();
                        recording_string_index_map[encoded_recording_name] = recording_index;
                        // Store the first recording_id we encounter for this encoded name
                        recording_name_to_id_map[encoded_recording_name] = recording_id;
                    } 
                    
                    ReleaseRecordingLink link = { release_index, release_id, rank, recording_index, recording_id };
                    links[recording_index].push_back(link);
                }
            }
            catch (std::exception& e)
            {
                lb_error("build rec index db exception: %s", e.what());
            }
            
            auto t2 = high_resolution_clock::now();
            total_query_us += duration_cast<microseconds>(t2 - t1).count();
            
            vector<string>       recording_texts(recording_string_index_map.size());
            vector<unsigned int> recording_ids(recording_string_index_map.size());
            // Map from recording_id to its original index (for linking aliases)
            map<unsigned int, unsigned int> recording_id_to_index;
            for(auto &it : recording_string_index_map) {
                recording_texts[it.second] = it.first;
                // Use the actual recording_id from the database, not the index
                unsigned int rec_id = recording_name_to_id_map[it.first];
                recording_ids[it.second] = rec_id;
                // Store the first index we see for each recording_id
                if (recording_id_to_index.find(rec_id) == recording_id_to_index.end()) {
                    recording_id_to_index[rec_id] = it.second;
                }
            }
            
            // Add recording aliases to the index
            // Collect unique recording_ids from our data
            set<unsigned int> unique_recording_ids;
            for(auto &it : recording_name_to_id_map) {
                unique_recording_ids.insert(it.second);
            }
            
            // For each recording_id, add its aliases if they exist
            for(unsigned int rec_id : unique_recording_ids) {
                auto alias_it = recording_aliases.find(rec_id);
                if (alias_it != recording_aliases.end()) {
                    for(const string &alias : alias_it->second) {
                        // Only add if this alias text is not already in the index
                        if (recording_string_index_map.find(alias) == recording_string_index_map.end()) {
                            unsigned int new_index = recording_texts.size();
                            recording_texts.push_back(alias);
                            recording_ids.push_back(rec_id);
                            
                            // Copy links from the original recording to this alias
                            auto orig_idx_it = recording_id_to_index.find(rec_id);
                            if (orig_idx_it != recording_id_to_index.end()) {
                                auto link_it = links.find(orig_idx_it->second);
                                if (link_it != links.end()) {
                                    links[new_index] = link_it->second;
                                }
                            }
                        }
                    }
                }
            }
            
            auto t3 = high_resolution_clock::now();
            total_encode_us += duration_cast<microseconds>(t3 - t2).count();

            FuzzyIndex *recording_index = new FuzzyIndex();
            if (recording_texts.size() > 0) {
                try
                {
                    recording_index->build(recording_ids, recording_texts);
                }
                catch(const std::exception& e)
                {
                    lb_error("artist_credit %d: Recording index build error: '%s'", artist_credit_id, e.what());
                }
            }

            vector<string>       release_texts(release_string_index_map.size());
            vector<unsigned int> release_ids(release_string_index_map.size());
            for(auto &it : release_string_index_map) {
                release_texts[it.second] = it.first;
                // Use the actual release_id from the database, not the index
                release_ids[it.second] = release_name_to_id_map[it.first];
            }
            FuzzyIndex *release_index = new FuzzyIndex();
            if (release_texts.size() > 0) {
                try
                {
                    release_index->build(release_ids, release_texts);
                }
                catch(const std::exception& e)
                {
                    lb_error("artist_credit %d: release index build error: '%s'", artist_credit_id, e.what());
                }
            }
            
            // Build stupid indexes from the collected data
            vector<string>       stupid_recording_texts;
            vector<unsigned int> stupid_recording_ids;
            vector<string>       stupid_release_texts;
            vector<unsigned int> stupid_release_ids;
            
            for (auto &it : stupid_recording_string_index_map) {
                stupid_recording_texts.push_back(it.first);
                stupid_recording_ids.push_back(it.second);
            }
            
            for (auto &it : stupid_release_string_index_map) {
                stupid_release_texts.push_back(it.first);
                stupid_release_ids.push_back(it.second);
            }
            
            // Build stupid recording index only if we have data
            std::unique_ptr<FuzzyIndex> stupid_recording_index;
            if (stupid_recording_texts.size() > 0) {
                stupid_recording_index = std::make_unique<FuzzyIndex>();
                try
                {
                    stupid_recording_index->build(stupid_recording_ids, stupid_recording_texts);
                }
                catch(const std::exception& e)
                {
                    lb_error("artist_credit %d: Stupid recording index build error: '%s'", artist_credit_id, e.what());
                    stupid_recording_index.reset();
                }
            }
            
            // Build stupid release index only if we have data
            std::unique_ptr<FuzzyIndex> stupid_release_index;
            if (stupid_release_texts.size() > 0) {
                stupid_release_index = std::make_unique<FuzzyIndex>();
                try
                {
                    stupid_release_index->build(stupid_release_ids, stupid_release_texts);
                }
                catch(const std::exception& e)
                {
                    lb_error("artist_credit %d: Stupid release index build error: '%s'", artist_credit_id, e.what());
                    stupid_release_index.reset();
                }
            }
            
            auto t4 = high_resolution_clock::now();
            total_fuzzy_build_us += duration_cast<microseconds>(t4 - t3).count();
            
            // Sort each vector of ReleaseRecordingLink by release_id
            for (auto& pair : links) {
                sort(pair.second.begin(), pair.second.end(), 
                     [](const ReleaseRecordingLink& a, const ReleaseRecordingLink& b) {
                         return a.release_id < b.release_id;
                     });
            }
            
            total_calls++;
            // Print timing stats every 100 calls
            if (total_calls % 100 == 0) {
                lb_log("  [thread timing] calls=%lld query=%.1fms encode=%.1fms fuzzy=%.1fms (avg per call: q=%.2fms e=%.2fms f=%.2fms)",
                       total_calls,
                       total_query_us / 1000.0, total_encode_us / 1000.0, total_fuzzy_build_us / 1000.0,
                       (double)total_query_us / total_calls / 1000.0,
                       (double)total_encode_us / total_calls / 1000.0,
                       (double)total_fuzzy_build_us / total_calls / 1000.0);
            }
            
            return new ReleaseRecordingIndex(recording_index, release_index, std::move(stupid_recording_index), std::move(stupid_release_index), links);
        }

        // Convenience overload that opens its own connection (for backward compatibility)
        ReleaseRecordingIndex *
        build_recording_release_indexes(unsigned int artist_credit_id) {
            SQLite::Database db(db_file, SQLite::OPEN_READONLY);
            return build_recording_release_indexes(artist_credit_id, db);
        }

        // Load with external DB connection (for connection reuse in server)
        ReleaseRecordingIndex *
        load(const int artist_credit_id, SQLite::Database &db) {
            FuzzyIndex                   *recording_index = new FuzzyIndex();
            FuzzyIndex                   *release_index = new FuzzyIndex();
            std::unique_ptr<FuzzyIndex>  stupid_recording_index;
            std::unique_ptr<FuzzyIndex>  stupid_release_index;
            map<unsigned int, vector<ReleaseRecordingLink>>  links;
            try
            {
                SQLite::Statement     query(db, fetch_blob_query);
            
                query.bind(1, artist_credit_id);
                if (query.executeStep()) {
                    const void* blob_data = query.getColumn(0).getBlob();
                    size_t blob_size = query.getColumn(0).getBytes();
                    
                    std::stringstream ss;
                    ss.write(static_cast<const char*>(blob_data), blob_size);
                    ss.seekg(ios_base::beg);
                    
                    {
                        cereal::BinaryInputArchive iarchive(ss);
                        iarchive(*recording_index, *release_index);
                        
                        // Check if there's more data for stupid indexes (new format)
                        // cereal handles unique_ptr serialization automatically
                        iarchive(stupid_recording_index, stupid_release_index);
                        iarchive(links);
                    }

                    
                    // Estimate in-memory size from blob size
                    size_t estimated_memory = (size_t)(blob_size * MEMORY_SIZE_RATIO);
                    return new ReleaseRecordingIndex(recording_index, release_index, std::move(stupid_recording_index), std::move(stupid_release_index), links, estimated_memory);
                } else {
                    //lb_error("Cannot load index for %d", artist_credit_id);
                    delete recording_index;
                    delete release_index;
                    // unique_ptr auto-deleted
                    return nullptr;
                }
            }
            catch (std::exception& e)
            {
                lb_error("load rec index db exception: %s", e.what());
                delete recording_index;
                delete release_index;
                // unique_ptr auto-deleted
            }
            return nullptr;
        }

        // Load with internal DB connection (for standalone tools)
        ReleaseRecordingIndex *
        load(const int artist_credit_id) {
            SQLite::Database db(db_file, SQLite::OPEN_READONLY);
            return load(artist_credit_id, db);
        }
        
        /**
         * Build and serialize index for a single artist_credit_id (for incremental updates).
         * Returns the serialized blob ready for storage in index_cache.
         */
        string build_index_for_update(unsigned int artist_credit_id, SQLite::Database& db) {
            auto index = build_recording_release_indexes(artist_credit_id, db);
            
            stringstream ss;
            {
                cereal::BinaryOutputArchive oarchive(ss);
                oarchive(*index->recording_index);
                oarchive(*index->release_index);
                
                // cereal handles unique_ptr serialization automatically
                oarchive(index->stupid_recording_index, index->stupid_release_index);
                oarchive(index->links);
            }
            
            delete index;
            return ss.str();
        }
        
        /**
         * Build indexes for a set of artist_credit_ids using parallel threads (for incremental updates).
         * 
         * @param artist_credit_ids Set of artist_credit_ids to build indexes for
         * @param num_threads Number of parallel threads (default 4)
         * @return Map of artist_credit_id → serialized blob
         */
        map<int, string> build_indexes_for_update(const set<int>& artist_credit_ids, int num_threads = 4) {
            map<int, string> results;
            
            if (artist_credit_ids.empty()) {
                return results;
            }
            
            // Filter out Various Artists and other special artist_credits
            vector<int> ids;
            ids.reserve(artist_credit_ids.size());
            for (int id : artist_credit_ids) {
                if (id > VARIOUS_ARTISTS_ARTIST_CREDIT_ID) {
                    ids.push_back(id);
                }
            }
            
            if (ids.empty()) {
                lb_log("No artist_credit_ids to process after filtering");
                return results;
            }
            
            lb_log("Building indexes for %zu artist_credit_ids using %d threads...", 
                   ids.size(), num_threads);
            
            // Thread result structure
            struct ThreadResult {
                unsigned int artist_credit_id;
                string blob;
                bool success;
            };
            
            vector<thread> threads;
            vector<ThreadResult> thread_results(ids.size());
            
            // Thread worker lambda
            auto thread_worker = [this](unsigned int artist_id, ThreadResult* result) {
                thread_local unique_ptr<SQLite::Database> tl_db;
                
                if (!tl_db) {
                    tl_db = make_unique<SQLite::Database>(db_file, SQLite::OPEN_READONLY);
                    tl_db->exec("PRAGMA cache_size=-65536;");
                    tl_db->exec("PRAGMA mmap_size=268435456;");
                }
                
                result->artist_credit_id = artist_id;
                result->success = true;
                
                try {
                    result->blob = build_index_for_update(artist_id, *tl_db);
                } catch (const exception& e) {
                    lb_error("Index build failed for artist_credit_id %u: %s", artist_id, e.what());
                    result->success = false;
                }
            };
            
            size_t next_idx = 0;
            size_t completed = 0;
            auto start_time = chrono::steady_clock::now();
            
            while (completed < ids.size()) {
                // Start new threads while we have capacity
                while (threads.size() < (size_t)num_threads && next_idx < ids.size()) {
                    thread_results[next_idx].artist_credit_id = ids[next_idx];
                    threads.emplace_back(thread_worker, ids[next_idx], &thread_results[next_idx]);
                    next_idx++;
                }
                
                // Wait for threads to complete
                for (auto& t : threads) {
                    if (t.joinable()) {
                        t.join();
                    }
                }
                
                // Collect results
                for (size_t i = completed; i < next_idx; i++) {
                    if (thread_results[i].success) {
                        results[thread_results[i].artist_credit_id] = thread_results[i].blob;
                    }
                }
                
                completed = next_idx;
                threads.clear();
                
                // Progress reporting
                if (completed % 100 == 0 && completed > 0) {
                    auto now = chrono::steady_clock::now();
                    double elapsed = chrono::duration<double>(now - start_time).count();
                    double items_per_sec = completed / elapsed;
                    double remaining = (ids.size() - completed) / items_per_sec;
                    printf("  Building indexes: %zu/%zu (%.1f/s, ETA: %.0fs)     \r", 
                           completed, ids.size(), items_per_sec, remaining);
                    fflush(stdout);
                }
            }
            
            printf("\n");
            lb_log("Built %zu indexes", results.size());
            
            return results;
        }
};
