#pragma once

#include <libpq-fe.h>
#include <string>
#include <unordered_set>
#include <fstream>
#include <cctype>
#include <functional>
#include "utils.hpp"
#include "unidecode/unidecode.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

using namespace std;

/**
 * Compute combined_lookup for deduplication.
 * This matches the Python: unidecode(re.sub(r'[^\w]+', '', artist + recording + release).lower())
 * We use unidecode to transliterate, then keep only alphanumeric chars and lowercase.
 */
inline string compute_combined_lookup(const string& artist, const string& recording, const string& release) {
    string combined = artist + recording + release;
    string decoded = unidecode::UnidecodeString(combined);
    
    string result;
    result.reserve(decoded.size());
    
    for (unsigned char c : decoded) {
        // \w in Python regex matches [a-zA-Z0-9_], but the pattern removes non-\w
        // so we keep alphanumeric and underscore, then lowercase
        if (isalnum(c) || c == '_') {
            result += tolower(c);
        }
    }
    
    return result;
}

// Structure to hold a mapping row
struct MappingRowData {
    int64_t artist_credit_id;
    string artist_mbids;
    string artist_credit_name;
    string artist_credit_sortname;
    int64_t release_id;
    string release_mbid;
    int64_t release_artist_credit_id;
    string release_name;
    int64_t recording_id;
    string recording_mbid;
    string recording_name;
    int64_t score;
};

// Row handler callback type
using RowHandler = function<void(const MappingRowData&)>;

/**
 * Create the canonical musicbrainz data directly from the database.
 * This skips creating the intermediate PostgreSQL table and does deduplication in memory.
 * 
 * Requires: mapping.canonical_release table to exist first.
 * 
 * Output columns:
 *   artist_credit_id, artist_mbids, artist_credit_name, artist_credit_sortname,
 *   release_id, release_mbid, release_artist_credit_id, release_name,
 *   recording_id, recording_mbid, recording_name, score
 */

// Helper function to process rows from a query result
// Deduplication is by combined_lookup (artist+recording+release name normalized), keeping lowest score
inline size_t process_rows(PGresult* res, const RowHandler& handler, unordered_set<string>& seen_lookups,
                           size_t& total_rows, size_t& written_rows) {
    int nrows = PQntuples(res);
    
    for (int i = 0; i < nrows; i++) {
        total_rows++;
        
        // Column indices from query:
        //  0: artist_credit_id
        //  1: artist_mbids (array as text)
        //  2: artist_credit_name
        //  3: artist_sortnames (array as text)
        //  4: release_id
        //  5: release_mbid
        //  6: release_artist_credit_id
        //  7: release_name
        //  8: recording_id
        //  9: recording_mbid
        // 10: recording_name
        // 11: score
        
        string artist_credit_name = PQgetvalue(res, i, 2);
        string release_name = PQgetvalue(res, i, 7);
        string recording_name = PQgetvalue(res, i, 10);
        
        // Deduplicate by combined_lookup (artist+recording+release name normalized)
        // This matches Python's post-process DELETE by combined_lookup, keeping lowest score
        // Since we ORDER BY score, the first occurrence has the best score
        string combined_lookup = compute_combined_lookup(artist_credit_name, recording_name, release_name);
        if (!seen_lookups.insert(combined_lookup).second) {
            continue;  // Already seen this combined_lookup, skip (keep the one with lower score)
        }
        
        MappingRowData row;
        
        // Parse integers
        char* val = PQgetvalue(res, i, 0);
        row.artist_credit_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, i, 4);
        row.release_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, i, 6);
        row.release_artist_credit_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, i, 8);
        row.recording_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, i, 11);
        row.score = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        
        // String fields
        row.artist_credit_name = artist_credit_name;
        row.release_name = release_name;
        row.recording_name = recording_name;
        row.recording_mbid = PQgetvalue(res, i, 9);
        row.release_mbid = PQgetvalue(res, i, 5);
        
        // Parse PostgreSQL arrays - remove { } braces
        string artist_mbids = PQgetvalue(res, i, 1);
        if (artist_mbids.length() >= 2 && artist_mbids[0] == '{' && artist_mbids.back() == '}') {
            artist_mbids = artist_mbids.substr(1, artist_mbids.length() - 2);
        }
        row.artist_mbids = artist_mbids;
        
        // Get first sortname from array for artist_credit_sortname
        string sortnames_raw = PQgetvalue(res, i, 3);
        string artist_credit_sortname;
        if (sortnames_raw.length() >= 2 && sortnames_raw[0] == '{' && sortnames_raw.back() == '}') {
            sortnames_raw = sortnames_raw.substr(1, sortnames_raw.length() - 2);
            if (!sortnames_raw.empty()) {
                if (sortnames_raw[0] == '"') {
                    size_t end = sortnames_raw.find("\",", 1);
                    if (end == string::npos) {
                        end = sortnames_raw.length() - 1;
                    }
                    artist_credit_sortname = sortnames_raw.substr(1, end - 1);
                    size_t pos = 0;
                    while ((pos = artist_credit_sortname.find("\"\"", pos)) != string::npos) {
                        artist_credit_sortname.replace(pos, 2, "\"");
                        pos++;
                    }
                } else {
                    size_t comma = sortnames_raw.find(',');
                    artist_credit_sortname = (comma != string::npos) 
                        ? sortnames_raw.substr(0, comma) 
                        : sortnames_raw;
                }
            }
        }
        row.artist_credit_sortname = artist_credit_sortname;
        
        // Call the handler
        handler(row);
        written_rows++;
        
        if (total_rows % 1000000 == 0) {
            lb_log("Processed %s rows, written %s unique rows", 
                   format_number(total_rows).c_str(), format_number(written_rows).c_str());
        }
    }
    
    return nrows;
}

