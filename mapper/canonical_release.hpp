#pragma once

#include <string>
#include <vector>
#include <set>
#include <libpq-fe.h>
#include "bulk_table.hpp"
#include "utils.hpp"

using namespace std;

// Test artist IDs for minimal dataset mode: Gun'n'roses, beyoncé, portishead, Erik Satie
const vector<int> TEST_ARTIST_IDS = {1160983, 49627, 65, 21238};

/**
 * This class creates the canonical release table.
 * 
 * The canonical release table contains ALL releases ordered by format, date, country, etc.
 * The SERIAL id column provides a score where lower = better.
 * 
 * Deduplication is by release_id (to avoid duplicates from multiple mediums per release),
 * NOT by release_group. This ensures all releases from a release_group are included,
 * allowing bonus tracks on non-standard editions to be found.
 */
class CanonicalRelease : public BulkInsertTable {
private:
    set<int> release_index;  // Track seen release IDs to deduplicate
    bool use_minimal_dataset;

public:
    CanonicalRelease(PGconn* conn, int batch_size = DEFAULT_BATCH_SIZE, bool unlogged = false, bool minimal_dataset = false)
        : BulkInsertTable("mapping.canonical_release", conn, batch_size, unlogged)
        , use_minimal_dataset(minimal_dataset)
    {}

    vector<ColumnDef> get_create_table_columns() override {
        return {
            {"id",           "SERIAL"},
            {"release",      "INTEGER NOT NULL"},
            {"release_mbid", "UUID NOT NULL"}
        };
    }

    vector<IndexDef> get_index_names() override {
        return {
            {"canonical_release_idx_release", "release", false},
            {"canonical_release_idx_id",      "id", false}
        };
    }

    /**
     * Get the SQL query for fetching canonical releases.
     * 
     * @param exclude_various_artists If true, use != 1, otherwise use = 1
     * @return The SQL query string
     */
    string get_insert_query(bool exclude_various_artists) {
        string op = exclude_various_artists ? "!=" : "=";
        string artist_filter = "";
        
        if (use_minimal_dataset) {
            lb_log("%s: Using a minimal dataset for artist credit pairs: artist_id %s 1", 
                   table_name.c_str(), op.c_str());
            
            string ids;
            for (size_t i = 0; i < TEST_ARTIST_IDS.size(); i++) {
                if (i > 0) ids += ",";
                ids += to_string(TEST_ARTIST_IDS[i]);
            }
            artist_filter = "AND rg.artist_credit IN (" + ids + ")";
        } else {
            lb_log("%s: Using a full dataset for artist credit pairs: artist_id %s 1", 
                   table_name.c_str(), op.c_str());
        }

        return R"(
            SELECT r.id AS release
                 , r.gid AS release_mbid
                 , rg.id AS release_group
              FROM musicbrainz.release_group rg
              JOIN musicbrainz.release r
                ON rg.id = r.release_group
         LEFT JOIN musicbrainz.release_country rc
                ON rc.release = r.id
              JOIN musicbrainz.medium m
                ON m.release = r.id
         LEFT JOIN musicbrainz.medium_format mf
                ON m.format = mf.id
         LEFT JOIN mapping.format_sort fs
                ON mf.id = fs.format
              JOIN musicbrainz.artist_credit ac
                ON rg.artist_credit = ac.id
         LEFT JOIN musicbrainz.release_group_primary_type rgpt
                ON rg.type = rgpt.id
         LEFT JOIN musicbrainz.release_group_secondary_type_join rgstj
                ON rg.id = rgstj.release_group
         LEFT JOIN musicbrainz.release_group_secondary_type rgst
                ON rgstj.secondary_type = rgst.id
         LEFT JOIN mapping.release_group_combined_type_sort rgcts
                ON (rgpt.id = rgcts.primary_type OR (rgpt.id IS NULL AND rgcts.primary_type IS NULL))
               AND (rgst.id = rgcts.secondary_type OR (rgst.id IS NULL AND rgcts.secondary_type IS NULL))
             WHERE rg.artist_credit )" + op + R"( 1
                   )" + artist_filter + R"(
          ORDER BY rgcts.sort NULLS LAST
                 , fs.sort NULLS LAST
                 , to_date(date_year::TEXT || '-' ||
                           COALESCE(date_month,12)::TEXT || '-' ||
                           COALESCE(date_day,28)::TEXT, 'YYYY-MM-DD')
                 , country, rg.artist_credit, rg.name, r.id
        )";
    }

    /**
     * Process a single row from the query result.
     * Deduplicates by release_id (not release_group) to avoid duplicates from multiple mediums.
     * All releases from a release_group are included, just not the same release twice.
     * Returns true if the row was added, false if it was a duplicate.
     */
    bool process_row(int release_id, const string& release_mbid, int release_group_id) {
        (void)release_group_id;  // Not used for deduplication
        
        // Deduplicate by release ID - avoid same release appearing twice (from multiple mediums)
        if (release_index.count(release_id) > 0) {
            return false;
        }
        
        release_index.insert(release_id);
        
        // Add row: release, release_mbid
        return add_row({
            int_value(release_id),
            escape_value(release_mbid)
        });
    }

    /**
     * Run the complete canonical release creation process.
     * Executes two queries (excluding and including Various Artists) and processes all rows.
     */
    bool run(bool no_swap = false, bool no_analyze = false) {
        lb_log("%s: start", table_name.c_str());
        
        // Create the temp table
        if (!create_tables()) {
            return false;
        }

        release_index.clear();
        int total_rows_processed = 0;
        int total_rows_inserted = 0;

        // Run two queries: first excluding Various Artists (id=1), then including only Various Artists
        vector<bool> query_modes = {true, false};  // true = exclude VA, false = include only VA
        
        for (size_t query_idx = 0; query_idx < query_modes.size(); query_idx++) {
            string query = get_insert_query(query_modes[query_idx]);
            
            lb_log("%s: execute query %zu of %zu", table_name.c_str(), query_idx + 1, query_modes.size());
            
            // Begin transaction for cursor
            PGresult* result = PQexec(conn, "BEGIN");
            if (PQresultStatus(result) != PGRES_COMMAND_OK) {
                lb_error("BEGIN failed: %s", PQerrorMessage(conn));
                PQclear(result);
                return false;
            }
            PQclear(result);
            
            // Use a cursor for large result sets
            string cursor_name = "canonical_release_cursor";
            string cursor_sql = "DECLARE " + cursor_name + " CURSOR FOR " + query;
            
            result = PQexec(conn, cursor_sql.c_str());
            if (PQresultStatus(result) != PGRES_COMMAND_OK) {
                lb_error("DECLARE CURSOR failed: %s", PQerrorMessage(conn));
                PQclear(result);
                PQexec(conn, "ROLLBACK");
                return false;
            }
            PQclear(result);
            
            // Fetch in batches
            int rows_this_query = 0;
            while (true) {
                string fetch_sql = "FETCH " + to_string(batch_size) + " FROM " + cursor_name;
                result = PQexec(conn, fetch_sql.c_str());
                
                if (PQresultStatus(result) != PGRES_TUPLES_OK) {
                    lb_error("FETCH failed: %s", PQerrorMessage(conn));
                    PQclear(result);
                    return false;
                }
                
                int num_rows = PQntuples(result);
                if (num_rows == 0) {
                    PQclear(result);
                    break;
                }
                
                for (int i = 0; i < num_rows; i++) {
                    char* release_val = PQgetvalue(result, i, 0);
                    char* mbid_val = PQgetvalue(result, i, 1);
                    char* rg_val = PQgetvalue(result, i, 2);
                    
                    int release_id = release_val ? atoi(release_val) : 0;
                    string release_mbid = mbid_val ? mbid_val : "";
                    int release_group_id = rg_val ? atoi(rg_val) : 0;
                    
                    if (process_row(release_id, release_mbid, release_group_id)) {
                        total_rows_inserted++;
                    }
                    total_rows_processed++;
                    rows_this_query++;
                }
                
                PQclear(result);
                
                if (rows_this_query % 100000 == 0) {
                    lb_log("%s: processed %d rows...", table_name.c_str(), rows_this_query);
                }
            }
            
            // Close cursor and commit transaction
            result = PQexec(conn, ("CLOSE " + cursor_name).c_str());
            PQclear(result);
            
            result = PQexec(conn, "COMMIT");
            if (PQresultStatus(result) != PGRES_COMMAND_OK) {
                lb_error("COMMIT failed: %s", PQerrorMessage(conn));
                PQclear(result);
                return false;
            }
            PQclear(result);
            
            lb_log("%s: query %zu complete, processed %d rows", 
                   table_name.c_str(), query_idx + 1, rows_this_query);
        }

        lb_log("%s: processed %d total rows, inserted %d unique releases", 
               table_name.c_str(), total_rows_processed, total_rows_inserted);

        // Finalize: flush, post-process, create indexes, swap
        return finalize(no_swap, no_analyze);
    }
};

