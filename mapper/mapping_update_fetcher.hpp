#pragma once

#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <libpq-fe.h>
#include "utils.hpp"
#include "canonical_musicbrainz_data.hpp"  // For MappingRowData, compute_combined_lookup, make_mapping_query

using namespace std;

/**
 * MappingUpdateFetcher - Fetches mapping data from PostgreSQL for specific artist_credit_ids.
 * 
 * This is used for incremental updates to fetch only the changed data rather than
 * the entire dataset. It uses make_mapping_query() with the artist_credit_id filter.
 * 
 * Usage:
 *   MappingUpdateFetcher fetcher(pg_conn);
 *   vector<MappingRowData> rows = fetcher.fetch(artist_credit_ids);
 */
class MappingUpdateFetcher {
private:
    PGconn* conn;
    
    /**
     * Convert a set of integers to a PostgreSQL array string.
     */
    static string to_pg_array(const set<int>& ids) {
        string result = "{";
        bool first = true;
        for (int id : ids) {
            if (!first) result += ",";
            result += to_string(id);
            first = false;
        }
        result += "}";
        return result;
    }
    
    /**
     * Parse a single row from the query result into a MappingRowData struct.
     */
    static MappingRowData parse_row(PGresult* res, int row_idx) {
        MappingRowData row;
        
        // Parse integers
        char* val = PQgetvalue(res, row_idx, 0);
        row.artist_credit_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, row_idx, 4);
        row.release_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, row_idx, 6);
        row.release_artist_credit_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, row_idx, 8);
        row.recording_id = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        val = PQgetvalue(res, row_idx, 11);
        row.score = (val && *val) ? strtoll(val, nullptr, 10) : 0;
        
        // String fields
        row.artist_credit_name = PQgetvalue(res, row_idx, 2);
        row.release_name = PQgetvalue(res, row_idx, 7);
        row.recording_name = PQgetvalue(res, row_idx, 10);
        row.recording_mbid = PQgetvalue(res, row_idx, 9);
        row.release_mbid = PQgetvalue(res, row_idx, 5);
        
        // Parse PostgreSQL arrays - remove { } braces
        string artist_mbids = PQgetvalue(res, row_idx, 1);
        if (artist_mbids.length() >= 2 && artist_mbids[0] == '{' && artist_mbids.back() == '}') {
            artist_mbids = artist_mbids.substr(1, artist_mbids.length() - 2);
        }
        row.artist_mbids = artist_mbids;
        
        // Get first sortname from array for artist_credit_sortname
        string sortnames_raw = PQgetvalue(res, row_idx, 3);
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
        
        return row;
    }
    
    /**
     * Execute a query and collect results with deduplication.
     */
    bool execute_query(const string& query, const string& pg_array,
                       vector<MappingRowData>& results,
                       unordered_set<string>& seen_lookups,
                       const char* description) {
        const char* paramValues[1] = { pg_array.c_str() };
        
        PGresult* res = PQexecParams(conn, query.c_str(), 1, nullptr,
                                      paramValues, nullptr, nullptr, 0);
        
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            lb_error("Query failed for %s: %s", description, PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        
        int nrows = PQntuples(res);
        size_t added = 0;
        
        for (int i = 0; i < nrows; i++) {
            MappingRowData row = parse_row(res, i);
            
            // Deduplicate by combined_lookup (same logic as full build)
            string combined_lookup = compute_combined_lookup(
                row.artist_credit_name, row.recording_name, row.release_name);
            
            if (seen_lookups.insert(combined_lookup).second) {
                results.push_back(row);
                added++;
            }
        }
        
        lb_log("  %s: %d rows queried, %zu unique rows added", description, nrows, added);
        PQclear(res);
        return true;
    }
    
public:
    MappingUpdateFetcher(PGconn* _conn) : conn(_conn) {}
    
    /**
     * Fetch mapping data for a set of artist_credit_ids.
     * 
     * @param artist_credit_ids Set of artist_credit_ids to fetch
     * @return Vector of MappingRowData, deduplicated by combined_lookup
     */
    vector<MappingRowData> fetch(const set<int>& artist_credit_ids) {
        vector<MappingRowData> results;
        unordered_set<string> seen_lookups;
        
        if (artist_credit_ids.empty()) {
            return results;
        }
        
        lb_log("Fetching mapping data for %zu artist_credit_ids...", artist_credit_ids.size());
        
        string pg_array = to_pg_array(artist_credit_ids);
        
        // Use make_mapping_query with the parameterized filter
        string query_with_releases = make_mapping_query(true, "ac.id = ANY($1::int[])");
        string query_standalone = make_mapping_query(false, "ac.id = ANY($1::int[])");
        
        // Query recordings with releases first (these have valid scores)
        if (!execute_query(query_with_releases, pg_array, results, seen_lookups, "with releases")) {
            return {};  // Return empty on error
        }
        
        // Then query standalone recordings
        if (!execute_query(query_standalone, pg_array, results, seen_lookups, "standalone")) {
            return {};  // Return empty on error
        }
        
        lb_log("Total: %zu mapping rows fetched", results.size());
        return results;
    }
    
    /**
     * Fetch mapping data for a batch from a larger set.
     * Useful for processing large change sets in chunks.
     * 
     * @param all_ids Full set of artist_credit_ids
     * @param batch_start Iterator to start of batch
     * @param batch_size Number of IDs to include in batch
     * @return Vector of MappingRowData for this batch
     */
    vector<MappingRowData> fetch_batch(const set<int>& all_ids, 
                                        set<int>::const_iterator batch_start,
                                        size_t batch_size) {
        set<int> batch;
        auto it = batch_start;
        for (size_t i = 0; i < batch_size && it != all_ids.end(); i++, ++it) {
            batch.insert(*it);
        }
        return fetch(batch);
    }
};
