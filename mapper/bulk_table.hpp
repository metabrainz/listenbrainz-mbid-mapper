#pragma once

#include <string>
#include <vector>
#include <tuple>
#include <functional>
#include <memory>
#include <libpq-fe.h>
#include "utils.hpp"

using namespace std;

// Default batch size for inserts
const int DEFAULT_BATCH_SIZE = 5000;

/**
 * Column definition for table creation
 */
struct ColumnDef {
    string name;
    string type_and_constraints;
    
    ColumnDef(const string& n, const string& t) : name(n), type_and_constraints(t) {}
};

/**
 * Index definition for index creation
 */
struct IndexDef {
    string name;
    string column_def;
    bool unique;
    
    IndexDef(const string& n, const string& c, bool u = false) : name(n), column_def(c), unique(u) {}
};

/**
 * Manage a bulk insert table with this class by providing only the table
 * definition, index definitions, post processing queries and row processing
 * function. The class will handle the insertion into a tmp table, creating indexes
 * on the temp table and finally swapping the table into production seamlessly.
 */
class BulkInsertTable {
protected:
    string table_name;
    string temp_table_name;
    PGconn* conn;
    vector<string> insert_columns;
    vector<shared_ptr<BulkInsertTable>> additional_tables;
    vector<vector<string>> pending_rows;
    int batch_size;
    int total_rows_inserted;
    bool unlogged;

    /**
     * Execute a SQL statement and check for errors
     */
    bool execute_sql(const string& sql, const string& error_msg) {
        PGresult* result = PQexec(conn, sql.c_str());
        ExecStatusType status = PQresultStatus(result);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            lb_error("%s: %s", error_msg.c_str(), PQerrorMessage(conn));
            PQclear(result);
            return false;
        }
        PQclear(result);
        return true;
    }

    /**
     * Escape a string for SQL (caller must PQfreemem the result)
     */
    string escape_string(const string& str) {
        char* escaped = PQescapeLiteral(conn, str.c_str(), str.length());
        if (!escaped) {
            lb_error("Failed to escape string: %s", PQerrorMessage(conn));
            return "''";
        }
        string result(escaped);
        PQfreemem(escaped);
        return result;
    }

    /**
     * Build and execute a batch INSERT statement
     */
    bool flush_pending_rows() {
        if (pending_rows.empty()) {
            return true;
        }

        // Build column list
        string columns_str;
        for (size_t i = 0; i < insert_columns.size(); i++) {
            if (i > 0) columns_str += ", ";
            columns_str += insert_columns[i];
        }

        // Build VALUES list
        string values_str;
        for (size_t row_idx = 0; row_idx < pending_rows.size(); row_idx++) {
            if (row_idx > 0) values_str += ", ";
            values_str += "(";
            const auto& row = pending_rows[row_idx];
            for (size_t col_idx = 0; col_idx < row.size(); col_idx++) {
                if (col_idx > 0) values_str += ", ";
                values_str += row[col_idx];  // Already escaped or is a number
            }
            values_str += ")";
        }

        string sql = "INSERT INTO " + temp_table_name + " (" + columns_str + ") VALUES " + values_str;
        
        if (!execute_sql(sql, "Failed to insert rows into " + temp_table_name)) {
            return false;
        }

        total_rows_inserted += pending_rows.size();
        pending_rows.clear();
        return true;
    }

