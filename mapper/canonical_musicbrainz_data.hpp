#pragma once

#include <libpq-fe.h>
#include <string>
#include <unordered_set>
#include <fstream>
#include <cctype>
#include "utils.hpp"
#include "unidecode/unidecode.hpp"

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

inline string escape_csv(const string& field) {
    if (field.find(',') == string::npos && 
        field.find('"') == string::npos &&
        field.find('\n') == string::npos &&
        field.find('\r') == string::npos) {
        return field;
    }
    string escaped = "\"";
    for (char c : field) {
        if (c == '"') escaped += "\"\"";
        else escaped += c;
    }
    escaped += "\"";
    return escaped;
}

/**
 * Create the canonical musicbrainz data CSV file directly from the database.
 * This skips creating the intermediate PostgreSQL table and does deduplication in memory.
 * 
 * Requires: mapping.canonical_release table to exist first.
 * 
 * Output columns match import.csv ground truth:
 *   artist_credit_id, artist_mbids, artist_credit_name, artist_credit_sortname,
 *   release_id, release_mbid, release_artist_credit_id, release_name,
 *   recording_id, recording_mbid, recording_name, score
 */

// Helper function to process rows from a query result and write to CSV
// Deduplication is by combined_lookup (artist+recording+release name normalized), keeping lowest score
inline size_t process_csv_rows(PGresult* res, ofstream& csv_file, unordered_set<string>& seen_lookups,
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
        
        string recording_mbid = PQgetvalue(res, i, 9);
        string release_mbid = PQgetvalue(res, i, 5);
        
        // Parse PostgreSQL arrays - remove { } braces
        string artist_mbids = PQgetvalue(res, i, 1);
        if (artist_mbids.length() >= 2 && artist_mbids[0] == '{' && artist_mbids.back() == '}') {
            artist_mbids = artist_mbids.substr(1, artist_mbids.length() - 2);
        }
        
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
        
        // Write CSV row
        csv_file << PQgetvalue(res, i, 0) << ","
                 << escape_csv(artist_mbids) << ","
                 << escape_csv(artist_credit_name) << ","
                 << escape_csv(artist_credit_sortname) << ","
                 << PQgetvalue(res, i, 4) << ","
                 << escape_csv(release_mbid) << ","
                 << PQgetvalue(res, i, 6) << ","
                 << escape_csv(release_name) << ","
                 << PQgetvalue(res, i, 8) << ","
                 << escape_csv(recording_mbid) << ","
                 << escape_csv(recording_name) << ","
                 << PQgetvalue(res, i, 11) << "\n";
        
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
                             ofstream& csv_file, unordered_set<string>& seen_pairs,
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
        
        process_csv_rows(res, csv_file, seen_pairs, total_rows, written_rows);
        PQclear(res);
    }
    
    res = PQexec(conn, ("CLOSE " + cursor_name).c_str());
    PQclear(res);
    
    return true;
}

inline bool create_canonical_musicbrainz_data_csv(PGconn* conn, const string& csv_path, bool use_minimal_dataset = false) {
    
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
    
    lb_log("Starting canonical musicbrainz data export...");
    
    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        lb_error("BEGIN failed: %s", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);
    
    ofstream csv_file(csv_path);
    if (!csv_file.is_open()) {
        lb_error("Failed to open CSV file: %s", csv_path.c_str());
        PQexec(conn, "ROLLBACK");
        return false;
    }
    
    // Header
    csv_file << "artist_credit_id,artist_mbids,artist_credit_name,artist_credit_sortname,"
             << "release_id,release_mbid,release_artist_credit_id,release_name,"
             << "recording_id,recording_mbid,recording_name,score\n";
    
    unordered_set<string> seen_lookups;
    size_t total_rows = 0;
    size_t written_rows = 0;
    
    // Run Query 1: Recordings with releases
    lb_log("Query 1: Fetching recordings with releases...");
    if (!run_cursor_query(conn, query1, "cursor1", csv_file, seen_lookups, total_rows, written_rows)) {
        csv_file.close();
        PQexec(conn, "ROLLBACK");
        return false;
    }
    lb_log("Query 1 complete: %s rows processed, %s unique rows written",
           format_number(total_rows).c_str(), format_number(written_rows).c_str());
    
    // Run Query 2: Standalone recordings
    lb_log("Query 2: Fetching standalone recordings (no releases)...");
    size_t standalone_start = total_rows;
    if (!run_cursor_query(conn, query2, "cursor2", csv_file, seen_lookups, total_rows, written_rows)) {
        csv_file.close();
        PQexec(conn, "ROLLBACK");
        return false;
    }
    lb_log("Query 2 complete: %s standalone recordings added",
           format_number(total_rows - standalone_start).c_str());
    
    csv_file.close();
    
    res = PQexec(conn, "COMMIT");
    PQclear(res);
    
    lb_log("CSV export complete: %s total rows, %s unique rows written", 
           format_number(total_rows).c_str(), format_number(written_rows).c_str());
    
    return true;
}

/**
 * Convenience function that connects to the database and creates the CSV.
 * Uses the CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable for connection.
 */
inline bool create_canonical_musicbrainz_data_csv_from_env(const string& csv_path, bool use_minimal_dataset = false) {
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
    
    bool result = create_canonical_musicbrainz_data_csv(conn, csv_path, use_minimal_dataset);
    
    PQfinish(conn);
    return result;
}
