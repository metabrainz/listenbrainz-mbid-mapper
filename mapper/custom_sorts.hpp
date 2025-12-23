#pragma once

#include <string>
#include <vector>
#include <optional>
#include <libpq-fe.h>
#include "utils.hpp"

using namespace std;

// Release group primary types in sort order
const vector<pair<int, string>> RELEASE_GROUP_PRIMARY_TYPES = {
    {1, "Album"},
    {2, "Single"},
    {3, "EP"},
    {11, "Other"},
    {12, "Broadcast"}
};

// Release group secondary types in sort order
const vector<pair<int, string>> RELEASE_GROUP_SECONDARY_TYPES = {
    {2, "Soundtrack"},
    {9, "Mixtape/Street"},
    {7, "Remix"},
    {5, "Audiobook"},
    {11, "Audio drama"},
    {3, "Spokenword"},
    {4, "Interview"},
    {10, "Demo"},
    {6, "Live"},
    {12, "Field recording"},
    {1, "Compilation"},
    {8, "DJ-mix"}
};

// Digital formats in sort order
const vector<pair<int, string>> DIGITAL_FORMATS = {
    {1, "CD"},
    {3, "SACD"},
    {6, "MiniDisc"},
    {11, "DAT"},
    {12, "Digital Media"},
    {16, "DCC"},
    {25, "HDCD"},
    {26, "USB Flash Drive"},
    {27, "slotMusic"},
    {28, "UMD"},
    {33, "CD-R"},
    {34, "8cm CD"},
    {35, "Blu-spec CD"},
    {36, "SHM-CD"},
    {37, "HQCD"},
    {38, "Hybrid SACD"},
    {39, "CD+G"},
    {40, "8cm CD+G"},
    {42, "Enhanced CD"},
    {43, "Data CD"},
    {44, "DTS CD"},
    {45, "Playbutton"},
    {46, "Music Card"},
    {49, "3.5\" Floppy Disk"},
    {57, "SHM-SACD"},
    {60, "CED"},
    {61, "Copy Control CD"},
    {62, "SD Card"},
    {63, "Hybrid SACD (CD layer)"},
    {64, "Hybrid SACD (SACD layer)"},
    {74, "PlayTape"},
    {75, "HiPac"},
    {76, "Floppy Disk"},
    {77, "Zip Disk"},
    {82, "VinylDisc (CD side)"},
    {48, "VinylDisc"}
};

// Video formats in sort order
const vector<pair<int, string>> VIDEO_FORMATS = {
    {2, "DVD"},
    {4, "DualDisc"},
    {5, "LaserDisc"},
    {71, "8\" LaserDisc"},
    {72, "12\" LaserDisc"},
    {17, "HD-DVD"},
    {18, "DVD-Audio"},
    {19, "DVD-Video"},
    {20, "Blu-ray"},
    {22, "VCD"},
    {23, "SVCD"},
    {41, "CDV"},
    {47, "DVDplus"},
    {59, "VHD"},
    {66, "DualDisc (DVD-Video side)"},
    {65, "DualDisc (DVD-Audio side)"},
    {67, "DualDisc (CD side)"},
    {68, "DVDplus (DVD-Audio side)"},
    {69, "DVDplus (DVD-Video side)"},
    {70, "DVDplus (CD side)"},
    {80, "VinylDisc (DVD side)"},
    {79, "Blu-ray-R"}
};

// Analog formats in sort order
const vector<pair<int, string>> ANALOG_FORMATS = {
    {7, "Vinyl"},
    {29, "7\" Vinyl"},
    {30, "10\" Vinyl"},
    {31, "12\" Vinyl"},
    {10, "Reel-to-reel"},
    {8, "Cassette"},
    {9, "Cartridge"},
    {78, "8-Track Cartridge"},
    {13, "Other"},
    {14, "Wax Cylinder"},
    {15, "Piano Roll"},
    {81, "VinylDisc (Vinyl side)"},
    {21, "VHS"},
    {24, "Betamax"},
    {50, "Edison Diamond Disc"},
    {51, "Flexi-disc"},
    {52, "7\" Flexi-disc"},
    {53, "Shellac"},
    {54, "10\" Shellac"},
    {55, "12\" Shellac"},
    {56, "7\" Shellac"},
    {58, "Pathe disc"},
    {73, "Phonograph record"}
};