/**
 * Convenience function to create the canonical release table.
 * Handles transaction management automatically.
 */
inline bool create_canonical_release_table(PGconn* conn, bool use_minimal_dataset = false, 
                                           bool no_swap = false, bool no_analyze = false) {
    // Begin transaction
    PGresult* result = PQexec(conn, "BEGIN");
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        lb_error("BEGIN failed: %s", PQerrorMessage(conn));
        PQclear(result);
        return false;
    }
    PQclear(result);
    
    // Suppress NOTICE messages
    result = PQexec(conn, "SET LOCAL client_min_messages TO WARNING");
    PQclear(result);
    
    CanonicalRelease table(conn, DEFAULT_BATCH_SIZE, false, use_minimal_dataset);
    
    if (!table.run(no_swap, no_analyze)) {
        PQexec(conn, "ROLLBACK");
        return false;
    }
    
    // Commit transaction
    result = PQexec(conn, "COMMIT");
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        lb_error("COMMIT failed: %s", PQerrorMessage(conn));
        PQclear(result);
        PQexec(conn, "ROLLBACK");
        return false;
    }
    PQclear(result);
    
    return true;
}

/**
 * Convenience function that connects to the database and creates the canonical release table.
 * Uses the CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable for connection.
 */
inline bool create_canonical_release_table_from_env(bool use_minimal_dataset = false,
                                                    bool no_swap = false, bool no_analyze = false) {
    const char* db_connect = std::getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
    if (!db_connect || strlen(db_connect) == 0) {
        lb_error("CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
        return false;
    }
    
    lb_log("Connecting to PostgreSQL for canonical release table...");
    PGconn* conn = PQconnectdb(db_connect);
    
    if (PQstatus(conn) != CONNECTION_OK) {
        lb_error("Connection to database failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }
    
    bool success = create_canonical_release_table(conn, use_minimal_dataset, no_swap, no_analyze);
    
    PQfinish(conn);
    return success;
}
