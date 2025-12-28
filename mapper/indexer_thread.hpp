#pragma once

#include <unistd.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "fuzzy_index.hpp"
#include "recording_index.hpp"
#include "index_cache.hpp"
#include "SQLiteCpp.h"

using namespace std;

const char *fetch_pending_artists_query = R"(
    WITH artist_ids AS (
            SELECT DISTINCT mapping.artist_credit_id
              FROM mapping
         LEFT JOIN index_cache
                ON mapping.artist_credit_id = index_cache.entity_id
             WHERE index_cache.entity_id is null
    )
            SELECT artist_credit_id, count(*) as cnt 
              FROM artist_ids 
             WHERE artist_credit_id > 2 
          GROUP BY artist_credit_id)";

class CreatorThread {
    public:
        unsigned int     artist_id;
        std::atomic<bool> done;
        stringstream    *sstream;
        thread          *th;
        
        CreatorThread() : artist_id(0), done(false), sstream(nullptr), th(nullptr) {
        }
};

// Thread-local database connection for reading during index building
// Using thread_local avoids the overhead of opening/closing connections per artist
void thread_build_index(RecordingIndex *ri, CreatorThread *th, unsigned int artist_id, const string &db_file,
                        const vector<string> *release_ngrams = nullptr, const vector<string> *recording_ngrams = nullptr) {
    // Thread-local connection: opened once per thread, reused for all artists processed by this thread
    thread_local unique_ptr<SQLite::Database> tl_db;
    
    if (!tl_db) {
        tl_db = make_unique<SQLite::Database>(db_file, SQLite::OPEN_READONLY);
        // Optimize for read-heavy workload
        tl_db->exec("PRAGMA cache_size=-65536;");  // 64MB cache per thread
        tl_db->exec("PRAGMA mmap_size=268435456;"); // 256MB mmap per thread
    }
    
    th->sstream = new stringstream();
    
    auto index = ri->build_recording_release_indexes(artist_id, *tl_db, release_ngrams, recording_ngrams);
    {
        cereal::BinaryOutputArchive oarchive(*th->sstream);
        oarchive(*index->recording_index);
        oarchive(*index->release_index);
        oarchive(index->stupid_recording_index, index->stupid_release_index);
        oarchive(index->links);
    }
    // Note: index->recording_index and index->release_index are deleted by
    // ReleaseRecordingIndex destructor when 'index' is deleted
    delete index;
    th->sstream->seekg(ios_base::end);
    th->sstream->seekg(ios_base::beg);
    th->done.store(true, std::memory_order_release);
}
    
class IndexerThread {
    private:
        string                  index_dir, db_file;
        int                     num_threads;
        const vector<string>    *release_ngrams;
        const vector<string>    *recording_ngrams;

    public:

        IndexerThread(const string &_index_dir, int _num_threads,
                     const vector<string> *_release_ngrams = nullptr, const vector<string> *_recording_ngrams = nullptr) 
            : release_ngrams(_release_ngrams), recording_ngrams(_recording_ngrams) { 
            index_dir = _index_dir;
            db_file = _index_dir + "/mapping.db";
            num_threads = _num_threads;
        }
        
        ~IndexerThread() {
        }
        
        void write_indexes_to_db(SQLite::Database &db, vector<CreatorThread *> &data) {
            
            SQLite::Transaction transaction(db);
            for(auto &entry : data) {
                SQLite::Statement query(db, insert_blob_query);
           
                query.bind(1, entry->artist_id);
                query.bind(2, (const char *)entry->sstream->str().c_str(), (int32_t)entry->sstream->str().length());
                query.exec();
            }
            transaction.commit();
        }
        
        void build_recording_indexes() { 
            deque<unsigned int> artist_ids;  // deque for O(1) pop_front
            try
            {
                SQLite::Database    db(db_file, SQLite::OPEN_READWRITE);
                SQLite::Statement   query(db, fetch_pending_artists_query);

                // Optimize SQLite for bulk writes
                db.exec("PRAGMA journal_mode=WAL;");
                db.exec("PRAGMA synchronous=NORMAL;");
                db.exec("PRAGMA cache_size=-262144;");  // 256MB cache
                db.exec("PRAGMA temp_store=MEMORY;");
               
                


                while (query.executeStep())
                    artist_ids.push_back(query.getColumn(0));

                lb_log("Build indexes for %zu artists", artist_ids.size());
                vector<CreatorThread *> threads;
                vector<CreatorThread *> data_to_commit;
                unsigned int count = 0;
                unsigned int total_count = artist_ids.size();
                
                RecordingIndex recording_index(index_dir);
                recording_index.load_recording_aliases();
                
                auto start_time = chrono::steady_clock::now();
                lb_log("Using %d threads", num_threads);
                
                while(artist_ids.size() || threads.size()) {
                    // Check for completed threads - iterate backwards for safe removal
                    for(int i = threads.size() - 1; i >= 0; i--) {
                        CreatorThread *th = threads[i];
                        if (th->done.load(std::memory_order_acquire)) {
                            th->th->join();
                            delete th->th;
                            th->th = nullptr;
                            data_to_commit.push_back(th);
                            threads.erase(threads.begin()+i);
                        }
                    }
                    
                    // Commit when we have enough data
                    if (data_to_commit.size() >= NUM_ROWS_PER_COMMIT) {
                        write_indexes_to_db(db, data_to_commit);
                        for(auto &entry : data_to_commit) {
                            delete entry->sstream;
                            delete entry;
                        }
                        data_to_commit.clear();
                    }
                    
                    // Start new threads while we have capacity and work to do
                    while (artist_ids.size() && threads.size() < (size_t)num_threads) {
                        CreatorThread *newthread = new CreatorThread();
                        unsigned int artist_id = artist_ids.front();
                        artist_ids.pop_front();  // O(1) for deque
                        newthread->done.store(false, std::memory_order_release);
                        newthread->artist_id = artist_id;
                        newthread->th = new thread(thread_build_index, &recording_index, newthread, artist_id, db_file,
                                                  release_ngrams, recording_ngrams); 
                        threads.push_back(newthread);
                        count++;
                    }
                    
                    // Progress reporting every 100 items
                    if ((count % 100) == 0 && count > 0) {
                        auto now = chrono::steady_clock::now();
                        double elapsed = chrono::duration<double>(now - start_time).count();
                        double items_per_sec = count / elapsed;
                        double remaining = (total_count - count) / items_per_sec;
                        printf("%d%% complete %.1f items/s (%u/%u) ETA: %.0fs     \r", 
                               (int)(count * 100/total_count), items_per_sec, count, total_count, remaining);
                        fflush(stdout);
                    }
                    
                    // Brief sleep only if all thread slots are full and none are done
                    if (threads.size() >= (size_t)num_threads) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
                
                // Final commit
                write_indexes_to_db(db, data_to_commit);
                for(auto &entry : data_to_commit) {
                    delete entry->sstream;
                    delete entry;
                }                                
                data_to_commit.clear();

                lb_log("indexed %lu rows                             ", count);
            }
            catch (std::exception& e)
            {
                printf("db exception: %s\n", e.what());
            }
            
        }
};