// Struct to hold combined release group type info
struct CombinedReleaseGroupType {
    optional<int> primary_type_id;
    string primary_type_name;
    optional<int> secondary_type_id;
    string secondary_type_name;
};

/**
 * Get a sort order based on both primary and secondary release group types
 * 
 * We want to sort by primary type first, then secondary type except for one case.
 * Singles and EPs should rank over albums that have a secondary type.
 */
inline vector<CombinedReleaseGroupType> get_combined_release_group_types_sort() {
    // Copy primary types and add NULL option
    vector<pair<optional<int>, string>> primary_types;
    for (const auto& pt : RELEASE_GROUP_PRIMARY_TYPES) {
        primary_types.push_back({pt.first, pt.second});
    }
    primary_types.push_back({nullopt, "NULL"});

    // Copy secondary types with NULL at the beginning
    vector<pair<optional<int>, string>> secondary_types;
    secondary_types.push_back({nullopt, "NULL"});
    for (const auto& st : RELEASE_GROUP_SECONDARY_TYPES) {
        secondary_types.push_back({st.first, st.second});
    }

    // Build combined types list
    vector<CombinedReleaseGroupType> combined_types;
    for (const auto& [primary_id, primary_name] : primary_types) {
        for (const auto& [secondary_id, secondary_name] : secondary_types) {
            combined_types.push_back({primary_id, primary_name, secondary_id, secondary_name});
        }
    }

    // Find and move Single (id=2) with NULL secondary to position 1
    auto single_it = std::find_if(combined_types.begin(), combined_types.end(),
        [](const CombinedReleaseGroupType& t) {
            return t.primary_type_id.has_value() && t.primary_type_id.value() == 2 
                   && !t.secondary_type_id.has_value();
        });
    if (single_it != combined_types.end()) {
        CombinedReleaseGroupType single_item = *single_it;
        combined_types.erase(single_it);
        combined_types.insert(combined_types.begin() + 1, single_item);
    }

    // Find and move EP (id=3) with NULL secondary to position 2
    auto ep_it = std::find_if(combined_types.begin(), combined_types.end(),
        [](const CombinedReleaseGroupType& t) {
            return t.primary_type_id.has_value() && t.primary_type_id.value() == 3 
                   && !t.secondary_type_id.has_value();
        });
    if (ep_it != combined_types.end()) {
        CombinedReleaseGroupType ep_item = *ep_it;
        combined_types.erase(ep_it);
        combined_types.insert(combined_types.begin() + 2, ep_item);
    }

    return combined_types;
}

/**
 * Execute a SQL statement and check for errors
 */
inline bool execute_sql(PGconn* conn, const string& sql, const string& error_msg) {
    PGresult* result = PQexec(conn, sql.c_str());
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        lb_error("%s: %s", error_msg.c_str(), PQerrorMessage(conn));
        PQclear(result);
        return false;
    }
    PQclear(result);
    return true;
}

/**
 * Helper function for inserting format rows
 */
inline int insert_format_rows(PGconn* conn, int sort_index, const vector<pair<int, string>>& formats) {
    for (const auto& [format_id, _] : formats) {
        string sql = "INSERT INTO mapping.format_sort (format, sort) VALUES (" 
                     + to_string(format_id) + ", " + to_string(sort_index) + ")";
        if (!execute_sql(conn, sql, "Failed to insert format row")) {
            throw runtime_error("Failed to insert format row");
        }
        sort_index++;
    }
    return sort_index;
}

/**
 * Ensure the mapping schema exists
 */
inline bool ensure_mapping_schema(PGconn* conn) {
    lb_log("Checking if mapping schema exists...");
    
    if (!execute_sql(conn, "CREATE SCHEMA IF NOT EXISTS mapping", "Failed to create mapping schema")) {
        return false;
    }
    
    lb_log("Mapping schema ready.");
    return true;
}

