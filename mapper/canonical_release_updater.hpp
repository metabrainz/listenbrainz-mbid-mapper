#pragma once

#include <string>
#include <set>
#include <vector>
#include <libpq-fe.h>
#include "utils.hpp"

using namespace std;

/**
 * CanonicalReleaseUpdater - Incremental updates to the canonical_release table.
 * 
 * Instead of rebuilding the entire table, this class re-evaluates canonical releases
 * only for release_groups that have changed. It:
 * 1. Takes a set of changed release_group_ids
 * 2. Deletes existing canonical_release entries for those release_groups
 * 3. Re-selects the best release for each using the same ranking logic as full build
 * 4. Inserts the new canonical releases
 * 
 * Usage:
 *   CanonicalReleaseUpdater updater(pg_conn);
 *   if (updater.update(changed_release_groups)) {
 *       // Success
 *   }
 */
class CanonicalReleaseUpdater {
private:
    PGconn* conn;
    
    /**
     * Build a PostgreSQL array literal from a set of integers.
     * e.g., {1,2,3}
     */
    static string build_pg_array(const set<int>& ids) {
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
    
public:
    CanonicalReleaseUpdater(PGconn* _conn) : conn(_conn) {}
    
    /**
     * Update canonical releases for a set of release_group_ids.
     * 
     * This uses ROW_NUMBER() OVER (PARTITION BY release_group) to select
     * the best release for each release_group, using the same sort criteria
     * as the full build:
     *   1. Release group type sort
     *   2. Format sort
     *   3. Release date
     *   4. Country, artist_credit, name, release_id
     * 
     * @param release_group_ids Set of release_group IDs to update
     * @return true on success, false on error
     */
    bool update(const set<int>& release_group_ids) {
        if (release_group_ids.empty()) {
            lb_log("CanonicalReleaseUpdater: No release_groups to update");
            return true;
        }
        
        lb_log("CanonicalReleaseUpdater: Updating %zu release_groups", release_group_ids.size());
        
        string array_str = build_pg_array(release_group_ids);
        
        // Step 1: Delete existing canonical_release entries for affected release_groups
        // We need to find releases that belong to these release_groups
        const char* delete_query = R"(
            DELETE FROM mapping.canonical_release cr
             WHERE cr.release IN (
                SELECT r.id
                  FROM musicbrainz.release r
                 WHERE r.release_group = ANY($1::int[])
             )
        )";
        
        const char* paramValues[1] = { array_str.c_str() };
        
        PGresult* res = PQexecParams(conn, delete_query, 1, nullptr,
                                      paramValues, nullptr, nullptr, 0);
        
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            lb_error("CanonicalReleaseUpdater: DELETE failed: %s", PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        
        int deleted = atoi(PQcmdTuples(res));
        lb_log("CanonicalReleaseUpdater: Deleted %d old entries", deleted);
        PQclear(res);
        
        // Step 2: Select the best release for each release_group and insert
        // Using ROW_NUMBER() to pick the best one per release_group
        // This mirrors the ORDER BY logic from CanonicalRelease::get_insert_query()
        const char* insert_query = R"(
            WITH ranked_releases AS (
                SELECT r.id AS release_id
                     , r.gid AS release_mbid
                     , rg.id AS release_group_id
                     , ROW_NUMBER() OVER (
                         PARTITION BY rg.id
                         ORDER BY rgcts.sort NULLS LAST
                                , fs.sort NULLS LAST
                                , to_date(date_year::TEXT || '-' ||
                                          COALESCE(date_month,12)::TEXT || '-' ||
                                          COALESCE(date_day,28)::TEXT, 'YYYY-MM-DD')
                                , rc.country NULLS LAST
                                , rg.artist_credit
                                , rg.name
                                , r.id
                       ) AS rnum
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
             LEFT JOIN musicbrainz.release_group_primary_type rgpt
                    ON rg.type = rgpt.id
             LEFT JOIN musicbrainz.release_group_secondary_type_join rgstj
                    ON rg.id = rgstj.release_group
             LEFT JOIN musicbrainz.release_group_secondary_type rgst
                    ON rgstj.secondary_type = rgst.id
             LEFT JOIN mapping.release_group_combined_type_sort rgcts
                    ON (rgpt.id = rgcts.primary_type OR (rgpt.id IS NULL AND rgcts.primary_type IS NULL))
                   AND (rgst.id = rgcts.secondary_type OR (rgst.id IS NULL AND rgcts.secondary_type IS NULL))
                 WHERE rg.id = ANY($1::int[])
            )
            INSERT INTO mapping.canonical_release (release, release_mbid)
            SELECT release_id, release_mbid
              FROM ranked_releases
             WHERE rnum = 1
        )";
        
        res = PQexecParams(conn, insert_query, 1, nullptr,
                            paramValues, nullptr, nullptr, 0);
        
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            lb_error("CanonicalReleaseUpdater: INSERT failed: %s", PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        
        int inserted = atoi(PQcmdTuples(res));
        lb_log("CanonicalReleaseUpdater: Inserted %d new entries", inserted);
        PQclear(res);
        
        return true;
    }
    
    /**
     * Update canonical releases in batches to avoid long-running transactions.
     * 
     * @param release_group_ids Set of release_group IDs to update
     * @param batch_size Number of release_groups to process per batch
     * @return true on success, false on error
     */
    bool update_batched(const set<int>& release_group_ids, size_t batch_size = 10000) {
        if (release_group_ids.empty()) {
            lb_log("CanonicalReleaseUpdater: No release_groups to update");
            return true;
        }
        
        lb_log("CanonicalReleaseUpdater: Updating %zu release_groups in batches of %zu",
               release_group_ids.size(), batch_size);
        
        // Convert set to vector for easier batching
        vector<int> ids(release_group_ids.begin(), release_group_ids.end());
        
        size_t total_processed = 0;
        size_t batch_num = 0;
        
        while (total_processed < ids.size()) {
            batch_num++;
            
            // Build batch
            set<int> batch;
            size_t batch_end = min(total_processed + batch_size, ids.size());
            for (size_t i = total_processed; i < batch_end; i++) {
                batch.insert(ids[i]);
            }
            
            // Begin transaction for this batch
            PGresult* res = PQexec(conn, "BEGIN");
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                lb_error("CanonicalReleaseUpdater: BEGIN failed: %s", PQerrorMessage(conn));
                PQclear(res);
                return false;
            }
            PQclear(res);
            
            // Update this batch
            if (!update(batch)) {
                PQexec(conn, "ROLLBACK");
                return false;
            }
            
            // Commit this batch
            res = PQexec(conn, "COMMIT");
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                lb_error("CanonicalReleaseUpdater: COMMIT failed: %s", PQerrorMessage(conn));
                PQclear(res);
                return false;
            }
            PQclear(res);
            
            total_processed = batch_end;
            lb_log("CanonicalReleaseUpdater: Batch %zu complete, %zu/%zu release_groups processed",
                   batch_num, total_processed, ids.size());
        }
        