// Helper to run a cursor query and process all rows
inline bool run_cursor_query(PGconn* conn, const string& query, const string& cursor_name,
                             const RowHandler& handler, unordered_set<string>& seen_lookups,
                             size_t& total_rows, size_t& written_rows) {
    const int FETCH_SIZE = 10000;
    
    string cursor_sql = "DECLARE " + cursor_name + " CURSOR FOR " + query;
    PGresult* res = PQexec(conn, cursor_sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        lb_error("DECLARE CURSOR failed: %s", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);
    
    while (true) {
        res = PQexec(conn, ("FETCH " + to_string(FETCH_SIZE) + " FROM " + cursor_name).c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            lb_error("FETCH failed: %s", PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        
        int nrows = PQntuples(res);
        if (nrows == 0) {
            PQclear(res);
            break;
        }
        
        process_rows(res, handler, seen_lookups, total_rows, written_rows);
        PQclear(res);
    }
    
    res = PQexec(conn, ("CLOSE " + cursor_name).c_str());
    PQclear(res);
    
    return true;
}

// Build the queries used for canonical musicbrainz data
inline pair<string, string> build_canonical_queries(bool use_minimal_dataset = false) {
    string artist_filter = "";
    if (use_minimal_dataset) {
        lb_log("Using minimal dataset for testing");
        artist_filter = " AND ac.id IN (1160983, 49627, 65, 21238)";
    }
    
    // Query 1: Recordings with releases - one row per (recording, release) pair
    // DISTINCT removes duplicates from same recording on multiple tracks of same release
    // ORDER BY score (cr.id) first so we see the best score for each combined_lookup first
    string query1 = R"(
        SELECT DISTINCT
               ac.id AS artist_credit_id
             , s.artist_mbids
             , ac.name AS artist_credit_name
             , s.artist_sortnames
             , rl.id AS release_id
             , rl.gid::TEXT AS release_mbid
             , rl.artist_credit AS release_artist_credit_id
             , rl.name AS release_name
             , r.id AS recording_id
             , r.gid::TEXT AS recording_mbid
             , r.name AS recording_name
             , cr.id AS score
          FROM musicbrainz.recording r
          JOIN musicbrainz.artist_credit ac
            ON r.artist_credit = ac.id
          JOIN musicbrainz.track t
            ON t.recording = r.id
          JOIN musicbrainz.medium m
            ON m.id = t.medium
          JOIN musicbrainz.release rl
            ON rl.id = m.release
          JOIN mapping.canonical_release cr
            ON rl.id = cr.release
          JOIN (SELECT artist_credit
                     , array_agg(a.gid ORDER BY position) AS artist_mbids
                     , array_agg(a.sort_name ORDER BY position) AS artist_sortnames
                  FROM musicbrainz.artist_credit_name acn2
                  JOIN musicbrainz.artist a
                    ON acn2.artist = a.id
              GROUP BY acn2.artist_credit) s
            ON ac.id = s.artist_credit
         WHERE 1=1 )" + artist_filter + R"(
      ORDER BY cr.id, ac.id
    )";
    
    // Query 2: Standalone recordings (no tracks/releases) - slower, run separately
    string query2 = R"(
        SELECT DISTINCT
               ac.id AS artist_credit_id
             , s.artist_mbids
             , ac.name AS artist_credit_name
             , s.artist_sortnames
             , 4294967295 AS release_id
             , ''::TEXT AS release_mbid
             , 4294967295 AS release_artist_credit_id
             , '' AS release_name
             , r.id AS recording_id
             , r.gid::TEXT AS recording_mbid
             , r.name AS recording_name
             , 4294967295 AS score
          FROM musicbrainz.recording r
          JOIN musicbrainz.artist_credit ac
            ON r.artist_credit = ac.id
          JOIN (SELECT artist_credit
                     , array_agg(a.gid ORDER BY position) AS artist_mbids
                     , array_agg(a.sort_name ORDER BY position) AS artist_sortnames
                  FROM musicbrainz.artist_credit_name acn2
                  JOIN musicbrainz.artist a
                    ON acn2.artist = a.id
              GROUP BY acn2.artist_credit) s
            ON ac.id = s.artist_credit
         WHERE NOT EXISTS (SELECT 1 FROM musicbrainz.track t WHERE t.recording = r.id)
           )" + artist_filter + R"(
      ORDER BY ac.id
    )";
    
    return {query1, query2};
}

/**
 * Stream canonical musicbrainz data directly to SQLite database.
 * This skips writing CSV to disk and inserts directly using prepared statements.
 */
inline bool create_canonical_musicbrainz_data_sqlite(PGconn* conn, SQLite::Database& db, bool use_minimal_dataset = false) {
    
    auto [query1, query2] = build_canonical_queries(use_minimal_dataset);
    
    lb_log("Starting canonical musicbrainz data export to SQLite...");
    
    // Begin PostgreSQL transaction
    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        lb_error("BEGIN failed: %s", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);
    
    // Configure SQLite for bulk insert and index creation performance
    db.exec("PRAGMA synchronous = OFF");
    db.exec("PRAGMA journal_mode = OFF");           // No journaling for bulk operations
    db.exec("PRAGMA cache_size = -2097152");        // 2GB cache (negative = KB)
    db.exec("PRAGMA temp_store = MEMORY");          // Keep temp data in RAM
    db.exec("PRAGMA mmap_size = 8589934592");       // 8GB memory-mapped I/O
    db.exec("PRAGMA threads = 16");                 // Enable multi-threaded sorting
    
    // Prepare SQLite insert statement
    SQLite::Statement stmt(db, 
        "INSERT INTO mapping VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    
    // Begin SQLite transaction
    db.exec("BEGIN TRANSACTION");
    
    unordered_set<string> seen_lookups;
    size_t total_rows = 0;
    size_t written_rows = 0;
    size_t batch_count = 0;
    const size_t COMMIT_INTERVAL = 500000;  // Larger batches = fewer commits
    
    // Row handler that inserts directly into SQLite
    RowHandler sqlite_handler = [&](const MappingRowData& row) {
        stmt.bind(1, row.artist_credit_id);
        stmt.bind(2, row.artist_mbids);
        stmt.bind(3, row.artist_credit_name);
        stmt.bind(4, row.artist_credit_sortname);
        stmt.bind(5, row.release_id);
        stmt.bind(6, row.release_mbid);
        stmt.bind(7, row.release_artist_credit_id);
        stmt.bind(8, row.release_name);
        stmt.bind(9, row.recording_id);
        stmt.bind(10, row.recording_mbid);
        stmt.bind(11, row.recording_name);
        stmt.bind(12, row.score);
        stmt.exec();
        stmt.reset();
        
        batch_count++;
        if (batch_count >= COMMIT_INTERVAL) {
            db.exec("COMMIT");
            db.exec("BEGIN TRANSACTION");
            batch_count = 0;
        }
    };
    
    // Run Query 1: Recordings with releases
    lb_log("Query 1: Fetching recordings with releases...");
    if (!run_cursor_query(conn, query1, "cursor1", sqlite_handler, seen_lookups, total_rows, written_rows)) {
        db.exec("ROLLBACK");
        PQexec(conn, "ROLLBACK");
        return false;
    }
    lb_log("Query 1 complete: %s rows processed, %s unique rows written",
           format_number(total_rows).c_str(), format_number(written_rows).c_str());
    
    // Run Query 2: Standalone recordings
    lb_log("Query 2: Fetching standalone recordings (no releases)...");
    size_t standalone_start = total_rows;
    if (!run_cursor_query(conn, query2, "cursor2", sqlite_handler, seen_lookups, total_rows, written_rows)) {
        db.exec("ROLLBACK");
        PQexec(conn, "ROLLBACK");
        return false;
    }
    lb_log("Query 2 complete: %s standalone recordings added",
           format_number(total_rows - standalone_start).c_str());
    
    // Commit remaining rows
    db.exec("COMMIT");
    
    res = PQexec(conn, "COMMIT");
    PQclear(res);
    
    lb_log("SQLite import complete: %s total rows, %s unique rows written", 
           format_number(total_rows).c_str(), format_number(written_rows).c_str());
    
    return true;
}

/**
 * Convenience function that connects to the database and streams to SQLite.
 * Uses the CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable for connection.
 */
inline bool create_canonical_musicbrainz_data_sqlite_from_env(SQLite::Database& db, bool use_minimal_dataset = false) {
    const char* conn_str = getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
    if (!conn_str || strlen(conn_str) == 0) {
        lb_error("CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
        return false;
    }
    
    lb_log("Connecting to PostgreSQL for canonical musicbrainz data...");
    PGconn* conn = PQconnectdb(conn_str);
    
    if (PQstatus(conn) != CONNECTION_OK) {
        lb_error("Connection failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }
    
    bool result = create_canonical_musicbrainz_data_sqlite(conn, db, use_minimal_dataset);
    
    PQfinish(conn);
    return result;
}
