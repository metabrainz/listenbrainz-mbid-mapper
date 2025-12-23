#include <string>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <memory>
#include "crow.h"
#include "fsm.hpp"
#include "statistics.hpp"
#include "test_cases.hpp"

using namespace std;

// Global app pointer for signal handler
static crow::SimpleApp* g_app = nullptr;

// Signal handler for graceful shutdown
void signal_handler(int signum) {
    lb_log("Received signal %d, shutting down gracefully...", signum);
    if (g_app) {
        g_app->stop();
    }
}

// Shared resources (created once, shared across all threads)
static string g_index_dir = "/data";
static string g_templates_dir = "/mapper/templates";
static int g_max_process_size = 100;  // max RSS in MB
static int g_cache_cleaner_delay = 60;  // seconds between cache cleaner checks
static int g_num_threads = 0;  // 0 = use all available cores
static int g_timeout = 30;  // connection timeout in seconds (default Crow is 5, too short for complex queries)
static const int REPORT_STATS_REQUEST_COUNT = 10;  // batch size for reporting stats to global
static ArtistIndex* g_artist_index = nullptr;
static IndexCache* g_index_cache = nullptr;
static Statistics* g_statistics = nullptr;
static std::atomic<bool> g_ready{false};

MappingSearch* get_mapping_search() {
    // Use unique_ptr so the MappingSearch is properly deleted when the thread exits
    thread_local std::unique_ptr<MappingSearch> mapping_search;
    if (!mapping_search) {
        mapping_search = std::make_unique<MappingSearch>(g_index_dir, g_artist_index, g_index_cache);
    }
    return mapping_search.get();
}

// Per-thread statistics to reduce mutex contention
struct ThreadStats {
    int count_200 = 0;
    int count_400 = 0;
    int count_404 = 0;
    int total = 0;
};

void update_stats(int status) {
    thread_local ThreadStats stats;
    
    if (status == 200) stats.count_200++;
    else if (status == 400) stats.count_400++;
    else if (status == 404) stats.count_404++;
    
    stats.total++;
    if (stats.total >= REPORT_STATS_REQUEST_COUNT) {
        g_statistics->update(stats.count_200, stats.count_400, stats.count_404);
        stats.count_200 = 0;
        stats.count_400 = 0;
        stats.count_404 = 0;
        stats.total = 0;
    }
}

void print_usage() {
    lb_log("Usage: server");
    lb_log("");
    lb_log("Required environment variables:");
    lb_log("  INDEX_DIR        Directory containing mapping.db and index files");
    lb_log("");
    lb_log("Optional environment variables:");
    lb_log("  HOST             Hostname/IP to bind to (default: 0.0.0.0)");
    lb_log("  PORT             Port number to listen on (default: 5000)");
    lb_log("  TEMPLATE_DIR     Templates directory (default: /mapper/templates)");
    lb_log("  NUM_THREADS      Number of worker threads (0 = auto, default: 0)");
    lb_log("  MAX_PROCESS_SIZE Max process RSS size in MB (default: 100)");
    lb_log("  CACHE_CLEANER_DELAY  Seconds between cache cleaner checks (default: 60)");
    lb_log("  TIMEOUT          Connection timeout in seconds (default: 30)");
}

