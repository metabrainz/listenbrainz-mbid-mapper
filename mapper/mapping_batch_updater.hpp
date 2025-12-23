#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>

#include "utils.hpp"
#include "SQLiteCpp.h"
#include "canonical_musicbrainz_data.hpp"  // For MappingRowData

using namespace std;

/**
 * MappingBatchUpdater - Atomically updates mapping and index_cache tables for a batch.
 * 
 * This class handles the atomic update of both tables in a single transaction:
 * 1. Delete old mapping rows for the batch's artist_credit_ids
 * 2. Insert new mapping rows
 * 3. Delete old index_cache rows for the batch's artist_credit_ids
 * 4. Insert new index_cache rows
 * 
 * If any step fails, the entire transaction is rolled back.
 * 
 * Usage:
 *   MappingBatchUpdater updater(db);
 *   bool success = updater.update(artist_credit_ids, mapping_rows, index_blobs);
 */
class MappingBatchUpdater {
private:
    SQLite::Database& db;
    
    /**
     * Build a SQL IN clause for a set of integers.
     * Returns "(1,2,3,4,...)" format.
     */
    static string build_in_clause(const set<int>& ids) {
        string result = "(";
        bool first = true;
        for (int id : ids) {
            if (!first) result += ",";
            result += to_string(id);
            first = false;
        }
        result += ")";
        return result;
    }
    
public:
    MappingBatchUpdater(SQLite::Database& _db) : db(_db) {}
    
    /**
     * Atomically update mapping and index_cache tables for a batch.
     * 
     * @param artist_credit_ids Set of artist_credit_ids being updated
     * @param mapping_rows New mapping rows to insert
     * @param index_blobs Map of artist_credit_id → serialized index blob
     * @return true on success (committed), false on failure (rolled back)
     */
    bool update(const set<int>& artist_credit_ids,
                const vector<MappingRowData>& mapping_rows,
                const map<int, string>& index_blobs) {
        
        if (artist_credit_ids.empty()) {
            lb_log("No artist_credit_ids to update");
            return true;
        }
        
        lb_log("Updating SQLite for %zu artist_credit_ids (%zu mapping rows, %zu index blobs)...",
               artist_credit_ids.size(), mapping_rows.size(), index_blobs.size());
        
        try {
            // Start transaction
            SQLite::Transaction transaction(db);
            
            // Build IN clause once for delete statements
            string in_clause = build_in_clause(artist_credit_ids);
            
            // Step 1: Delete old mapping rows
            string delete_mapping_sql = "DELETE FROM mapping WHERE artist_credit_id IN " + in_clause;
            int deleted_mapping = db.exec(delete_mapping_sql);
            lb_log("  Deleted %d old mapping rows", deleted_mapping);
            
            // Step 2: Insert new mapping rows
            if (!mapping_rows.empty()) {
                SQLite::Statement insert_mapping(db,
                    "INSERT INTO mapping VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
                
                for (const auto& row : mapping_rows) {
                    insert_mapping.bind(1, row.artist_credit_id);
                    insert_mapping.bind(2, row.artist_mbids);
                    insert_mapping.bind(3, row.artist_credit_name);
                    insert_mapping.bind(4, row.artist_credit_sortname);
                    insert_mapping.bind(5, row.release_id);
                    insert_mapping.bind(6, row.release_mbid);
                    insert_mapping.bind(7, row.release_artist_credit_id);
                    insert_mapping.bind(8, row.release_name);
                    insert_mapping.bind(9, row.recording_id);
                    insert_mapping.bind(10, row.recording_mbid);
                    insert_mapping.bind(11, row.recording_name);
                    insert_mapping.bind(12, row.score);
                    insert_mapping.exec();
                    insert_mapping.reset();
                }
                lb_log("  Inserted %zu new mapping rows", mapping_rows.size());
            }
            
            // Step 3: Delete old index_cache rows
            string delete_cache_sql = "DELETE FROM index_cache WHERE entity_id IN " + in_clause;
            int deleted_cache = db.exec(delete_cache_sql);
            lb_log("  Deleted %d old index_cache rows", deleted_cache);
            
            // Step 4: Insert new index_cache rows
            if (!index_blobs.empty()) {
                SQLite::Statement insert_cache(db,
                    "INSERT INTO index_cache (entity_id, index_data) VALUES (?, ?)");
                
                for (const auto& [entity_id, blob] : index_blobs) {
                    insert_cache.bind(1, entity_id);
                    insert_cache.bind(2, blob.data(), static_cast<int>(blob.size()));
                    insert_cache.exec();
                    insert_cache.reset();
                }
                lb_log("  Inserted %zu new index_cache rows", index_blobs.size());
            }
            
            // Commit transaction
            transaction.commit();
            lb_log("  Transaction committed successfully");
            
            return true;
            
        } catch (const exception& e) {
            lb_error("SQLite batch update failed: %s", e.what());
            lb_error("Transaction rolled back");
            return false;
        }
    }
    
    /**
     * Delete all data for a set of artist_credit_ids (no replacement).
     * Used when artist_credits have been deleted from MusicBrainz.
     * 
     * @param artist_credit_ids Set of artist_credit_ids to delete
     * @return true on success, false on failure
     */
    bool delete_only(const set<int>& artist_credit_ids) {
        if (artist_credit_ids.empty()) {
            return true;
        }
        
        lb_log("Deleting SQLite data for %zu artist_credit_ids...", artist_credit_ids.size());
        
        try {
            SQLite::Transaction transaction(db);
            
            string in_clause = build_in_clause(artist_credit_ids);
            
            string delete_mapping_sql = "DELETE FROM mapping WHERE artist_credit_id IN " + in_clause;
            int deleted_mapping = db.exec(delete_mapping_sql);
            
            string delete_cache_sql = "DELETE FROM index_cache WHERE entity_id IN " + in_clause;
            int deleted_cache = db.exec(delete_cache_sql);
            
            transaction.commit();
            
            lb_log("  Deleted %d mapping rows and %d index_cache rows", 
                   deleted_mapping, deleted_cache);
            
            return true;
            
        } catch (const exception& e) {
            lb_error("SQLite delete failed: %s", e.what());
            return false;
        }
    }
};
