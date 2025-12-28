#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libpq-fe.h>
#include "SQLiteCpp.h"
#include "fuzzy_index.hpp"
#include "tfidf_vectorizer.hpp"
#include "encode.hpp"
#include "utils.hpp"

using namespace std;

#define MAX_NGRAMS 500000

namespace PopularNgram {

inline void generate_ngram_histogram(PGconn* conn, SQLite::Database& db, const string& table_name, const string& column_name, const string& sqlite_table) {
    // Query to get ALL names from the specified table
    lb_log("Querying distinct %s names from PostgreSQL...", table_name.c_str());
    string query_str = "SELECT DISTINCT " + column_name + " FROM musicbrainz." + table_name + " WHERE " + column_name + " IS NOT NULL ORDER BY " + column_name;
    PGresult *res = PQexec(conn, query_str.c_str());
    
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        lb_error("Query failed: %s", PQerrorMessage(conn));
        PQclear(res);
        throw std::runtime_error("PostgreSQL query failed");
    }
    
    // Create vectorizer and tokenize dynamically as we fetch rows
    lb_log("Tokenizing into 3-grams...");
    TfIdfVectorizer vectorizer(false, true, false);
    EncodeSearchData encode;
    
    map<string, int> ngram_counts;
    int num_rows = PQntuples(res);
    
    for (int i = 0; i < num_rows; i++) {
        char* name = PQgetvalue(res, i, 0);
        if (name && strlen(name) > 0) {
            string item_name(name);
            string encoded_name = encode.encode_string(item_name);
            vector<string> tokens = vectorizer.tokenise_document(encoded_name);
            
            // Count each 3-gram
            for (const auto& token : tokens) {
                ngram_counts[token]++;
            }
        }
        
        if ((i + 1) % 1000000 == 0) {
            lb_log("Processed %d %s names...", i + 1, table_name.c_str());
        }
    }
    
    int count = num_rows;
    PQclear(res);
    
    lb_log("Total %s names processed: %d", table_name.c_str(), count);
    
    // Sort by frequency (ascending)
    vector<pair<string, int>> sorted_ngrams(ngram_counts.begin(), ngram_counts.end());
    sort(sorted_ngrams.begin(), sorted_ngrams.end(),
         [](const pair<string, int>& a, const pair<string, int>& b) {
             return a.second < b.second;
         });
    lb_log("Total sorted unique 3-grams: %zu", ngram_counts.size());
    
    // Drop existing table if it exists and create new one
    db.exec("DROP TABLE IF EXISTS " + sqlite_table);
    db.exec("CREATE TABLE " + sqlite_table + " (ngram TEXT NOT NULL, frequency INTEGER NOT NULL)");
    
    // Write 3-grams to SQLite table
    SQLite::Statement insert(db, "INSERT INTO " + sqlite_table + " (ngram, frequency) VALUES (?, ?)");
    
    db.exec("BEGIN TRANSACTION");
    int total = 0;
    for (const auto& pair : sorted_ngrams) {
        insert.bind(1, pair.first);
        insert.bind(2, pair.second);
        insert.exec();
        insert.reset();
        total++;
        if (total >= MAX_NGRAMS)
            break;
    }
    db.exec("COMMIT");
    
    lb_log("Successfully wrote %d unique 3-grams to table: %s", total, sqlite_table.c_str());
}

inline void generate_popular_ngrams(SQLite::Database& db) {
    try {
        lb_log("Generating 3-gram histograms from release and recording names...");
        
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
        
        // Generate histogram for release names
        lb_log("\n=== Processing Release Names ===");
        generate_ngram_histogram(conn, db, "release", "name", "release_ngram");
        
        // Generate histogram for recording names
        lb_log("\n=== Processing Recording Names ===");
        generate_ngram_histogram(conn, db, "recording", "name", "recording_ngram");
        
        PQfinish(conn);
        
    } catch (const std::exception& e) {
        lb_error("Error generating 3-gram histogram: %s", e.what());
        throw;
    }
}

inline vector<string> load_ngrams(SQLite::Database& db, const string& sqlite_table) {
    vector<string> ngrams;
    
    try {
        SQLite::Statement query(db, "SELECT ngram FROM " + sqlite_table + " ORDER BY frequency ASC");
        
        while (query.executeStep()) {
            string ngram = query.getColumn(0).getString();
            ngrams.push_back(ngram);
        }
        
        lb_log("Loaded %zu n-grams from table: %s", ngrams.size(), sqlite_table.c_str());
    } catch (const std::exception& e) {
        lb_error("Error loading n-grams from table %s: %s", sqlite_table.c_str(), e.what());
        throw;
    }
    
    return ngrams;
}

inline pair<vector<string>, vector<string>> load_all_ngrams(SQLite::Database& db) {
    auto release_ngrams = load_ngrams(db, "release_ngram");
    auto recording_ngrams = load_ngrams(db, "recording_ngram");
    return make_pair(release_ngrams, recording_ngrams);
}

} // namespace PopularNgram