/**
 * Create the format_sort table
 */
inline bool create_format_sort_table(PGconn* conn) {
    lb_log("Creating format_sort table...");
    
    if (!execute_sql(conn, "DROP TABLE IF EXISTS mapping.format_sort", "Failed to drop format_sort table")) {
        return false;
    }
    
    if (!execute_sql(conn, "CREATE TABLE mapping.format_sort (format integer, sort integer)", 
                     "Failed to create format_sort table")) {
        return false;
    }
    
    try {
        int sort_index = 1;
        sort_index = insert_format_rows(conn, sort_index, DIGITAL_FORMATS);
        sort_index = insert_format_rows(conn, sort_index, VIDEO_FORMATS);
        sort_index = insert_format_rows(conn, sort_index, ANALOG_FORMATS);
    } catch (const exception& e) {
        lb_error("Failed to insert format rows: %s", e.what());
        return false;
    }
    
    if (!execute_sql(conn, "CREATE INDEX format_sort_format_ndx ON mapping.format_sort(format)",
                     "Failed to create format_sort_format_ndx")) {
        return false;
    }
    
    if (!execute_sql(conn, "CREATE INDEX format_sort_sort_ndx ON mapping.format_sort(sort)",
                     "Failed to create format_sort_sort_ndx")) {
        return false;
    }
    
    lb_log("format_sort table created successfully.");
    return true;
}

/**
 * Create the release_group_secondary_type_sort table
 */
inline bool create_release_group_secondary_type_sort_table(PGconn* conn) {
    lb_log("Creating release_group_secondary_type_sort table...");
    
    if (!execute_sql(conn, "DROP TABLE IF EXISTS mapping.release_group_secondary_type_sort",
                     "Failed to drop release_group_secondary_type_sort table")) {
        return false;
    }
    
    if (!execute_sql(conn, 
                     "CREATE TABLE mapping.release_group_secondary_type_sort ("
                     "sort INTEGER, secondary_type INTEGER)",
                     "Failed to create release_group_secondary_type_sort table")) {
        return false;
    }
    
    int sort_index = 1;
    for (const auto& [secondary_type_id, _] : RELEASE_GROUP_SECONDARY_TYPES) {
        string sql = "INSERT INTO mapping.release_group_secondary_type_sort (sort, secondary_type) VALUES ("
                     + to_string(sort_index) + ", " + to_string(secondary_type_id) + ")";
        if (!execute_sql(conn, sql, "Failed to insert release_group_secondary_type_sort row")) {
            return false;
        }
        sort_index++;
    }
    
    if (!execute_sql(conn, 
                     "CREATE INDEX release_group_secondary_type_sort_ndx_secondary_type "
                     "ON mapping.release_group_secondary_type_sort(secondary_type)",
                     "Failed to create release_group_secondary_type_sort_ndx_secondary_type")) {
        return false;
    }
    
    if (!execute_sql(conn,
                     "CREATE INDEX release_group_secondary_type_sort_ndx_sort "
                     "ON mapping.release_group_secondary_type_sort(sort)",
                     "Failed to create release_group_secondary_type_sort_ndx_sort")) {
        return false;
    }
    
    lb_log("release_group_secondary_type_sort table created successfully.");
    return true;
}

/**
 * Create the release_group_combined_type_sort table
 */
