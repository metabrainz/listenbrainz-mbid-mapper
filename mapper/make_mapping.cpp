#include "make_mapping.hpp"
#include "artist_index.hpp"
#include "indexer_thread.hpp"
#include "custom_sorts.hpp"
#include "canonical_release.hpp"
#include "canonical_musicbrainz_data.hpp"
#include "changed_data.hpp"
#include <libpq-fe.h>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <filesystem>

MakeMapping::MakeMapping(const string& _index_dir) : index_dir(_index_dir) {
}

void MakeMapping::create() {
    auto t0 = std::chrono::high_resolution_clock::now();
    
    // Create index directory if it doesn't exist
    std::filesystem::create_directories(index_dir);
    
    string db_file = index_dir + "/mapping.db";
    string csv_file = index_dir + "/import.csv";
    
    // Check if database already exists
    if (std::filesystem::exists(db_file)) {
        lb_error("Error: Database file already exists: %s", db_file.c_str());
        lb_error("Please remove the existing database file before creating a new one. Perhaps you didn't mean this? :)");
        throw std::runtime_error("Database file already exists");
    }
    
    // Get DB connection string from environment variable
    const char* db_connect = std::getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
    if (!db_connect || strlen(db_connect) == 0) {
        throw std::runtime_error("CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
    }
    
    lb_log("Connecting to PostgreSQL...");
    
    // Connect to PostgreSQL using libpq
    PGconn *conn = PQconnectdb(db_connect);
    
    if (PQstatus(conn) != CONNECTION_OK) {
        lb_error("Connection to database failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        throw std::runtime_error("PostgreSQL connection failed");
    }

    // Create custom sort tables
    lb_log("Creating custom sort tables...");
    if (!create_custom_sort_tables(conn)) {
        PQfinish(conn);
        throw std::runtime_error("Failed to create custom sort tables");
    }
    
    // Create canonical release table
    lb_log("Creating canonical release table...");
    CanonicalRelease canonical_release(conn);
    if (!canonical_release.run()) {
        PQfinish(conn);
        throw std::runtime_error("Failed to create canonical release table");
    }

    // Create SQLite database and stream data directly into it
    lb_log("Creating SQLite database: %s", db_file.c_str());
    SQLite::Database db(db_file, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    
    // Create mapping table schema
    db.exec(R"(
        CREATE TABLE mapping (
            artist_credit_id INTEGER NOT NULL,
            artist_mbids TEXT NOT NULL,
            artist_credit_name TEXT NOT NULL,
            artist_credit_sortname TEXT,
            release_id INTEGER NOT NULL,
            release_mbid TEXT NOT NULL,
            release_artist_credit_id INTEGER NOT NULL,
            release_name TEXT,
            recording_id INTEGER NOT NULL,
            recording_mbid TEXT NOT NULL,
            recording_name TEXT,
            score INTEGER NOT NULL
        )
    )");
    
    // Create index_cache table
    db.exec(R"(
        CREATE TABLE index_cache (
            entity_id INTEGER NOT NULL UNIQUE,
            index_data BLOB NOT NULL
        )
    )");
    db.exec("CREATE INDEX entity_id_idx ON index_cache(entity_id)");
    
    // Stream data directly from PostgreSQL to SQLite
    lb_log("Streaming data from PostgreSQL to SQLite...");
    if (!create_canonical_musicbrainz_data_sqlite(conn, db)) {
        PQfinish(conn);
        throw std::runtime_error("Failed to stream data to SQLite");
    }
    
    // Initialize update_metadata table with current timestamp for incremental updates
    lb_log("Initializing update timestamp for incremental updates...");
    if (!initialize_update_timestamp(conn, db)) {
        PQfinish(conn);
        throw std::runtime_error("Failed to initialize update timestamp");
    }
    
    // Create PostgreSQL table with artist_credit_ids for artist index queries
    lb_log("Creating PostgreSQL table for artist index queries...");
    if (!create_artist_credit_id_table(conn, db)) {
        PQfinish(conn);
        throw std::runtime_error("Failed to create artist_credit_id table");
    }
    
    PQfinish(conn);
    
    // Create indexes on the mapping table
    // Re-apply performance settings for index creation (in case they were reset)
    db.exec("PRAGMA synchronous = OFF");
    db.exec("PRAGMA journal_mode = OFF");
    db.exec("PRAGMA cache_size = -2097152");        // 2GB cache
    db.exec("PRAGMA temp_store = MEMORY");
    db.exec("PRAGMA mmap_size = 8589934592");       // 8GB mmap
    db.exec("PRAGMA threads = 8");                 // Multi-threaded sorting
    
    lb_log("Creating indexes on mapping table...");
    auto idx_start = std::chrono::high_resolution_clock::now();
    
    lb_log("  Creating artist_credit_id_ndx...");
    db.exec("CREATE INDEX artist_credit_id_ndx ON mapping(artist_credit_id)");
    
    lb_log("  Creating release_artist_credit_id_ndx...");
    db.exec("CREATE INDEX release_artist_credit_id_ndx ON mapping(release_artist_credit_id)");
    
    lb_log("  Creating release_id_ndx...");
    db.exec("CREATE INDEX release_id_ndx ON mapping(release_id)");
    
    lb_log("  Creating recording_id_ndx...");
    db.exec("CREATE INDEX recording_id_ndx ON mapping(recording_id)");
    
    lb_log("  Creating release_id_recording_id_ndx...");
    db.exec("CREATE INDEX release_id_recording_id_ndx ON mapping(release_id, recording_id)");
    
    auto idx_end = std::chrono::high_resolution_clock::now();
    auto idx_duration = std::chrono::duration_cast<std::chrono::seconds>(idx_end - idx_start);
    lb_log("All indexes created in %ld seconds", idx_duration.count());
    
    auto t1 = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    lb_log("\nMapping database created in %.3f seconds.", duration.count() / 1000.0);
}

void print_usage() {
    lb_log("Usage: make_mapping");
    lb_log("");
    lb_log("Required environment variables:");
    lb_log("  INDEX_DIR                         Directory to create mapping.db in");
    lb_log("  CANONICAL_MUSICBRAINZ_DATA_CONNECT  PostgreSQL connection string");
}

int main(int argc, char *argv[])
{
    init_logging();
    load_env_file();  // Load .env file, env vars take precedence
    
    // Parse arguments (options only)
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
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
    
    // Validate CANONICAL_MUSICBRAINZ_DATA_CONNECT is set
    const char* db_connect = std::getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
    if (!db_connect || strlen(db_connect) == 0) {
        lb_error("Error: CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
        print_usage();
        return -1;
    }
    
    try {
        // Create mapping database with all data
        MakeMapping importer(index_dir);
        importer.create();
        lb_log("Mapping database created successfully!");
    } catch (const std::exception& e) {
        lb_error("Error: %s", e.what());
        return -1;
    }
    return 0;
}