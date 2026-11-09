// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// String similarity utility for suggesting fixes in parsing errors
// Uses Levenshtein distance algorithm for edit distance calculation

#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace lightusd {

/// String similarity utilities for suggesting fixes
namespace string_similarity {

/// Calculate Levenshtein distance (edit distance) between two strings
/// Lower values indicate more similar strings
/// @param s1 First string
/// @param s2 Second string
/// @return Edit distance (number of single-character edits needed)
inline int LevenshteinDistance(const std::string& s1, const std::string& s2) {
  const size_t m = s1.length();
  const size_t n = s2.length();

  // Create distance matrix
  std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));

  // Initialize first row and column
  for (size_t i = 0; i <= m; ++i) {
    dp[i][0] = static_cast<int>(i);
  }
  for (size_t j = 0; j <= n; ++j) {
    dp[0][j] = static_cast<int>(j);
  }

  // Fill distance matrix
  for (size_t i = 1; i <= m; ++i) {
    for (size_t j = 1; j <= n; ++j) {
      if (s1[i - 1] == s2[j - 1]) {
        dp[i][j] = dp[i - 1][j - 1];
      } else {
        dp[i][j] = 1 + std::min({dp[i - 1][j],      // deletion
                                 dp[i][j - 1],      // insertion
                                 dp[i - 1][j - 1]   // substitution
        });
      }
    }
  }

  return dp[m][n];
}

/// Calculate similarity score (0.0 to 1.0) between two strings
/// 1.0 means identical, 0.0 means completely different
/// @param s1 First string
/// @param s2 Second string
/// @return Similarity score (0.0 to 1.0)
inline double SimilarityScore(const std::string& s1, const std::string& s2) {
  if (s1.empty() && s2.empty()) {
    return 1.0;
  }
  if (s1.empty() || s2.empty()) {
    return 0.0;
  }

  int distance = LevenshteinDistance(s1, s2);
  int max_length = static_cast<int>(std::max(s1.length(), s2.length()));

  return 1.0 - (static_cast<double>(distance) / static_cast<double>(max_length));
}

/// Find the closest match from a list of candidates
/// @param input The input string to match
/// @param candidates List of candidate strings to search
/// @param min_similarity Minimum similarity threshold (0.0 to 1.0)
/// @return Closest matching candidate string, or empty string if no match found
inline std::string FindClosestMatch(
    const std::string& input,
    const std::vector<std::string>& candidates,
    double min_similarity = 0.6) {
  if (candidates.empty() || input.empty()) {
    return std::string();
  }

  double best_similarity = min_similarity;
  std::string best_match;

  for (const auto& candidate : candidates) {
    double similarity = SimilarityScore(input, candidate);
    if (similarity > best_similarity) {
      best_similarity = similarity;
      best_match = candidate;
    }
  }

  return best_match;
}

/// Find multiple close matches from a list of candidates (top N)
/// @param input The input string to match
/// @param candidates List of candidate strings to search
/// @param top_n Number of top matches to return
/// @param min_similarity Minimum similarity threshold (0.0 to 1.0)
/// @return Vector of top N matching candidates sorted by similarity
inline std::vector<std::string> FindTopMatches(
    const std::string& input,
    const std::vector<std::string>& candidates,
    size_t top_n = 3,
    double min_similarity = 0.5) {
  if (candidates.empty() || input.empty()) {
    return std::vector<std::string>();
  }

  // Calculate similarity for all candidates
  std::vector<std::pair<double, std::string>> scored_candidates;
  for (const auto& candidate : candidates) {
    double similarity = SimilarityScore(input, candidate);
    if (similarity >= min_similarity) {
      scored_candidates.emplace_back(similarity, candidate);
    }
  }

  // Sort by similarity (descending)
  std::sort(scored_candidates.begin(), scored_candidates.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  // Extract top N matches
  std::vector<std::string> results;
  for (size_t i = 0; i < std::min(top_n, scored_candidates.size()); ++i) {
    results.push_back(scored_candidates[i].second);
  }

  return results;
}

}  // namespace string_similarity

}  // namespace lightusd
