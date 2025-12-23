#include <cstdlib>
#include <cstring>

#include <libpq-fe.h>

#include "utils.hpp"
#include "defs.hpp"
#include "SQLiteCpp.h"
#include "changed_data.hpp"
#include "canonical_release_updater.hpp"

using namespace std;

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
        
        // Step 3: Update mapping table in SQLite
        lb_log("=== Step 3: Updating mapping table ===");
        const auto& changed_artist_credits = changed_data.get_changed_artist_credit_ids();
        
        if (!changed_artist_credits.empty()) {
            // TODO: Implement MappingUpdater (Step 3 of plan)
            lb_log("TODO: Update %zu artist_credit_ids in mapping table",
                   changed_artist_credits.size());
        } else {
            lb_log("No artist_credit_ids to update in mapping table");
        }
        
        // Step 4: Rebuild fuzzy indexes
        lb_log("=== Step 4: Rebuilding fuzzy indexes ===");
        if (!changed_artist_credits.empty()) {
            // TODO: Implement IndexUpdater (Step 4 of plan)
            lb_log("TODO: Rebuild indexes for %zu artist_credit_ids",
                   changed_artist_credits.size());
        } else {
            lb_log("No indexes to rebuild");
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
