#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <fstream>
#include <string>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

using namespace std;

// Log levels (mirroring Python's logging module)
enum LogLevel {
    LOG_DEBUG = 10,
    LOG_INFO = 20,
    LOG_WARNING = 30,
    LOG_ERROR = 40
};

static LogLevel g_log_level = LOG_INFO;  // Default level

// Call this at the start of main() to ensure logs appear in Docker
// and optionally set the log level
inline void init_logging(LogLevel level = LOG_INFO) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_log_level = level;
    
    // Also check environment variable for log level
    const char* env_level = std::getenv("LOG_LEVEL");
    if (env_level) {
        std::string level_str(env_level);
        if (level_str == "DEBUG") g_log_level = LOG_DEBUG;
        else if (level_str == "INFO") g_log_level = LOG_INFO;
        else if (level_str == "WARNING" || level_str == "WARN") g_log_level = LOG_WARNING;
        else if (level_str == "ERROR") g_log_level = LOG_ERROR;
    }
}

inline void set_log_level(LogLevel level) {
    g_log_level = level;
}

inline void lb_log_message(LogLevel level, const char* level_name, const char *format, va_list args) {
    if (level < g_log_level) return;
    
    char buffer[32];
    time_t current_time = time(nullptr);
    tm *local_time = std::localtime(&current_time);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", local_time);
    printf("%s [%s]: ", buffer, level_name);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
}

inline void lb_debug(const char *format, ...) {
    va_list args;
    va_start(args, format);
    lb_log_message(LOG_DEBUG, "DEBUG", format, args);
    va_end(args);
}

inline void lb_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    lb_log_message(LOG_INFO, "INFO", format, args);
    va_end(args);
}

inline void lb_warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    lb_log_message(LOG_WARNING, "WARN", format, args);
    va_end(args);
}

inline void lb_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    lb_log_message(LOG_ERROR, "ERROR", format, args);
    va_end(args);
}

/**
 * Format a number with comma separators (e.g., 1234567 -> "1,234,567")
 */
inline string format_number(size_t n) {
    string s = to_string(n);
    int insert_pos = s.length() - 3;
    while (insert_pos > 0) {
        s.insert(insert_pos, ",");
        insert_pos -= 3;
    }
    return s;
}

// Load environment variables from a .env file
// Only sets variables that are not already set in the environment
// (environment variables take precedence over .env file)
inline void load_env_file(const char* filename = ".env") {
    std::ifstream file(filename);
    if (!file.is_open()) {
        // Try common locations
        const char* locations[] = {
            ".env",
            "../.env",
            "../../.env",
            nullptr
        };
        
        for (int i = 0; locations[i] != nullptr; i++) {
            file.open(locations[i]);
            if (file.is_open()) break;
        }
        
        if (!file.is_open()) {
            // No .env file found - that's okay, just use environment variables
            return;
        }
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;
        
        // Find the = separator
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        
        // Extract key and value
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        
        // Trim whitespace from key
        size_t start = key.find_first_not_of(" \t");
        size_t end = key.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        key = key.substr(start, end - start + 1);
        
        // Trim whitespace and quotes from value
        start = value.find_first_not_of(" \t");
        if (start != std::string::npos) {
            end = value.find_last_not_of(" \t");
            value = value.substr(start, end - start + 1);
        } else {
            value = "";
        }
        
        // Remove surrounding quotes if present
        if (value.size() >= 2) {
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }
        
        // Only set if not already in environment (env vars take precedence)
        if (std::getenv(key.c_str()) == nullptr) {
            setenv(key.c_str(), value.c_str(), 0);
        }
    }
    
    file.close();
}

// Returns current process RSS in MB
inline size_t get_current_rss_mb() {
    std::ifstream statm("/proc/self/statm");
    if (!statm)
         return 0;

    size_t size, resident;
    statm >> size >> resident;

    // Values in statm are reported in pages. 
    // sysconf(_SC_PAGESIZE) typically returns 4096 (4KB).
    long page_size = sysconf(_SC_PAGESIZE); 
    return (resident * page_size) / (1024 * 1024);
}
        
