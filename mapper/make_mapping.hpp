#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <iostream>
#include <sstream>
#include <filesystem>

#include "SQLiteCpp.h"
#include "utils.hpp"
#include "defs.hpp"
#include "tfidf_vectorizer.hpp"

// Forward declaration for libpq types
struct pg_conn;
typedef struct pg_conn PGconn;

using namespace std;

class MakeMapping {
private:
    string index_dir;

public:
    MakeMapping(const string& _index_dir);
    void create();
};