int main(int argc, char* argv[]) {
    init_logging();
    load_env_file();  // Load .env file, env vars take precedence
    
    // Parse arguments (options only)
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            lb_error("Error: Unknown option: %s", arg.c_str());
            print_usage();
            return 1;
        }
    }
    
    // Get required INDEX_DIR from environment
    const char* env_index_dir = std::getenv("INDEX_DIR");
    if (!env_index_dir || strlen(env_index_dir) == 0) {
        lb_error("Error: INDEX_DIR environment variable not set");
        print_usage();
        return 1;
    }
    g_index_dir = env_index_dir;
    
    // Get optional config from environment variables
    string host = "0.0.0.0";
    int port = 5000;
    
    const char* env_host = std::getenv("HOST");
    if (env_host && strlen(env_host) > 0) {
        host = env_host;
    }
    
    const char* env_port = std::getenv("PORT");
    if (env_port && strlen(env_port) > 0) {
        int p = atoi(env_port);
        if (p > 0 && p <= 65535) port = p;
    }
    
    const char* env_template_dir = std::getenv("TEMPLATE_DIR");
    if (env_template_dir && strlen(env_template_dir) > 0) {
        g_templates_dir = env_template_dir;
    }
    
    const char* env_num_threads = std::getenv("NUM_THREADS");
    if (env_num_threads && strlen(env_num_threads) > 0) {
        g_num_threads = atoi(env_num_threads);
        if (g_num_threads < 0) g_num_threads = 0;
    }
    
    const char* env_max_process_size = std::getenv("MAX_PROCESS_SIZE");
    if (env_max_process_size && strlen(env_max_process_size) > 0) {
        g_max_process_size = atoi(env_max_process_size);
        if (g_max_process_size < 1) g_max_process_size = 100;
    }
    lb_log("Max process size: %d MB", g_max_process_size);
    
    const char* env_cache_cleaner_delay = std::getenv("CACHE_CLEANER_DELAY");
    if (env_cache_cleaner_delay && strlen(env_cache_cleaner_delay) > 0) {
        g_cache_cleaner_delay = atoi(env_cache_cleaner_delay);
        if (g_cache_cleaner_delay < 1) g_cache_cleaner_delay = 60;
    }
    
    const char* env_timeout = std::getenv("TIMEOUT");
    if (env_timeout && strlen(env_timeout) > 0) {
        g_timeout = atoi(env_timeout);
        if (g_timeout < 1) g_timeout = 30;
        if (g_timeout > 255) g_timeout = 255;  // Crow uses uint8_t
    }
    
    g_statistics = new Statistics();

    // Load shared indexes BEFORE starting the server
    lb_log("Loading shared indexes...");
    g_artist_index = new ArtistIndex(g_index_dir);
    if (!g_artist_index->load()) {
        lb_error("Failed to load artist index. INDEX_DIR=%s", g_index_dir.c_str());
        delete g_artist_index;
        return -1;
    }
    
    crow::SimpleApp app;
    g_app = &app;  // Store for signal handler
    crow::mustache::set_global_base(g_templates_dir);
    
    // Register signal handlers for graceful shutdown (needed for ASan leak reports)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create index cache
    g_index_cache = new IndexCache(g_max_process_size, g_cache_cleaner_delay);
    g_index_cache->start();

    g_ready = true;

    CROW_ROUTE(app, "/")
    ([](const crow::request& req) {
        // Show loading page if not ready
        if (!g_ready) {
            auto page_text = crow::mustache::load_text("loading.html");
            if (page_text.empty()) {
                return crow::response(500, "Template \"loading.html\" not found.");
            }
            auto page = crow::mustache::compile(page_text);
            return crow::response(200, page.render());
        }

        auto page_text = crow::mustache::load_text("index.html");
        if (page_text.empty()) {
            return crow::response(500, "Template \"index.html\" not found.");
        }
        auto page = crow::mustache::compile(page_text);
        crow::mustache::context ctx;
        
        auto artist_credit_name = req.url_params.get("artist_credit_name");
        auto release_name = req.url_params.get("release_name");
        auto recording_name = req.url_params.get("recording_name");
        
        // Preserve form values
        ctx["artist_credit_name"] = artist_credit_name ? artist_credit_name : "";
        ctx["release_name"] = release_name ? release_name : "";
        ctx["recording_name"] = recording_name ? recording_name : "";
        ctx["searched"] = false;
        
        // Only search if we have required parameters
        if (artist_credit_name && recording_name) {
            ctx["searched"] = true;
            
            auto start = std::chrono::high_resolution_clock::now();
            
            MappingSearch* mapping_search = get_mapping_search();
            SearchMatch* result = mapping_search->search(
                artist_credit_name,
                release_name ? release_name : "",
                recording_name
            );
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration<double, std::milli>(end - start);
            char time_buf[32];
            snprintf(time_buf, sizeof(time_buf), "%.1f", duration.count());
            ctx["search_time_ms"] = time_buf;
            
            if (result) {
                ctx["has_match"] = true;
                ctx["result_artist_credit_name"] = result->artist_credit_name;
                
                // Build artist MBIDs as list for template iteration
                std::vector<crow::mustache::context> mbid_list;
                for (size_t i = 0; i < result->artist_credit_mbids.size(); i++) {
                    crow::mustache::context mbid_ctx;
                    mbid_ctx["mbid"] = result->artist_credit_mbids[i];
                    if (i == result->artist_credit_mbids.size() - 1) {
                        mbid_ctx["last"] = true;
                    }
                    mbid_list.push_back(mbid_ctx);
                }
                ctx["artist_mbids"] = std::move(mbid_list);
                ctx["artist_credit_id"] = std::move(result->artist_credit_id);
                
                ctx["result_release_name"] = result->release_name;
                ctx["result_release_mbid"] = result->release_mbid;
                ctx["result_recording_name"] = result->recording_name;
                ctx["result_recording_mbid"] = result->recording_mbid;
                
                char conf_buf[32];
                snprintf(conf_buf, sizeof(conf_buf), "%.2f", result->confidence);
                ctx["result_confidence"] = conf_buf;
                
                delete result;
            } else {
                ctx["has_match"] = false;
            }
        }
        
        update_stats(200);
        return crow::response(200, page.render(ctx));
    });

    CROW_ROUTE(app, "/docs")
    ([]() {
        if (!g_ready) {
            auto page = crow::mustache::load("loading.html");
            update_stats(200);
            return crow::response(200, page.render());
        }
        auto page = crow::mustache::load("docs.html");
        update_stats(200);
        return crow::response(200, page.render());
    });

    CROW_ROUTE(app, "/supported")
    ([]() {
        if (!g_ready) {
            auto page = crow::mustache::load("loading.html");
            return crow::response(200, page.render());
        }
        
        auto page_text = crow::mustache::load_text("supported.html");
        if (page_text.empty()) {
            return crow::response(500, "Template \"supported.html\" not found.");
        }
        auto page = crow::mustache::compile(page_text);
        crow::mustache::context ctx;
        
        // URL encode helper
        auto url_encode = [](const std::string& value) -> std::string {
            std::ostringstream escaped;
            escaped.fill('0');
            escaped << std::hex;
            for (char c : value) {
                if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    escaped << c;
                } else {
                    escaped << std::uppercase;
                    escaped << '%' << std::setw(2) << int((unsigned char)c);
                    escaped << std::nouppercase;
                }
            }
            return escaped.str();
        };
        
        std::vector<crow::mustache::context> cases_list;
        for (const auto& tc : get_test_cases()) {
            crow::mustache::context tc_ctx;
            tc_ctx["artist_credit_name"] = tc.artist_credit_name;
            tc_ctx["release_name"] = tc.release_name;
            tc_ctx["recording_name"] = tc.recording_name;
            tc_ctx["artist_credit_name_encoded"] = url_encode(tc.artist_credit_name);
            tc_ctx["release_name_encoded"] = url_encode(tc.release_name);
            tc_ctx["recording_name_encoded"] = url_encode(tc.recording_name);
            cases_list.push_back(tc_ctx);
        }
        ctx["test_cases"] = std::move(cases_list);
        
        update_stats(200);
        return crow::response(200, page.render(ctx));
    });

    CROW_ROUTE(app, "/mapping/lookup")
    ([](const crow::request& req) {
        // Return 503 Service Unavailable if not ready
        if (!g_ready) {
            crow::json::wvalue error;
            error["error"] = "Server is starting up, indexes are still loading";
            return crow::response(503, error);
        }

        auto artist_credit_name = req.url_params.get("artist_credit_name");
        auto release_name = req.url_params.get("release_name");
        auto recording_name = req.url_params.get("recording_name");

        if (!artist_credit_name || !recording_name) {
            crow::json::wvalue error;
            error["error"] = "Missing required parameters: artist_credit_name and recording_name are required";
            update_stats(400);
            return crow::response(400, error);
        }

        MappingSearch* mapping_search = get_mapping_search();
        SearchMatch* result = mapping_search->search(
            artist_credit_name,
            release_name ? release_name : "",
            recording_name
        );

        crow::json::wvalue response;
        if (result) {
            response["artist_credit_id"] = result->artist_credit_id;
            response["artist_credit_name"] = result->artist_credit_name;
            
            crow::json::wvalue::list mbids;
            for (const auto& mbid : result->artist_credit_mbids) {
                mbids.push_back(mbid);
            }
            response["artist_credit_mbids"] = std::move(mbids);
            
            response["release_id"] = result->release_id;
            response["release_name"] = result->release_name;
            response["release_mbid"] = result->release_mbid;
            
            response["recording_id"] = result->recording_id;
            response["recording_name"] = result->recording_name;
            response["recording_mbid"] = result->recording_mbid;
            
            response["confidence"] = result->confidence;
            
            delete result;
            update_stats(200);
            return crow::response(200, response);
        } else {
            response["error"] = "No match found";
            update_stats(404);
            return crow::response(404, response);
        }
    });

    CROW_ROUTE(app, "/metrics")
    ([]() {
        crow::response res(200, g_statistics->get_metrics());
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        return res;
    });

    lb_log("Starting server on %s:%d", host.c_str(), port);
    lb_log("Index directory: %s", g_index_dir.c_str());
    lb_log("Connection timeout: %d seconds", g_timeout);
    
    // Configure Crow for high performance:
    // - timeout: Increase from default 5s to handle complex queries without 502s
    // - loglevel: Reduce logging overhead in production (Warning level)
    // - signal_clear: Don't install default signal handlers (useful for containers)
    app.timeout(static_cast<std::uint8_t>(g_timeout))
       .loglevel(crow::LogLevel::Warning)
       .signal_clear();
    
    if (g_num_threads > 0) {
        lb_log("Using %d threads", g_num_threads);
        app.bindaddr(host).port(port).concurrency(g_num_threads).run();
    } else {
        unsigned int hw_threads = std::thread::hardware_concurrency();
        lb_log("Using all available CPU cores (%u threads)", hw_threads);
        app.bindaddr(host).port(port).multithreaded().run();
    }

    return 0;
}