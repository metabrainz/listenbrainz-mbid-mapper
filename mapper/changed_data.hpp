#pragma once

#include <string>
#include <set>
#include <libpq-fe.h>
#include "SQLiteCpp.h"
#include "utils.hpp"
#include "defs.hpp"

using namespace std;

/**
 * MusicBrainzChangedData - Tracks and collects changed entities from MusicBrainz.
 * 
 * This class manages the last_updated timestamp in SQLite and queries PostgreSQL
 * for entities that have changed since that timestamp. It collects:
 * - release_group_ids: For canonical_release table updates
 * - artist_credit_ids: For mapping and fuzzy index updates  
 * - recording_ids: For fine-grained tracking (optional use)
 * 
 * Usage:
 *   MusicBrainzChangedData changed_data(pg_conn, sqlite_db);
 *   if (changed_data.collect()) {
 *       // Use get_changed_*() methods
 *       changed_data.save_current_timestamp();
 *   }
 */
class MusicBrainzChangedData {
private:
    PGconn* pg_conn;
    SQLite::Database& sqlite_db;
    
    // Collected changed entity IDs
    set<int> changed_release_groups;
    set<int> changed_artist_credit_ids;
    set<int> changed_recording_ids;
    
    // Timestamps
    string last_updated;
    string current_timestamp;
    
    // SQL for metadata table
    static constexpr const char* CREATE_METADATA_TABLE = R"(
        CREATE TABLE IF NOT EXISTS update_metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
    )";
    
    static constexpr const char* GET_LAST_UPDATED = R"(
        SELECT value FROM update_metadata WHERE key = 'last_updated'
    )";
    
    static constexpr const char* SET_LAST_UPDATED = R"(
        INSERT OR REPLACE INTO update_metadata (key, value) VALUES ('last_updated', ?)
    )";
    
    // =========================================================================
    // Queries for changed release_groups (for canonical_release updates)
    // =========================================================================
    
    // Releases that were updated (date, country, etc. might have changed)
    static constexpr const char* CHANGED_RELEASE_GROUPS_FROM_RELEASES = R"(
        SELECT DISTINCT r.release_group
          FROM musicbrainz.release r
         WHERE r.last_updated > $1::timestamp
    )";
    
    // Release_groups that were directly updated (type might have changed)
    static constexpr const char* CHANGED_RELEASE_GROUPS_DIRECT = R"(
        SELECT DISTINCT rg.id
          FROM musicbrainz.release_group rg
         WHERE rg.last_updated > $1::timestamp
    )";
    
    // Mediums that were updated (format might have changed)
    static constexpr const char* CHANGED_RELEASE_GROUPS_FROM_MEDIUMS = R"(
        SELECT DISTINCT r.release_group
          FROM musicbrainz.medium m
          JOIN musicbrainz.release r ON r.id = m.release
         WHERE m.last_updated > $1::timestamp
    )";
    
    // Release countries that were updated
    // Note: release_country table doesn't have last_updated column
    // We detect country changes by checking if releases were updated (which includes country changes)
    // This is already covered by CHANGED_RELEASE_GROUPS_FROM_RELEASES, so this query is a no-op
    // Keeping it for documentation purposes - country changes are tracked via release.last_updated
    // We still reference $1 to satisfy the parameter binding
    static constexpr const char* CHANGED_RELEASE_GROUPS_FROM_COUNTRIES = R"(
        SELECT DISTINCT r.release_group
          FROM musicbrainz.release r
         WHERE FALSE AND r.last_updated > $1::timestamp
    )";
    
    // =========================================================================
    // Queries for changed artist_credit_ids (for mapping/index updates)
    // =========================================================================
    
    // Recordings that were directly updated
    static constexpr const char* CHANGED_ARTIST_CREDITS_FROM_RECORDINGS = R"(
        SELECT DISTINCT r.artist_credit
          FROM musicbrainz.recording r
         WHERE r.last_updated > $1::timestamp
    )";
    
    // Releases that were updated - affects all recordings on those releases
    static constexpr const char* CHANGED_ARTIST_CREDITS_FROM_RELEASES = R"(
        SELECT DISTINCT rec.artist_credit
          FROM musicbrainz.release rl
          JOIN musicbrainz.medium m ON m.release = rl.id
          JOIN musicbrainz.track t ON t.medium = m.id
          JOIN musicbrainz.recording rec ON rec.id = t.recording
         WHERE rl.last_updated > $1::timestamp
    )";
    
    // Artists that were updated - affects all artist_credits using that artist
    static constexpr const char* CHANGED_ARTIST_CREDITS_FROM_ARTISTS = R"(
        SELECT DISTINCT acn.artist_credit
          FROM musicbrainz.artist a
          JOIN musicbrainz.artist_credit_name acn ON acn.artist = a.id
         WHERE a.last_updated > $1::timestamp
    )";
    
    // Artist credits that were created (new artist credits since last update)
    // Note: artist_credit table has `created` column but no `last_updated`
    // This catches newly created artist credits
    static constexpr const char* CHANGED_ARTIST_CREDITS_DIRECT = R"(
        SELECT DISTINCT ac.id
          FROM musicbrainz.artist_credit ac
         WHERE ac.created > $1::timestamp
    )";
    
    // =========================================================================
    // Queries for changed recording_ids (for fine-grained tracking)
    // =========================================================================
    
    // Recordings that were directly updated
    static constexpr const char* CHANGED_RECORDINGS_DIRECT = R"(
        SELECT DISTINCT r.id
          FROM musicbrainz.recording r
         WHERE r.last_updated > $1::timestamp
    )";
    
    /**
     * Execute a PostgreSQL query with last_updated parameter and collect integer results.
     */
    bool execute_and_collect(const char* query, const char* description, set<int>& result_set) {
        const char* paramValues[1] = { last_updated.c_str() };
        
        PGresult* res = PQexecParams(pg_conn, query, 1, nullptr,
                                      paramValues, nullptr, nullptr, 0);
        
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            lb_error("Query failed for %s: %s", description, PQerrorMessage(pg_conn));
            PQclear(res);
            return false;
        }
        
        int count = PQntuples(res);
        for (int i = 0; i < count; i++) {
            const char* val = PQgetvalue(res, i, 0);
            if (val && *val) {
                result_set.insert(atoi(val));
            }
        }
        
        lb_log("  %s: %d rows, total in set: %zu", description, count, result_set.size());
        PQclear(res);
        return true;
    }
    