inline bool create_release_group_combined_type_sort_table(PGconn* conn) {
    lb_log("Creating release_group_combined_type_sort table...");
    
    if (!execute_sql(conn, "DROP TABLE IF EXISTS mapping.release_group_combined_type_sort",
                     "Failed to drop release_group_combined_type_sort table")) {
        return false;
    }
    
    if (!execute_sql(conn,
                     "CREATE TABLE mapping.release_group_combined_type_sort ("
                     "sort INTEGER, primary_type INTEGER, secondary_type INTEGER)",
                     "Failed to create release_group_combined_type_sort table")) {
        return false;
    }
    
    auto combined_types = get_combined_release_group_types_sort();
    int sort_index = 1;
    
    for (const auto& ct : combined_types) {
        string primary_str = ct.primary_type_id.has_value() ? to_string(ct.primary_type_id.value()) : "NULL";
        string secondary_str = ct.secondary_type_id.has_value() ? to_string(ct.secondary_type_id.value()) : "NULL";
        
        string sql = "INSERT INTO mapping.release_group_combined_type_sort (sort, primary_type, secondary_type) VALUES ("
                     + to_string(sort_index) + ", " + primary_str + ", " + secondary_str + ")";
        if (!execute_sql(conn, sql, "Failed to insert release_group_combined_type_sort row")) {
            return false;
        }
        sort_index++;
    }
    
    if (!execute_sql(conn,
                     "CREATE INDEX release_group_combined_type_sort_ndx_primary_secondary_type "
                     "ON mapping.release_group_combined_type_sort(primary_type, secondary_type)",
                     "Failed to create release_group_combined_type_sort_ndx_primary_secondary_type")) {
        return false;
    }
    
    if (!execute_sql(conn,
                     "CREATE INDEX release_group_combined_type_sort_ndx_sort "
                     "ON mapping.release_group_combined_type_sort(sort)",
                     "Failed to create release_group_combined_type_sort_ndx_sort")) {
        return false;
    }
    
    lb_log("release_group_combined_type_sort table created successfully.");
    return true;
}

/**
 * Create all custom sort tables that contain the preferred sort orders 
 * for releases in the MSB mapping.
 */
inline bool create_custom_sort_tables(PGconn* conn) {
    lb_log("Creating custom sort tables...");
    
    // Start transaction
    if (!execute_sql(conn, "BEGIN", "Failed to begin transaction")) {
        return false;
    }
    
    // Suppress NOTICE messages (e.g., "table does not exist, skipping" from DROP IF EXISTS)
    if (!execute_sql(conn, "SET LOCAL client_min_messages TO WARNING", "Failed to set client_min_messages")) {
        execute_sql(conn, "ROLLBACK", "Failed to rollback");
        return false;
    }
    
    // Ensure the mapping schema exists
    if (!ensure_mapping_schema(conn)) {
        execute_sql(conn, "ROLLBACK", "Failed to rollback");
        return false;
    }
    
    // Create format_sort table
    if (!create_format_sort_table(conn)) {
        execute_sql(conn, "ROLLBACK", "Failed to rollback");
        return false;
    }
    
    // Create release_group_secondary_type_sort table
    if (!create_release_group_secondary_type_sort_table(conn)) {
        execute_sql(conn, "ROLLBACK", "Failed to rollback");
        return false;
    }
    
    // Create release_group_combined_type_sort table
    if (!create_release_group_combined_type_sort_table(conn)) {
        execute_sql(conn, "ROLLBACK", "Failed to rollback");
        return false;
    }
    
    // Commit transaction
    if (!execute_sql(conn, "COMMIT", "Failed to commit transaction")) {
        execute_sql(conn, "ROLLBACK", "Failed to rollback");
        return false;
    }
    
    lb_log("All custom sort tables created successfully.");
    return true;
}

/**
 * Convenience function that connects to the database and creates the custom sort tables.
 * Uses the CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable for connection.
 */
inline bool create_custom_sort_tables_from_env() {
    const char* db_connect = std::getenv("CANONICAL_MUSICBRAINZ_DATA_CONNECT");
    if (!db_connect || strlen(db_connect) == 0) {
        lb_error("CANONICAL_MUSICBRAINZ_DATA_CONNECT environment variable not set");
        return false;
    }
    
    lb_log("Connecting to PostgreSQL...");
    PGconn* conn = PQconnectdb(db_connect);
    
    if (PQstatus(conn) != CONNECTION_OK) {
        lb_error("Connection to database failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }
    
    bool success = create_custom_sort_tables(conn);
    
    PQfinish(conn);
    return success;
}