        lb_log("CanonicalReleaseUpdater: All %zu release_groups updated successfully",
               release_group_ids.size());
        return true;
    }
    
    /**
     * Get the release_ids that are currently canonical for given release_groups.
     * Useful for determining what changed (for downstream updates).
     * 
     * @param release_group_ids Set of release_group IDs to check
     * @return Set of release_ids that are canonical for those release_groups
     */
    set<int> get_canonical_releases_for_groups(const set<int>& release_group_ids) {
        set<int> result;
        
        if (release_group_ids.empty()) {
            return result;
        }
        
        string array_str = build_pg_array(release_group_ids);
        
        const char* query = R"(
            SELECT cr.release
              FROM mapping.canonical_release cr
              JOIN musicbrainz.release r ON r.id = cr.release
             WHERE r.release_group = ANY($1::int[])
        )";
        
        const char* paramValues[1] = { array_str.c_str() };
        
        PGresult* res = PQexecParams(conn, query, 1, nullptr,
                                      paramValues, nullptr, nullptr, 0);
        
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            lb_error("CanonicalReleaseUpdater: Query failed: %s", PQerrorMessage(conn));
            PQclear(res);
            return result;
        }
        
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; i++) {
            result.insert(atoi(PQgetvalue(res, i, 0)));
        }
        
        PQclear(res);
        return result;
    }
};

/**
 * Convenience function to update canonical releases for changed release_groups.
 */
inline bool update_canonical_releases(PGconn* conn, const set<int>& release_group_ids, 
                                       bool use_batching = true, size_t batch_size = 10000) {
    CanonicalReleaseUpdater updater(conn);
    
    if (use_batching && release_group_ids.size() > batch_size) {
        return updater.update_batched(release_group_ids, batch_size);
    } else {
        // For smaller updates, wrap in a single transaction
        PGresult* res = PQexec(conn, "BEGIN");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            lb_error("update_canonical_releases: BEGIN failed: %s", PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        PQclear(res);
        
        bool success = updater.update(release_group_ids);
        
        if (success) {
            res = PQexec(conn, "COMMIT");
            PQclear(res);
        } else {
            res = PQexec(conn, "ROLLBACK");
            PQclear(res);
        }
        
        return success;
    }
}