public:
    BulkInsertTable(const string& _table_name, PGconn* _conn, int _batch_size = DEFAULT_BATCH_SIZE, bool _unlogged = false)
        : table_name(_table_name)
        , temp_table_name(_table_name + "_tmp")
        , conn(_conn)
        , batch_size(_batch_size)
        , total_rows_inserted(0)
        , unlogged(_unlogged)
    {}

    virtual ~BulkInsertTable() = default;

    /**
     * Add an additional bulk table that will be managed alongside this one.
     * Useful when a single query generates data for multiple tables.
     */
    void add_additional_bulk_table(shared_ptr<BulkInsertTable> bulk_table) {
        additional_tables.push_back(bulk_table);
    }

    /**
     * Return the column definitions for creating the table.
     * Override this in derived classes.
     */
    virtual vector<ColumnDef> get_create_table_columns() = 0;

    /**
     * Return post-processing queries to run after data insertion but before index creation.
     * Override this in derived classes if needed.
     */
    virtual vector<string> get_post_process_queries() {
        return {};
    }

    /**
     * Return index definitions for the table.
     * Override this in derived classes.
     */
    virtual vector<IndexDef> get_index_names() = 0;

    /**
     * Get the table name
     */
    const string& get_table_name() const {
        return table_name;
    }

    /**
     * Get the temp table name
     */
    const string& get_temp_table_name() const {
        return temp_table_name;
    }

    /**
     * Check if the table exists and has data
     */
    bool table_exists() {
        string sql = "SELECT 1 FROM " + table_name + " LIMIT 1";
        PGresult* result = PQexec(conn, sql.c_str());
        ExecStatusType status = PQresultStatus(result);
        bool exists = (status == PGRES_TUPLES_OK && PQntuples(result) > 0);
        PQclear(result);
        return exists;
    }

    /**
     * Create the temp table based on column definitions
     */
    bool create_tables() {
        lb_log("%s: drop old tables, create new tables", table_name.c_str());

        // Handle schema creation if table name contains a schema
        size_t dot_pos = table_name.find('.');
        if (dot_pos != string::npos) {
            string schema = table_name.substr(0, dot_pos);
            string create_schema_sql = "CREATE SCHEMA IF NOT EXISTS " + schema;
            // Ignore errors on schema creation (may not have privileges)
            PGresult* result = PQexec(conn, create_schema_sql.c_str());
            PQclear(result);
        }

        // Build column definitions
        vector<string> column_strs;
        insert_columns.clear();
        
        for (const auto& col : get_create_table_columns()) {
            column_strs.push_back(col.name + " " + col.type_and_constraints);
            // Don't include auto-generated columns in insert column list
            if (col.name != "id" && col.type_and_constraints.find("SERIAL") == string::npos) {
                insert_columns.push_back(col.name);
            }
        }

        string columns_str;
        for (size_t i = 0; i < column_strs.size(); i++) {
            if (i > 0) columns_str += ", ";
            columns_str += column_strs[i];
        }

        // Drop existing temp table
        if (!execute_sql("DROP TABLE IF EXISTS " + temp_table_name, 
                        "Failed to drop temp table " + temp_table_name)) {
            return false;
        }

        // Create new temp table
        string create_sql = "CREATE ";
        if (unlogged) create_sql += "UNLOGGED ";
        create_sql += "TABLE " + temp_table_name + " (" + columns_str + ")";
        
        if (!execute_sql(create_sql, "Failed to create temp table " + temp_table_name)) {
            return false;
        }

        // Chain to additional tables
        for (auto& table : additional_tables) {
            if (!table->create_tables()) {
                return false;
            }
        }

        return true;
    }

    /**
     * Add a row to be inserted. The row should contain pre-escaped values or numeric strings.
     * Returns true if successful, false on error.
     */
    bool add_row(const vector<string>& row) {
        pending_rows.push_back(row);
        
        if (pending_rows.size() >= static_cast<size_t>(batch_size)) {
            return flush_pending_rows();
        }
        return true;
    }

    /**
     * Add a row to a specific table (this one or an additional table)
     */
    bool add_row_to_table(const string& target_table, const vector<string>& row) {
        if (target_table == table_name) {
            return add_row(row);
        }
        
        for (auto& table : additional_tables) {
            if (table->get_table_name() == target_table) {
                return table->add_row(row);
            }
        }
        
        lb_error("Unknown table: %s", target_table.c_str());
        return false;
    }

    /**
     * Flush any remaining pending rows
     */
    bool flush() {
        if (!flush_pending_rows()) {
            return false;
        }
        
        for (auto& table : additional_tables) {
            if (!table->flush()) {
                return false;
            }
        }
        
        return true;
    }

    /**
     * Run post-processing queries
     */
    bool post_process() {
        lb_log("%s: post process inserted rows", table_name.c_str());
        
        for (const auto& query : get_post_process_queries()) {
            if (!execute_sql(query, "Failed to run post-process query")) {
                return false;
            }
        }

        for (auto& table : additional_tables) {
            if (!table->post_process()) {
                return false;
            }
        }

        return true;
    }

    /**
     * Create indexes on the temp table
     */
    bool create_indexes(bool no_analyze = false) {
        lb_log("%s: create indexes", table_name.c_str());

        for (const auto& idx : get_index_names()) {
            string uniq = idx.unique ? "UNIQUE " : "";
            string col_def = idx.column_def;
            
            // Add parentheses if not already present
            if (col_def.find('(') == string::npos) {
                col_def = "(" + col_def + ")";
            }

            string sql = "CREATE " + uniq + "INDEX " + idx.name + "_tmp ON " + temp_table_name + " " + col_def;
            if (!execute_sql(sql, "Failed to create index " + idx.name)) {
                return false;
            }
        }

        // Handle SERIAL column sequence
        for (const auto& col : get_create_table_columns()) {
            if (col.name == "id" && col.type_and_constraints.find("SERIAL") != string::npos) {
                lb_log("%s: set sequence value", table_name.c_str());
                string sql = "SELECT setval('" + temp_table_name + "_id_seq', COALESCE(max(id), 0) + 1, false) FROM " + temp_table_name;
                if (!execute_sql(sql, "Failed to set sequence value")) {
                    return false;
                }
                break;
            }
        }

        if (!no_analyze) {
            lb_log("%s: analyze table", table_name.c_str());
            if (!execute_sql("ANALYZE " + temp_table_name, "Failed to analyze table")) {
                return false;
            }
        }

        for (auto& table : additional_tables) {
            if (!table->create_indexes(no_analyze)) {
                return false;
            }
        }

        return true;
    }

    /**
     * Swap the temp table into production.
     * If no_swap_transaction is true, caller is responsible for transaction management.
     */
    bool swap_into_production(bool no_swap_transaction = false) {
        lb_log("%s: swap tables and indexes into production", table_name.c_str());

        // Parse schema and simple table name
        string schema;
        string simple_table_name;
        size_t dot_pos = table_name.find('.');
        if (dot_pos != string::npos) {
            schema = table_name.substr(0, dot_pos);
            simple_table_name = table_name.substr(dot_pos + 1);
        } else {
            simple_table_name = table_name;
        }

        // Drop old table
        if (!execute_sql("DROP TABLE IF EXISTS " + table_name, 
                        "Failed to drop old table " + table_name)) {
            return false;
        }

        // Rename temp table
        if (!execute_sql("ALTER TABLE " + temp_table_name + " RENAME TO " + simple_table_name,
                        "Failed to rename temp table")) {
            return false;
        }

        // Rename indexes
        for (const auto& idx : get_index_names()) {
            string sql;
            if (!schema.empty()) {
                sql = "ALTER INDEX " + schema + "." + idx.name + "_tmp RENAME TO " + idx.name;
            } else {
                sql = "ALTER INDEX " + idx.name + "_tmp RENAME TO " + idx.name;
            }
            if (!execute_sql(sql, "Failed to rename index " + idx.name)) {
                return false;
            }
        }

        // Rename sequence if exists
        string seq_sql = "ALTER SEQUENCE IF EXISTS " + temp_table_name + "_id_seq RENAME TO " + simple_table_name + "_id_seq";
        execute_sql(seq_sql, "");  // Ignore errors, sequence may not exist

        if (!no_swap_transaction) {
            for (auto& table : additional_tables) {
                if (!table->swap_into_production(no_swap_transaction)) {
                    return false;
                }
            }
        }

        return true;
    }

    /**
     * Get total number of rows inserted
     */
    int get_total_rows_inserted() const {
        return total_rows_inserted;
    }

    /**
     * Helper to escape a string value for insertion
     */
    string escape_value(const string& val) {
        return escape_string(val);
    }

    /**
     * Helper to convert an integer to string for insertion
     */
    static string int_value(int val) {
        return to_string(val);
    }

    /**
     * Helper to convert an unsigned int to string for insertion
     */
    static string uint_value(unsigned int val) {
        return to_string(val);
    }

    /**
     * Helper for NULL values
     */
    static string null_value() {
        return "NULL";
    }

    /**
     * Run the complete bulk insert process:
     * 1. Create tables
     * 2. (Caller inserts data via add_row)
     * 3. Flush remaining rows
     * 4. Post-process
     * 5. Create indexes
     * 6. Swap into production
     * 
     * This is a convenience method that wraps the typical workflow.
     * For custom workflows, call individual methods directly.
     */
    bool finalize(bool no_swap = false, bool no_analyze = false) {
        lb_log("%s: finalizing bulk insert", table_name.c_str());

        // Flush any remaining rows
        if (!flush()) {
            return false;
        }

        lb_log("%s: complete! inserted %d rows total", table_name.c_str(), total_rows_inserted);

        // Post-process
        if (!post_process()) {
            return false;
        }

        // Create indexes
        if (!create_indexes(no_analyze)) {
            return false;
        }

        // Swap into production
        if (!no_swap) {
            if (!swap_into_production()) {
                return false;
            }
        } else {
            lb_log("%s: defer swap tables", table_name.c_str());
        }

        lb_log("%s: done", table_name.c_str());
        return true;
    }

    /**
     * Run everything within a transaction.
     * Begins transaction, calls create_tables(), then returns.
     * Caller should add rows, then call finalize_with_transaction().
     */
    bool begin_transaction() {
        if (!execute_sql("BEGIN", "Failed to begin transaction")) {
            return false;
        }
        
        // Suppress NOTICE messages
        execute_sql("SET LOCAL client_min_messages TO WARNING", "");
        
        return true;
    }

    /**
     * Commit the current transaction
     */
    bool commit_transaction() {
        return execute_sql("COMMIT", "Failed to commit transaction");
    }

    /**
     * Rollback the current transaction
     */
    bool rollback_transaction() {
        return execute_sql("ROLLBACK", "Failed to rollback transaction");
    }
};

/**
 * Helper class for running a bulk insert operation with automatic transaction management
 * and proper cleanup on errors.
 */
class BulkInsertTransaction {
private:
    BulkInsertTable& table;
    bool committed;
    bool started;

public:
    BulkInsertTransaction(BulkInsertTable& t) : table(t), committed(false), started(false) {}
    
    ~BulkInsertTransaction() {
        if (started && !committed) {
            table.rollback_transaction();
        }
    }

    bool begin() {
        if (!table.begin_transaction()) {
            return false;
        }
        started = true;
        
        if (!table.create_tables()) {
            return false;
        }
        
        return true;
    }

    bool commit(bool no_swap = false, bool no_analyze = false) {
        if (!table.finalize(no_swap, no_analyze)) {
            return false;
        }
        
        if (!table.commit_transaction()) {
            return false;
        }
        
        committed = true;
        return true;
    }

    void rollback() {
        if (started && !committed) {
            table.rollback_transaction();
            committed = true;  // Prevent double rollback in destructor
        }
    }
};