public:
    MusicBrainzChangedData(PGconn* _pg_conn, SQLite::Database& _sqlite_db)
        : pg_conn(_pg_conn), sqlite_db(_sqlite_db) {}
    
    /**
     * Get the last_updated timestamp from SQLite.
     * Creates the metadata table if it doesn't exist.
     * Returns empty string if no timestamp has been saved yet.
     */
    string get_last_updated() {
        try {
            sqlite_db.exec(CREATE_METADATA_TABLE);
            SQLite::Statement query(sqlite_db, GET_LAST_UPDATED);
            if (query.executeStep()) {
                return query.getColumn(0).getString();
            }
        } catch (exception& e) {
            lb_error("Error getting last_updated: %s", e.what());
        }
        return "";
    }
    
    /**
     * Get the current replication timestamp from PostgreSQL.
     * This uses the replication_control table to get the actual data timestamp,
     * not the wall clock time. This ensures we track what data we've processed.
     */
    bool fetch_current_timestamp() {
        PGresult* res = PQexec(pg_conn, 
            "SELECT last_replication_date::TEXT FROM musicbrainz.replication_control");
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            lb_error("Failed to get replication timestamp: %s", PQerrorMessage(pg_conn));
            PQclear(res);
            return false;
        }
        if (PQntuples(res) == 0) {
            lb_error("No rows in replication_control table");
            PQclear(res);
            return false;
        }
        current_timestamp = PQgetvalue(res, 0, 0);
        PQclear(res);
        return true;
    }
    
    /**
     * Save the current timestamp to SQLite, marking this update as complete.
     */
    bool save_current_timestamp() {
        if (current_timestamp.empty()) {
            lb_error("No current timestamp to save - call fetch_current_timestamp() first");
            return false;
        }
        
        try {
            SQLite::Statement query(sqlite_db, SET_LAST_UPDATED);
            query.bind(1, current_timestamp);
            query.exec();
            lb_log("Saved last_updated: %s", current_timestamp.c_str());
            return true;
        } catch (exception& e) {
            lb_error("Error saving last_updated: %s", e.what());
            return false;
        }
    }
    
    /**
     * Check if this is the first run (no previous timestamp).
     */
    bool is_first_run() {
        return get_last_updated().empty();
    }
    
    /**
     * Collect all changed entities from PostgreSQL.
     * 
     * @return true if collection succeeded, false on error
     */
    bool collect() {
        // Get last_updated timestamp
        last_updated = get_last_updated();
        if (last_updated.empty()) {
            lb_error("No last_updated timestamp found. Run a full build first.");
            lb_error("Use make_mapping and make_indexes to create the initial database.");
            return false;
        }
        
        lb_log("Collecting changes since: %s", last_updated.c_str());
        
        // Get current timestamp before we start querying
        if (!fetch_current_timestamp()) {
            return false;
        }
        lb_log("Current timestamp: %s", current_timestamp.c_str());
        
        // Clear any previous results
        changed_release_groups.clear();
        changed_artist_credit_ids.clear();
        changed_recording_ids.clear();
        
        // Collect changed release_groups
        lb_log("Collecting changed release_groups...");
        if (!execute_and_collect(CHANGED_RELEASE_GROUPS_FROM_RELEASES, 
                                  "releases", changed_release_groups)) return false;
        if (!execute_and_collect(CHANGED_RELEASE_GROUPS_DIRECT,
                                  "release_groups", changed_release_groups)) return false;
        if (!execute_and_collect(CHANGED_RELEASE_GROUPS_FROM_MEDIUMS,
                                  "mediums", changed_release_groups)) return false;
        if (!execute_and_collect(CHANGED_RELEASE_GROUPS_FROM_COUNTRIES,
                                  "release_countries", changed_release_groups)) return false;
        
        // Collect changed artist_credit_ids
        lb_log("Collecting changed artist_credit_ids...");
        if (!execute_and_collect(CHANGED_ARTIST_CREDITS_FROM_RECORDINGS,
                                  "recordings", changed_artist_credit_ids)) return false;
        if (!execute_and_collect(CHANGED_ARTIST_CREDITS_FROM_RELEASES,
                                  "releases", changed_artist_credit_ids)) return false;
        if (!execute_and_collect(CHANGED_ARTIST_CREDITS_FROM_ARTISTS,
                                  "artists", changed_artist_credit_ids)) return false;
        if (!execute_and_collect(CHANGED_ARTIST_CREDITS_DIRECT,
                                  "artist_credits", changed_artist_credit_ids)) return false;
        
        // Collect changed recording_ids
        lb_log("Collecting changed recording_ids...");
        if (!execute_and_collect(CHANGED_RECORDINGS_DIRECT,
                                  "recordings", changed_recording_ids)) return false;
        
        // Filter out Various Artists and other special artist_credits
        // (artist_credit_id 1 = Various Artists has millions of releases)
        size_t before_filter = changed_artist_credit_ids.size();
        for (int id = 1; id <= VARIOUS_ARTISTS_ARTIST_CREDIT_ID; id++) {
            changed_artist_credit_ids.erase(id);
        }
        if (before_filter != changed_artist_credit_ids.size()) {
            lb_log("Filtered out %zu special artist_credit_ids (Various Artists etc.)",
                   before_filter - changed_artist_credit_ids.size());
        }
        
        return true;
    }
    
    /**
     * Check if any changes were detected.
     */
    bool has_changes() const {
        return !changed_release_groups.empty() || 
               !changed_artist_credit_ids.empty() ||
               !changed_recording_ids.empty();
    }
    
    /**
     * Get the set of changed release_group IDs.
     * These need canonical_release table re-evaluation.
     */
    const set<int>& get_changed_release_groups() const {
        return changed_release_groups;
    }
    
    /**
     * Get the set of changed artist_credit IDs.
     * These need mapping rows updated and fuzzy indexes rebuilt.
     */
    const set<int>& get_changed_artist_credit_ids() const {
        return changed_artist_credit_ids;
    }
    
    /**
     * Get the set of changed recording IDs.
     * For fine-grained tracking if needed.
     */
    const set<int>& get_changed_recording_ids() const {
        return changed_recording_ids;
    }
    
    /**
     * Get the timestamp that will be saved after successful update.
     */
    const string& get_current_timestamp() const {
        return current_timestamp;
    }
    
    /**
     * Get the timestamp we're comparing against.
     */
    const string& get_last_updated_timestamp() const {
        return last_updated;
    }
    
    /**
     * Print a summary of collected changes.
     */
    void print_summary() const {
        lb_log("Change summary:");
        lb_log("  Time range: %s to %s", last_updated.c_str(), current_timestamp.c_str());
        lb_log("  Release groups to update: %zu", changed_release_groups.size());
        lb_log("  Artist credits to update: %zu", changed_artist_credit_ids.size());
        lb_log("  Recordings changed: %zu", changed_recording_ids.size());
    }
};

/**
 * Initialize the update_metadata table with the current timestamp.
 * Call this after a successful full build to enable incremental updates.
 */
inline bool initialize_update_timestamp(PGconn* pg_conn, SQLite::Database& sqlite_db) {
    // Create metadata table
    sqlite_db.exec(R"(
        CREATE TABLE IF NOT EXISTS update_metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
    )");
    
    // Get replication timestamp from PostgreSQL (not wall clock time)
    PGresult* res = PQexec(pg_conn, 
        "SELECT last_replication_date::TEXT FROM musicbrainz.replication_control");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        lb_error("Failed to get replication timestamp: %s", PQerrorMessage(pg_conn));
        PQclear(res);
        return false;
    }
    if (PQntuples(res) == 0) {
        lb_error("No rows in replication_control table");
        PQclear(res);
        return false;
    }
    string timestamp = PQgetvalue(res, 0, 0);
    PQclear(res);
    
    // Save it
    SQLite::Statement query(sqlite_db, 
        "INSERT OR REPLACE INTO update_metadata (key, value) VALUES ('last_updated', ?)");
    query.bind(1, timestamp);
    query.exec();
    
    lb_log("Initialized last_updated timestamp: %s", timestamp.c_str());
    return true;
}
