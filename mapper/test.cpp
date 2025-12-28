#include <stdio.h>
#include <tuple>
#include <sstream>
#include <algorithm>
#include <iterator>
#include "fsm.hpp"
#include "test_cases.hpp"
#include "init.h"  // nmslib init

#ifdef INFO
#undef INFO
#endif

#ifdef CHECK
#undef CHECK
#endif

#ifdef WARN
#undef WARN
#endif

#include <catch2/catch_all.hpp>

MappingSearch *mapping_search;

string
make_comma_sep_string(const vector<string> &str_array) {
    string ret; 
    int index = 0;
    for(auto &it : str_array) {
        if (index > 0)
            ret += string(",");
                
        ret += it;
        index += 1;
    }
    return ret;
}

string join(const vector<string>& strings, const string& delimiter) {
    if (strings.empty()) {
        return "";
    }
    ostringstream oss;
    copy(strings.begin(), strings.end() - 1,
              ostream_iterator<string>(oss, delimiter.c_str()));
    oss << strings.back();
    return oss.str();
}

tuple<string, string, string>
lookup(const string &artist_credit_name, const string &release_name, const string &recording_name) {
    SearchMatch *result = mapping_search->search(artist_credit_name, release_name, recording_name);
    if (!result) {
        tuple<string, string, string> ret = { string(), string(), string() };
        lb_log("no matches");
        return ret; 
    }
    lb_log("%-8d %s %s", 
        result->artist_credit_id,
        join(result->artist_credit_mbids, string(",")).c_str(),
        result->artist_credit_name.c_str());
    lb_log("%-8d %s %s", 
        result->release_id,
        result->release_mbid.c_str(),
        result->release_name.c_str());
    lb_log("%-8d %s %s\n", 
        result->recording_id,
        result->recording_mbid.c_str(),
        result->recording_name.c_str());
   
    string artist_mbids;
    int index = 0;
    for(auto &it : result->artist_credit_mbids) {
        if (index > 0)
            artist_mbids += string(",");
                
        artist_mbids += it;
        index += 1;
    }

    tuple<string, string, string> ret = { artist_mbids, result->release_mbid, result->recording_mbid };
    delete result;
    return ret;
}

TEST_CASE("Basic tests") {
    auto test_case = GENERATE(from_range(get_basic_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Messy inputs") {
    auto test_case = GENERATE(from_range(get_messy_input_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Select the correct release") {
    auto test_case = GENERATE(from_range(get_release_selection_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("No release given") {
    auto test_case = GENERATE(from_range(get_no_release_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Obscure releases") {
    auto test_case = GENERATE(from_range(get_obscure_release_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Obscure recordings") {
    auto test_case = GENERATE(from_range(get_obscure_recording_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Special characters") {
    auto test_case = GENERATE(from_range(get_special_character_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Artist credits / artist aliases") {
    auto test_case = GENERATE(from_range(get_artist_credit_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Non album recordings") {
    auto test_case = GENERATE(from_range(get_non_album_recording_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Avoid most popular matches") {
    auto test_case = GENERATE(from_range(get_avoid_popular_match_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Avoid punctuation") {
    auto test_case = GENERATE(from_range(get_punctuation_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Second best release match") {
    auto test_case = GENERATE(from_range(get_second_best_release_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Fuzzy release search") {
    auto test_case = GENERATE(from_range(get_fuzzy_release_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Recording aliases") {
    auto test_case = GENERATE(from_range(get_recording_alias_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

TEST_CASE("Heavily overloaded artist names") {
    auto test_case = GENERATE(from_range(get_overloaded_artist_tests()));

    INFO("Artist: " << test_case.artist_credit_name);
    INFO("Release: " << test_case.release_name);
    INFO("Recording: " << test_case.recording_name);

    tuple<string, string, string> result = lookup(test_case.artist_credit_name,
                                                 test_case.release_name,
                                                 test_case.recording_name);

    REQUIRE(get<0>(result) == test_case.artist_credit_mbids);
    REQUIRE(get<1>(result) == test_case.release_mbid);
    REQUIRE(get<2>(result) == test_case.recording_mbid);
}

int main(int argc, char* argv[]) {
    init_logging(LOG_DEBUG);
    load_env_file();  // Load .env file, env vars take precedence
    
    // Initialize nmslib once in main thread before any FuzzyIndex is created
    similarity::initLibrary(0, LIB_LOGNONE, NULL);
    
    const char* env_index_dir = std::getenv("INDEX_DIR");
    if (!env_index_dir || strlen(env_index_dir) == 0) {
        lb_error("Error: INDEX_DIR environment variable not set");
        return -1;
    }
    string index_dir = env_index_dir;
    
    ArtistIndex* artist_index = new ArtistIndex(index_dir);
    if (!artist_index->load()) {
        lb_error("Failed to load artist index. INDEX_DIR=%s", index_dir.c_str());
        delete artist_index;
        return -1;
    }
    IndexCache* index_cache = new IndexCache();
    
    mapping_search = new MappingSearch(index_dir, artist_index, index_cache);

    Catch::Session session;
    int returnCode = session.run(argc, argv);
    
    delete mapping_search;
    delete artist_index;
    delete index_cache;

    return returnCode;
}
