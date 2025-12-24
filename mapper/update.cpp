#include <cstdlib>
#include <cstring>

#include <libpq-fe.h>

#include "utils.hpp"
#include "defs.hpp"
#include "SQLiteCpp.h"
#include "changed_data.hpp"
#include "canonical_release_updater.hpp"
#include "mapping_update_fetcher.hpp"
#include "recording_index.hpp"
#include "mapping_batch_updater.hpp"
#include "artist_index.hpp"
#include "init.h"  // nmslib init

using namespace std;

// Batch size for processing artist_credit_ids
constexpr size_t BATCH_SIZE = 5000;

void print_usage() {
    lb_log("Usage: update [options]");
    lb_log("");
    lb_log("Options:");
    lb_log("  --help, -h       Show this help message");
    lb_log("  --dry-run        Show what would be updated without making changes (default)");
    lb_log("  --apply          Actually apply the changes");
    lb_log("");
    lb_log("Required environment variables:");
    lb_log("  INDEX_DIR                         Directory containing mapping.db");
    lb_log("  CANONICAL_MUSICBRAINZ_DATA_CONNECT  PostgreSQL connection string");
}

int main(int argc, char* argv[]) {
    init_logging();
    load_env_file();
    
    // Initialize nmslib once in main thread before any FuzzyIndex is created
    similarity::initLibrary(0, LIB_LOGNONE, NULL);
    
    bool dry_run = true;  // Default to dry-run for safety
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--apply") {
            dry_run = false;
        } else {
            lb_error("Unknown option: %s", arg.c_str());
            print_usage();
            return 1;
        }
    }
    
    const char* env_index_dir = getenv("INDEX_DIR");
    if (!env_index_dir || strlen(env_index_dir) == 0) {
        lb_error("INDEX_DIR environment variable not set");
        return 1;
    }
    
    string index_dir = env_index_dir;
    lb_log("INDEX_DIR: %s", index_dir.c_str());
    lb_log("Mode: %s", dry_run ? "DRY RUN (use --apply to make changes)" : "APPLY (changes will be made)");
    
    const char* pg_connect = getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
    if (!pg_connect || strlen(pg_connect) == 0) {
        lb_error("CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
        return 1;
    }
    
    // Connect to PostgreSQL
    lb_log("Connecting to PostgreSQL...");
    PGconn* pg_conn = PQconnectdb(pg_connect);
    if (PQstatus(pg_conn) != CONNECTION_OK) {
        lb_error("PostgreSQL connection failed: %s", PQerrorMessage(pg_conn));
        PQfinish(pg_conn);
        return 1;
    }
    
    try {
        // Open SQLite database
        string db_file = index_dir + "/mapping.db";
        lb_log("Opening SQLite database: %s", db_file.c_str());
        SQLite::Database db(db_file, SQLite::OPEN_READWRITE);
        
        // Step 1: Collect changed data
        lb_log("=== Step 1: Collecting changed data ===");
        MusicBrainzChangedData changed_data(pg_conn, db);
        
        if (!changed_data.collect()) {
            lb_error("Failed to collect changed data");
            PQfinish(pg_conn);
            return 1;
        }
        
        changed_data.print_summary();
        
        if (!changed_data.has_changes()) {
            lb_log("No changes detected, nothing to update");
            changed_data.save_current_timestamp();
            PQfinish(pg_conn);
            return 0;
        }
        
        if (dry_run) {
            lb_log("=== DRY RUN - No changes will be made ===");
            lb_log("Would update:");
            lb_log("  - %zu release_groups in canonical_release table", 
                   changed_data.get_changed_release_groups().size());
            lb_log("  - %zu artist_credit_ids in mapping table and indexes",
                   changed_data.get_changed_artist_credit_ids().size());
            PQfinish(pg_conn);
            return 0;
        }
        
        // Step 2: Update canonical_release table in PostgreSQL
        lb_log("=== Step 2: Updating canonical_release table ===");
        const auto& changed_release_groups = changed_data.get_changed_release_groups();
        
        if (!changed_release_groups.empty()) {
            if (!update_canonical_releases(pg_conn, changed_release_groups)) {
                lb_error("Failed to update canonical_release table");
                PQfinish(pg_conn);
                return 1;
            }
        } else {
            lb_log("No release_groups to update in canonical_release table");
        }
        
        // Step 3: Update mapping and indexes in batches
        lb_log("=== Step 3: Updating mapping table and indexes in batches ===");
        const auto& changed_artist_credits = changed_data.get_changed_artist_credit_ids();
        
        if (!changed_artist_credits.empty()) {
            // Convert set to vector for batching
            vector<int> ac_ids(changed_artist_credits.begin(), changed_artist_credits.end());
            
            size_t total = ac_ids.size();
            size_t num_batches = (total + BATCH_SIZE - 1) / BATCH_SIZE;
            
            lb_log("Processing %zu artist_credit_ids in %zu batches of %zu",
                   total, num_batches, BATCH_SIZE);
            
            MappingUpdateFetcher fetcher(pg_conn);
            RecordingIndex recording_index(index_dir);
            recording_index.load_recording_aliases();
            MappingBatchUpdater updater(db);
            
            for (size_t batch_num = 0; batch_num < num_batches; batch_num++) {
                size_t start_idx = batch_num * BATCH_SIZE;
                size_t end_idx = min(start_idx + BATCH_SIZE, total);
                
                // Build set of IDs for this batch
                set<int> batch_ids(ac_ids.begin() + start_idx, ac_ids.begin() + end_idx);
                
                lb_log("");
                lb_log("--- Batch %zu/%zu (%zu IDs) ---", 
                       batch_num + 1, num_batches, batch_ids.size());
                
                // Step 3a: Fetch mapping data from PostgreSQL
                lb_log("Fetching mapping data from PostgreSQL...");
                vector<MappingRowData> mapping_rows = fetcher.fetch(batch_ids);
                lb_log("Fetched %zu mapping rows", mapping_rows.size());
                
                // Step 3b: Build fuzzy indexes
                lb_log("Building fuzzy indexes...");
                map<int, string> index_blobs = recording_index.build_indexes_for_update(batch_ids);
                lb_log("Built %zu index blobs", index_blobs.size());
                
                // Step 3c: Update SQLite atomically
                lb_log("Updating SQLite database...");
                if (!updater.update(batch_ids, mapping_rows, index_blobs)) {
                    lb_error("Batch %zu failed - stopping update", batch_num + 1);
                    lb_error("Previous batches have been committed, but remaining batches skipped");
                    PQfinish(pg_conn);
                    return 1;
                }
                
                lb_log("Batch %zu/%zu complete", batch_num + 1, num_batches);
            }
            
            lb_log("");
            lb_log("All %zu batches completed successfully", num_batches);
        } else {
            lb_log("No artist_credit_ids to update in mapping table");
        }
        
        // Step 4: Rebuild artist indexes
        // Since artist data changes with most updates and nmslib doesn't support
        // incremental updates, we rebuild all three artist indexes (entity_id < 0)
        // This takes ~3 minutes and ensures artist search stays current
        lb_log("=== Step 4: Rebuilding artist indexes ===");
        {
            ArtistIndex artist_index(index_dir);
            artist_index.build();
        }
        
        // Step 5: Save the new timestamp
        lb_log("=== Step 5: Saving timestamp ===");
        if (!changed_data.save_current_timestamp()) {
            lb_error("Failed to save timestamp");
            PQfinish(pg_conn);
            return 1;
        }
        
        lb_log("=== Incremental update complete! ===");
        PQfinish(pg_conn);
        return 0;
        
    } catch (exception& e) {
        lb_error("Update failed: %s", e.what());
        PQfinish(pg_conn);
        return 1;
    }
}
