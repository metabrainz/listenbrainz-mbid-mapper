#ifndef MURMUR_VECTORIZER_H
#define MURMUR_VECTORIZER_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <armadillo>
#include <map>
#include <cmath>
#include <set>
#include <cstdint>

#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>

/**
 * MurmurHash3 32-bit hash function implementation
 */
inline uint32_t murmurhash3_32(const char* key, uint32_t len, uint32_t seed = 42) {
    static const uint32_t c1 = 0xcc9e2d51;
    static const uint32_t c2 = 0x1b873593;
    static const uint32_t r1 = 15;
    static const uint32_t r2 = 13;
    static const uint32_t m = 5;
    static const uint32_t n = 0xe6546b64;

    uint32_t hash = seed;
    const int nblocks = len / 4;
    const uint32_t* blocks = (const uint32_t*)(key);

    for (int i = 0; i < nblocks; i++) {
        uint32_t k = blocks[i];
        k *= c1;
        k = (k << r1) | (k >> (32 - r1));
        k *= c2;

        hash ^= k;
        hash = ((hash << r2) | (hash >> (32 - r2))) * m + n;
    }

    const uint8_t* tail = (const uint8_t*)(key + nblocks * 4);
    uint32_t k1 = 0;

    switch (len & 3) {
        case 3: k1 ^= tail[2] << 16;
        case 2: k1 ^= tail[1] << 8;
        case 1: k1 ^= tail[0];
                k1 *= c1; 
                k1 = (k1 << r1) | (k1 >> (32 - r1)); 
                k1 *= c2; 
                hash ^= k1;
    };

    hash ^= len;
    hash ^= (hash >> 16);
    hash *= 0x85ebca6b;
    hash ^= (hash >> 13);
    hash *= 0xc2b2ae35;
    hash ^= (hash >> 16);

    return hash;
}

class MurmurHashVectorizer
{
    public:
        /**
         * Constructor.
         */
        MurmurHashVectorizer()
        {
            // TODO: Initialize member variables as needed
        }

        /**
         * Convert raw documents to a hashed feature representation.
         * 
         * @param documents: a list of strings. Each string is a document (raw text).
         * 
         * @return matrix with numerical features. 
         *         Each row is a feature. 
         *         Each column is a document.
         */
        arma::sp_mat transform(std::vector<std::string>& documents)
        {
            // Tokenize all documents
            auto documents_tokenised = tokenise_documents(documents);
            
            // Create sparse matrix: rows = features, columns = documents
            arma::sp_mat result(VECTOR_SIZE, documents.size());
            
            // Process each document
            for (size_t doc_idx = 0; doc_idx < documents_tokenised.size(); ++doc_idx) {
                auto sparse_features = vectorize(documents_tokenised[doc_idx]);
                
                // Set values in the sparse matrix
                for (const auto& feature : sparse_features) {
                    result(feature.id, doc_idx) = feature.value;
                }
            }
            
            return result;
        }

        /**
         * Alias for transform (no fitting needed for hash-based vectorizer).
         * 
         * @param documents: a list of strings. Each string is a document (raw text).
         * 
         * @return matrix with numerical features. 
         *         Each row is a feature. 
         *         Each column is a document.
         */
        arma::sp_mat fit_transform(std::vector<std::string>& documents)
        {
            return transform(documents);
        }

    protected:
        struct SparseFeature {
            uint32_t id;
            float value;
        };

        std::vector<SparseFeature> vectorize(const std::vector<std::string>& ngrams)
        {
            std::map<uint32_t, float> counts;
            
            for (const auto& gram : ngrams) {
                uint32_t idx = get_feature_index(gram);
                counts[idx] += 1.0f; // Term Frequency
            }

            std::vector<SparseFeature> vec;
            for (auto const& [id, tf] : counts) {
                // Apply your pre-calculated Global IDF here if needed
                // float weight = tf * global_idf[id]; 
                vec.push_back({id, tf});
            }
            return vec;
        }

        std::vector<std::string> tokenise_document(std::string& document)
        {
            std::vector<std::string> tokens;
           
            auto l = document.length();
            if (l < 3) {
                auto d = document;
                while(d.size() < 3)
                    d += std::string(" ");
                tokens.push_back(d);
            }
            else
                for(size_t i = 0; i < l - 2; i++) {
                    tokens.push_back(document.substr(i, 3));
                }

            return tokens;
        }

        std::vector<std::vector<std::string>> tokenise_documents(std::vector<std::string>& documents)
        {
            std::vector<std::vector<std::string>> documents_tokenised;
            for (size_t i = 0; i < documents.size(); i++)
                documents_tokenised.push_back(tokenise_document(documents[i]));
            return documents_tokenised;
        }

        uint32_t get_feature_index(const std::string& term)
        {
            return murmurhash3_32(term.c_str(), term.length()) % VECTOR_SIZE;
        }

    private:
        // Choose a power of 2 for the vector size to keep bitwise operations fast
        static const uint32_t VECTOR_SIZE = 1 << 18; // 262,144 dimensions
        // TODO: Add member variables as needed
};

#endif
