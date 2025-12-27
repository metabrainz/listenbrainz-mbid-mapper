#pragma once

#include <stdio.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace std;

#include "defs.hpp"
#include "tfidf_vectorizer.hpp"
#include "levenshtein.hpp"
#include "popular_ngram.hpp"

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

    public:
        vector<unsigned int>      index_ids; 
        vector<string>            index_texts;

        /**
         * Constructor
         * @param ngrams: Optional pointer to PopularNgram data to seed Global IDF weights.
         */
        FuzzyIndex(const PopularNgram *ngrams = nullptr) :
             vectorizer(false, false) {

            space = similarity::SpaceFactoryRegistry<float>::Instance().CreateSpace(
                "negdotprod_sparse_fast", similarity::AnyParams());
            
            // Set global weights from ngrams if provided
            if (ngrams != nullptr && !ngrams->ngrams.empty()) {
                std::unordered_map<std::string, double> global_idf;
                for (const auto& ngram : ngrams->ngrams) {
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
            if (offset >= index_texts.size()) return "";
            return index_texts[offset];
        }

        /**
         * Converts the Armadillo sparse matrix into NMSLIB ObjectVector format.
         */
        void transform_text(const arma::sp_mat &matrix, similarity::ObjectVector &data) {
            auto sparse_space = reinterpret_cast<const similarity::SpaceSparseVector<float>*>(space);
            
            for (arma::uword c = 0; c < matrix.n_cols; ++c) {
                std::vector<similarity::SparseVectElem<float>> sparse_items;

                for (arma::sp_mat::const_col_iterator it = matrix.begin_col(c); it != matrix.end_col(c); ++it) {
                    float val = static_cast<float>(*it);
                    sparse_items.push_back(similarity::SparseVectElem<float>(it.row(), val));
                }

                std::sort(sparse_items.begin(), sparse_items.end());
                data.push_back(sparse_space->CreateObjFromVect(c, -1, sparse_items));
            }
        }

        /**
         * Builds the index.
         */
        void build(vector<unsigned int> &_index_ids, vector<string> &text_data) {
            if (text_data.empty()) throw std::length_error("no index data provided.");
            
            index_ids = _index_ids; 
            index_texts = text_data;
            vector<string> short_texts;
            for(auto & it : text_data)
                short_texts.push_back(it.substr(0, MAX_ENCODED_STRING_LENGTH));
           
            // Ensure we don't overwrite global weights if they were provided in constructor
            arma::sp_mat matrix;
            if (vectorizer.get_vocabulary_().empty()) {
                matrix = vectorizer.fit_transform(short_texts);
            } else {
                matrix = vectorizer.transform(short_texts);
            }
            transform_text(matrix, vectorized_data);
            
            // Use the fast inverted index with global weights
            index = similarity::MethodFactoryRegistry<float>::Instance().CreateMethod(
                false, 
                "simple_invindx",
                "negdotprod_sparse_fast",
                *space, 
                vectorized_data
            );
            index->CreateIndex(similarity::AnyParams());
        }

        /**
         * Search function that calculates confidence and handles long string post-processing.
         */
        vector<IndexResult> * search(const string &query_string, float min_confidence, char source) {
            if (index == nullptr) return nullptr;
            
            vector<string> text_data = { query_string.substr(0, MAX_ENCODED_STRING_LENGTH) };
            similarity::ObjectVector query_data;
            
            // Vectorize query using existing weights
            arma::sp_mat matrix = vectorizer.transform(text_data);
            transform_text(matrix, query_data);

            unsigned k = NUM_FUZZY_SEARCH_RESULTS;
            const unsigned max_k = 1000;
            vector<IndexResult> *results = new vector<IndexResult>;

            while (k <= max_k) {
                similarity::KNNQuery<float> knn(*space, query_data[0], k);
                index->Search(&knn, -1);
                
                results->clear();
                auto queue = knn.Result()->Clone();
                while (!queue->Empty()) {
                    // Negate distance (negative dot product) to get positive confidence score
                    float dist = -queue->TopDistance();
                    
                    if (dist >= min_confidence) {
                        results->push_back(IndexResult(index_ids[queue->TopObject()->id()], queue->TopObject()->id(), dist, source));
                    }
                    queue->Pop();
                }
                delete queue;
                
                if (results->size() < k) break;
                k += NUM_FUZZY_SEARCH_RESULTS;
            }

            for(auto &obj : query_data) delete obj;
            
            // NMSLIB returns min-distance first; we want highest confidence first
            reverse(results->begin(), results->end());

            // Check if post-processing is needed for strings exceeding n-gram limit
            bool needs_post = query_string.size() > MAX_ENCODED_STRING_LENGTH;
            if(!needs_post) {
                for(auto &r : *results) {
                    if(index_texts[r.result_index].size() > MAX_ENCODED_STRING_LENGTH) {
                        needs_post = true; 
                        break;
                    }
                }
            }

            if (needs_post) {
                auto updated = post_process_long_query(query_string, results, min_confidence, source);
                delete results;
                return updated;
            }
            return results;
        }

        vector<IndexResult> * post_process_long_query(const string &query, vector<IndexResult> *results, float min_confidence, char source) {
            vector<IndexResult> *updated = new vector<IndexResult>;
            for(int i = results->size() - 1; i >= 0; i--) {
                unsigned int id = (*results)[i].id;
                unsigned int offset = (*results)[i].result_index;
                
                size_t dist = lev_edit_distance(query.size(), (const lev_byte*)query.c_str(), 
                                                index_texts[offset].size(), (const lev_byte*)index_texts[offset].c_str(), 1);
                
                float conf = (dist == 0) ? 1.0f : 1.0f - ((float)dist / (float)max(query.size(), index_texts[offset].size()));

                if (conf >= min_confidence) {
                    updated->push_back({ id, offset, conf, source });
                }
            }
            return updated;
        }

        template<class Archive>
        void save(Archive & archive) const {
            vector<uint8_t> index_data;
            if (index) index->SerializeIndex(index_data, vectorized_data);
            // vectorizer is serialized here, preserving the global weights
            archive(index_data, vectorizer, index_ids, index_texts); 
        }
      
        template<class Archive>
        void load(Archive & archive) {
            vector<uint8_t> index_data;
            for (auto datum : vectorized_data) delete datum;
            vectorized_data.clear();

            archive(index_data, vectorizer, index_ids, index_texts); 
            delete index;
            delete space;
            
            space = similarity::SpaceFactoryRegistry<float>::Instance().CreateSpace(
                "negdotprod_sparse_fast", similarity::AnyParams());
            
            if (index_data.empty()) return;
    
            index = similarity::MethodFactoryRegistry<float>::Instance().CreateMethod(
                false, "simple_invindx", "negdotprod_sparse_fast", *space, vectorized_data);
            
            index->UnserializeIndex(index_data, vectorized_data);
        }
};