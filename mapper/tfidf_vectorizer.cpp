#include <stdio.h>
#include "tfidf_vectorizer.hpp"

TfIdfVectorizer::TfIdfVectorizer(bool binary, bool lowercase, bool use_idf, int max_features, std::string norm, bool sublinear_tf)
{
    this->binary = binary;
    this->max_features = max_features; 
    if (norm == "l2") this->p = 2;
    else if (norm == "l1") this->p = 1;
    else this->p = 0;
    this->lowercase = lowercase;
    this->use_idf = use_idf;
    this->sublinear_tf = sublinear_tf;
}

// --- NEW METHOD IMPLEMENTATION ---
void TfIdfVectorizer::set_global_weights(const std::unordered_map<std::string, double>& global_idf)
{
    this->idf_.clear();
    this->vocabulary_.clear();

    size_t index = 0;
    for (auto const& [gram, weight] : global_idf) {
        this->idf_[gram] = weight;
        this->vocabulary_[gram] = index++;
    }
}

std::vector<std::string> TfIdfVectorizer::tokenise_document(std::string& document)
{
    std::vector<std::string> tokens;
    auto l = document.length();
    
    // Maintain your existing 3-gram logic
    if (l < 3) {
        auto d = document;
        while(d.size() < 3) d += std::string(" ");
        tokens.push_back(d);
    }
    else {
        for(int i = 0; i < l - 2; i++) {
            tokens.push_back(document.substr(i, 3));
        }
    }
    return tokens;
}

std::vector<std::vector<std::string>> TfIdfVectorizer::tokenise_documents(std::vector<std::string>& documents)
{
    std::vector<std::vector<std::string>> documents_tokenised;
    for (size_t i = 0; i < documents.size(); i++)
        documents_tokenised.push_back(tokenise_document(documents[i]));
    return documents_tokenised;
}

std::vector<std::map<std::string, int>> TfIdfVectorizer::word_count(std::vector<std::vector<std::string>>& documents_tokenised)
{
    std::vector<std::map<std::string, int>> documents_word_counts;
    std::string word;
    std::set<std::string> words_set;
    for (size_t d = 0; d < documents_tokenised.size(); d++)
    {
        std::map<std::string, int> wc;
        documents_word_counts.push_back(wc);
        for (size_t w = 0; w < documents_tokenised[d].size(); w++)
        {
            word = documents_tokenised[d][w];
            documents_word_counts[d][word] += 1;
            words_set.insert(word);
        }
    }

    size_t i = 0;
    for (auto it = words_set.begin(); it != words_set.end(); ++it)
    {
        word = *it;
        this->vocabulary_[word] = i;
        i++;
    }
    return documents_word_counts;
}

void TfIdfVectorizer::fit(std::vector<std::string>& documents)
{
    this->vocabulary_.clear();
    this->idf_.clear();
    std::vector<std::vector<std::string>> documents_tokenised = tokenise_documents(documents);
    std::vector<std::map<std::string, int>> documents_word_counts = word_count(documents_tokenised);
    idf(documents_word_counts);
}

std::map<std::string, double> TfIdfVectorizer::idf(std::vector<std::map<std::string, int>>& documents_word_counts)
{
    size_t documents = documents_word_counts.size();
    double d_documents = (double)documents;
    std::unordered_map<std::string, int> doc_freq;
    
    for(const auto& doc : documents_word_counts) {
        for(auto const& [key, count] : doc) {
            doc_freq[key]++;
        }
    }
    
    for (auto const& [key, vocab_idx] : this->vocabulary_)
    {
        int value = doc_freq[key];
        // Standard formula used in legacy implementation
        this->idf_[key] = std::log((d_documents + 1) / (value + 1)) + 1;
    }

    if (this->max_features > 0 && (int)this->idf_.size() > this->max_features)
    {
        // ... (Max features pruning logic as per your original file) ...
        // Note: Generally not recommended when using Global Weights.
    }
    return this->idf_;
} 

std::vector<std::map<std::string, double>> TfIdfVectorizer::tf(std::vector<std::vector<std::string>>& documents_tokenised)
{
    std::vector<std::map<std::string, double>> documents_word_frequency;
    for (size_t d = 0; d < documents_tokenised.size(); d++)
    {
        std::map<std::string, double> wf;
        for (size_t w = 0; w < documents_tokenised[d].size(); w++)
        {
            wf[documents_tokenised[d][w]] += 1;
        }
        for (auto& s : wf)
        {
            if(this->binary) s.second = 1.0;
            else {
                s.second /= documents_tokenised[d].size();
                if(this->sublinear_tf) s.second = 1.0 + std::log(s.second);
            }
        }
        documents_word_frequency.push_back(wf);
    }
    return documents_word_frequency;
}

arma::sp_mat TfIdfVectorizer::fit_transform(std::vector<std::string>& documents)
{
    fit(documents);
    return transform(documents);
}

arma::sp_mat TfIdfVectorizer::transform(std::vector<std::string>& documents)
{
    if (this->vocabulary_.empty()) return arma::sp_mat();

    std::vector<std::vector<std::string>> documents_tokenised = tokenise_documents(documents);
    std::vector<std::map<std::string, double>> documents_tf = tf(documents_tokenised);
    
    arma::sp_mat X_transformed(this->vocabulary_.size(), documents.size());

    for (size_t d = 0; d < documents.size(); d++)
    {
        for (auto const& [word, tf_val] : documents_tf[d])
        {
            auto vocab_it = this->vocabulary_.find(word);
            if (vocab_it == this->vocabulary_.end()) continue;
            
            size_t row_idx = vocab_it->second;
            
            auto idf_it = this->idf_.find(word);
            if (idf_it == this->idf_.end()) continue;

            if (this->use_idf) {
                X_transformed(row_idx, d) = tf_val * idf_it->second;
            } else {
                X_transformed(row_idx, d) = (tf_val > 0) ? 1.0 : 0.0;
            }
        }
    }

    if (this->p != 0)
    {
        for (size_t i = 0; i < X_transformed.n_cols; ++i) {
            double norm_val = arma::norm(X_transformed.col(i), this->p);
            if (norm_val > 1e-10) {
                X_transformed.col(i) /= norm_val;
            }
        }
    }
    return X_transformed;
}

std::map<std::string, double> TfIdfVectorizer::get_idf_() { return this->idf_; }
std::map<std::string, size_t> TfIdfVectorizer::get_vocabulary_() { return this->vocabulary_; }

