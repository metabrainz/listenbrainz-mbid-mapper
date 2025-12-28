#include <stdio.h>
#include <thread>

#include "artist_index.hpp"
#include "indexer_thread.hpp"
#include "utils.hpp"
#include "SQLiteCpp.h"
#include "init.h"  // nmslib init

void print_usage() {
    lb_log("Usage: make_indexes [--skip-artists] [--force-rebuild]");
    lb_log("Options:");
    lb_log("  --skip-artists   Skip building artist indexes");
    lb_log("  --force-rebuild  Force rebuild all recording indexes (ignore cache)");
    lb_log("");
    lb_log("Required environment variables:");
    lb_log("  INDEX_DIR                         Directory containing mapping.db");
    lb_log("  CANONICAL_MUSICBRAINZ_DATA_CONNECT  PostgreSQL connection string (unless --skip-artists)");
    lb_log("");
    lb_log("Optional environment variables:");
    lb_log("  NUM_BUILD_THREADS                 Thread count (0 = num CPU cores, default: 0)");
}

int main(int argc, char *argv[])
{
    init_logging();
    load_env_file();  // Load .env file, env vars take precedence
    
    bool skip_artists = false;
    bool force_rebuild = false;
    
    // Parse arguments (options only, no positional arguments)
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--skip-artists") {
            skip_artists = true;
        } else if (arg == "--force-rebuild") {
            force_rebuild = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            lb_error("Error: Unknown option: %s", arg.c_str());
            print_usage();
            return -1;
        }
    }
    
    // Get required INDEX_DIR from environment
    const char* env_index_dir = std::getenv("INDEX_DIR");
    if (!env_index_dir || strlen(env_index_dir) == 0) {
        lb_error("Error: INDEX_DIR environment variable not set");
        print_usage();
        return -1;
    }
    string index_dir = env_index_dir;
    
    // Get optional NUM_BUILD_THREADS from environment
    int num_threads = 0;
    const char* env_num_threads = std::getenv("NUM_BUILD_THREADS");
    if (env_num_threads && strlen(env_num_threads) > 0) {
        num_threads = std::atoi(env_num_threads);
    }
    
    // Validate CANONICAL_MUSICBRAINZ_DATA_CONNECT is set (needed for artist index building)
    if (!skip_artists) {
        const char* db_connect = std::getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
        if (!db_connect || strlen(db_connect) == 0) {
            lb_error("Error: CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
            lb_error("This is required for building artist indexes. Use --skip-artists to skip.");
            return -1;
        }
    }
    
    // Validate flag combinations
    if (skip_artists && force_rebuild) {
        lb_error("Error: --skip-artists and --force-rebuild cannot be used together");
        print_usage();
        return -1;
    }
   
    // Clear cache if force rebuild is requested
    // Drop the index first, delete cache, then recreate index at the end - faster for bulk operations
    string db_file = index_dir + "/mapping.db";
    if (force_rebuild) {
        lb_log("force rebuild requested - dropping index and clearing cache");
        try {
            SQLite::Database db(db_file, SQLite::OPEN_READWRITE);
            db.exec("DROP INDEX IF EXISTS entity_id_idx");
            lb_log("entity_id_idx dropped");
            db.exec("DELETE FROM index_cache");
            lb_log("index cache cleared successfully");
        } catch (const std::exception& e) {
            lb_error("Error clearing index cache: %s", e.what());
            return -1;
        }
    }

    // Initialize nmslib once in main thread before ANY FuzzyIndex is created
    // This is necessary because initLibrary() is not thread-safe (it modifies global registries)
    similarity::initLibrary(0, LIB_LOGNONE, NULL);

    if (!skip_artists) {
        lb_log("build artist indexes");
        ArtistIndex *artist_index = new ArtistIndex(index_dir);
        artist_index->build();
        // clean up to free memory
        delete artist_index;
    } else {
        lb_log("skipping artist indexes (--skip-artists specified)");
    }


    // 0 means use number of CPU cores
    num_threads = (num_threads <= 0) ? std::thread::hardware_concurrency() : num_threads;
    if (num_threads <= 0) num_threads = 4;  // fallback if hardware_concurrency() fails
                                            
    lb_log("build recording indexes with %d threads", num_threads);
    IndexerThread mapping(index_dir, num_threads);
    mapping.build_recording_indexes();

    // Recreate the index after force rebuild
    if (force_rebuild) {
        lb_log("recreating entity_id_idx index");
        try {
            SQLite::Database db(db_file, SQLite::OPEN_READWRITE);
            db.exec("CREATE INDEX IF NOT EXISTS entity_id_idx ON index_cache(entity_id)");
            lb_log("entity_id_idx recreated successfully");
        } catch (const std::exception& e) {
            lb_error("Error recreating index: %s", e.what());
            return -1;
        }
    }

    return 0;
}
