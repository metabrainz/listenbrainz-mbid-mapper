#pragma once

#include <stdio.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <math.h>
#include <algorithm> // for reverse and sort

using namespace std;

#include "defs.hpp"
#include "tfidf_vectorizer.hpp"
#include "levenshtein.hpp"

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>

#include "index.h"
#include "init.h"
#include "params.h"
#include "rangequery.h"
#include "knnquery.h"
#include "knnqueue.h"
#include "methodfactory.h"
#include "spacefactory.h"
#include "space.h"
#include "space/space_vector.h"
#include "space/space_sparse_vector.h"

const auto NUM_FUZZY_SEARCH_RESULTS = 10;

class FuzzyIndex {
    private:
        similarity::Index<float> *index = nullptr;
        similarity::Space<float> *space = nullptr;
        TfIdfVectorizer           vectorizer;
        similarity::ObjectVector  vectorized_data;
        bool                      use_hnsw; // Branching flag

    public:
        vector<unsigned int>      index_ids; 
        vector<string>            index_texts;

        // Default to false to preserve legacy Artist index behavior
        FuzzyIndex(bool use_hnsw = false, const vector<string> *ngrams = nullptr) :
             vectorizer(false, false), use_hnsw(use_hnsw) {

            string space_type = use_hnsw ? "cosinesimil_sparse" : "negdotprod_sparse_fast";
            space = similarity::SpaceFactoryRegistry<float>::Instance().CreateSpace(
                space_type, similarity::AnyParams());
            
            // Set global weights from ngrams if provided
            if (ngrams != nullptr && !ngrams->empty()) {
                std::unordered_map<std::string, double> global_idf;
                for (const auto& ngram : *ngrams) {
                    global_idf[ngram] = 1.0;  // Weight of 1.0 for all popular ngrams
                }
                vectorizer.set_global_weights(global_idf);
            }
        }
        
        ~FuzzyIndex() {
            for(auto &obj : vectorized_data)
                delete obj;
            delete index;
            delete space;
        }
        
        string get_index_text(unsigned int offset) {
            if (offset >= index_texts.size()) {
                printf("ERROR: get_index_text offset %u out of bounds\n", offset);
                return "";
            }
            return index_texts[offset];
        }

        // Branching Normalization Logic
        void transform_text(const arma::sp_mat &matrix, similarity::ObjectVector &data) {
            auto sparse_space = reinterpret_cast<const similarity::SpaceSparseVector<float>*>(space);
            
            // sp_mat is Column-Major (csc)
            for (arma::uword c = 0; c < matrix.n_cols; ++c) {
                std::vector<similarity::SparseVectElem<float>> sparse_items;
                float sq_sum = 0.0f;

                for (arma::sp_mat::const_col_iterator it = matrix.begin_col(c); it != matrix.end_col(c); ++it) {
                    float val = *it;
                    sparse_items.push_back(similarity::SparseVectElem<float>(it.row(), val));
                    if (use_hnsw) sq_sum += val * val;
                }

                // Path B: L2 Normalization for Cosine Space
                if (use_hnsw && sq_sum > 1e-10f) {
                    float norm = std::sqrt(sq_sum);
                    for (auto &item : sparse_items) {
                        item.val_ /= norm;
                    }
                }

                std::sort(sparse_items.begin(), sparse_items.end());
                data.push_back(sparse_space->CreateObjFromVect(c, -1, sparse_items));
            }
        }

        void build(vector<unsigned int> &_index_ids, vector<string> &text_data) {
            if (text_data.empty()) throw std::length_error("no index data provided.");
            
            index_ids = _index_ids; 
            index_texts = text_data;
            vector<string> short_texts;
            for(auto & it : text_data)
                short_texts.push_back(it.substr(0, MAX_ENCODED_STRING_LENGTH));
           
            arma::sp_mat matrix = vectorizer.fit_transform(short_texts);
            transform_text(matrix, vectorized_data);
            
            if (use_hnsw) {
                // Path B: HNSW Method
                index = similarity::MethodFactoryRegistry<float>::Instance().CreateMethod(
                    false, "hnsw", "cosinesimil_sparse", *space, vectorized_data);
                
                // High accuracy params for small/noisy recording clusters
                similarity::AnyParams index_params({
                    "M=32", 
                    "efConstruction=400", 
                    "post=0"
                });
                index->CreateIndex(index_params);
            } else {
                // Legacy Path: Inverted Index
                index = similarity::MethodFactoryRegistry<float>::Instance().CreateMethod(
                    false, "simple_invindx", "negdotprod_sparse_fast", *space, vectorized_data);
                index->CreateIndex(similarity::AnyParams());
            }
        }

