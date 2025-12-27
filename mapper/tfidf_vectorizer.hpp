#ifndef TFIDF_VECTORISER_H
#define TFIDF_VECTORISER_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <armadillo>
#include <map>
#include <unordered_map> // Added
#include <cmath>
#include <set>

#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>

class TfIdfVectorizer
{
    public:
        TfIdfVectorizer(bool binary=false, bool lowercase=true, bool use_idf=true, int max_features=-1, std::string norm="l2", bool sublinear_tf=false);

        void fit(std::vector<std::string>& documents);
        arma::sp_mat transform(std::vector<std::string>& documents);
        arma::sp_mat fit_transform(std::vector<std::string>& documents);

        // --- NEW METHOD ---
        /**
         * Overwrites internal IDF and Vocabulary with global data.
         * Call this once after instantiation to use the 'Anchor' approach.
         */
        void set_global_weights(const std::unordered_map<std::string, double>& global_idf);

        std::map<std::string, double> get_idf_();
        std::map<std::string, size_t> get_vocabulary_();
        std::vector<std::string> tokenise_document(std::string& document);
        
        template<class Archive>
        void serialize(Archive & archive)
        {
            archive( idf_, vocabulary_, binary, max_features, p, lowercase, use_idf, sublinear_tf );
        }
        
    protected:
        std::vector<std::vector<std::string>> tokenise_documents(std::vector<std::string>& documents);
        std::vector<std::map<std::string, int>> word_count(std::vector<std::vector<std::string>>& documents_tokenised);
        std::vector<std::map<std::string, double>> tf(std::vector<std::vector<std::string>>& documents_tokenised);
        std::map<std::string, double> idf(std::vector<std::map<std::string, int>>& documents_word_counts);

    private:
        std::map<std::string, double> idf_;
        std::map<std::string, size_t> vocabulary_;
        bool binary;
        int max_features;
        double p;
        bool lowercase;
        bool use_idf;
        bool sublinear_tf;
};

#endif