        vector<IndexResult> * search(const string &query_string, float min_confidence, char source) {
            if (index == nullptr) return nullptr;
            
            vector<string> text_data;
            similarity::ObjectVector query_data;
            vector<IndexResult> *results = new vector<IndexResult>;

            text_data.push_back(query_string.substr(0, MAX_ENCODED_STRING_LENGTH));
            arma::sp_mat matrix = vectorizer.transform(text_data);
            transform_text(matrix, query_data);

            unsigned k = NUM_FUZZY_SEARCH_RESULTS;
            bool has_long = false;
            const unsigned max_k = 1000;
            constexpr float PERFECT_MATCH_EPSILON = 1e-6f;

            while (k <= max_k) {
                similarity::KNNQuery<float> knn(*space, query_data[0], k);
                index->Search(&knn, -1);

                bool found_non_perfect = false;
                results->clear();
                has_long = false;
                
                auto queue = knn.Result()->Clone();
                while (!queue->Empty()) {
                    // Cosine dist is [0, 2], negdotprod is < 0. 
                    // This logic maintains confidence compatibility.
                    auto dist = -queue->TopDistance(); 
                    if (dist >= min_confidence) {
                        if (index_texts[queue->TopObject()->id()].size() > MAX_ENCODED_STRING_LENGTH)
                            has_long = true;
                        results->push_back(IndexResult(index_ids[queue->TopObject()->id()], queue->TopObject()->id(), dist, source));
                        
                        if (dist < (1.0f - PERFECT_MATCH_EPSILON)) {
                            found_non_perfect = true;
                        }
                    }
                    queue->Pop();
                }
                delete queue;
                
                if (found_non_perfect || results->empty() || results->size() < k) break;
                k += NUM_FUZZY_SEARCH_RESULTS;
            }

            for(auto &obj : query_data) delete obj;
            
            reverse(results->begin(), results->end());
            if (query_string.size() > MAX_ENCODED_STRING_LENGTH || has_long) {
                auto updated = post_process_long_query(query_string, results, min_confidence, source);
                delete results;
                return updated;
            }
            return results;
        }
         
        // (post_process_long_query remains unchanged as it uses edit distance)
        vector<IndexResult> * post_process_long_query(const string &query, vector<IndexResult> *results, float min_confidence, char source) {
            vector<IndexResult> *updated = new vector<IndexResult>;
            for(int i = results->size() - 1; i >= 0; i--) {
                unsigned int id = (*results)[i].id;
                unsigned int index_offset = (*results)[i].result_index;
                size_t dist = lev_edit_distance(query.size(), (const lev_byte*)query.c_str(), 
                                                index_texts[index_offset].size(), (const lev_byte*)index_texts[index_offset].c_str(), 1);
                float conf = (dist == 0) ? 1.0f : 1.0f - std::abs((float)dist / (float)query.size());

                if (conf >= min_confidence) {
                    updated->push_back({ id, index_offset, conf, source });
                }
            }
            return updated;
        }

        template<class Archive>
        void save(Archive & archive) const {
            vector<uint8_t> index_data;
            if (index) index->SerializeIndex(index_data, vectorized_data);
            // Save the flag so the loader knows which space/method to rebuild
            archive(index_data, vectorizer, index_ids, index_texts, use_hnsw); 
        }
      
        template<class Archive>
        void load(Archive & archive) {
            vector<uint8_t> index_data;
            for (auto datum : vectorized_data) delete datum;
            vectorized_data.clear();

            archive(index_data, vectorizer, index_ids, index_texts, use_hnsw); 
            delete index;
            delete space;
            
            // Re-initialize correct space
            string space_type = use_hnsw ? "cosinesimil_sparse" : "negdotprod_sparse_fast";
            space = similarity::SpaceFactoryRegistry<float>::Instance().CreateSpace(
                space_type, similarity::AnyParams());
            
            if (index_data.empty()) return;
    
            string method_type = use_hnsw ? "hnsw" : "simple_invindx";
            index = similarity::MethodFactoryRegistry<float>::Instance().CreateMethod(
                false, method_type, space_type, *space, vectorized_data);
            
            index->UnserializeIndex(index_data, vectorized_data);
        }
};
