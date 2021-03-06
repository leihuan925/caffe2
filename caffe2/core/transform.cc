#include "caffe2/core/transform.h"

#include "caffe2/core/common.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/net.h"
#include "caffe2/proto/caffe2.pb.h"

namespace caffe2 {

using transform::Graph;

CAFFE_DEFINE_REGISTRY(TransformRegistry, Transform);

std::vector<std::vector<int>> Transform::PatternMatch(const Graph& graph) {
  // checks if the node at index i is matched already or not
  std::vector<bool> matched(graph.size(), false);

  // stores matches, which are ordered subgraphs of G
  std::vector<std::vector<int>> matches;

  // Consider every possible node as the starting point.
  for (int idx = 0; idx < graph.size(); ++idx) {
    // The current working subgraph. We will try to add new nodes to this,
    // when invoking the PatternRule.
    std::vector<int> subgraph;

    // The largest "validated" subgraph found so far.
    // This will be mutated by PatternMatchHelper.
    std::vector<int> best_subgraph;

    // Only begin to match if the start node is accepted.
    if (!matched.at(idx) && PatternRule(graph, subgraph, idx)) {
      subgraph.push_back(idx);
      PatternMatchHelper(graph, matched, &subgraph, &best_subgraph);
      subgraph.pop_back();
    }
    if (best_subgraph.size() > 0) { // match found
      matches.push_back(best_subgraph);
      for (const auto& x : best_subgraph) {
        matched[x] = true;
      }
    }
  }
  return matches;
}

void Transform::TryNeighbors(
    const Graph& graph,
    const std::map<int, std::vector<string>>& neighbors,
    const std::vector<bool>& matched,
    std::vector<int>* subgraph_ptr,
    std::vector<int>* best_subgraph_ptr) {
  auto& subgraph = *subgraph_ptr;
  for (const auto& edge : neighbors) {
    int j = edge.first;
    if (std::find(subgraph.begin(), subgraph.end(), j) == subgraph.end()) {
      if (!matched.at(j) && PatternRule(graph, subgraph, j)) {
        subgraph.push_back(j);
        PatternMatchHelper(graph, matched, subgraph_ptr, best_subgraph_ptr);
        subgraph.pop_back();
      }
    }
  }
}

void Transform::PatternMatchHelper(
    const Graph& graph,
    const std::vector<bool>& matched,
    std::vector<int>* subgraph_ptr,
    std::vector<int>* best_subgraph_ptr) {
  CHECK(subgraph_ptr);
  auto& subgraph = *subgraph_ptr;
  CHECK(best_subgraph_ptr);
  auto& best_subgraph = *best_subgraph_ptr;

  // If the current subgraph is valid, and the largest we've seen so far,
  // make it the best_subgraph.
  if (ValidatorRule(graph, subgraph) &&
      subgraph.size() > best_subgraph.size()) {
    best_subgraph = subgraph;
  }

  int size_before = subgraph.size();

  if (pattern_match_type_ == CONNECTED_SUBGRAPH) {
    // Connected Component Order Pattern Matching
    // We want to match subgraphs which are connected ConnectedComponents

    // Try adding each parent and child of every node in the subgraph,
    // and see if we can accept it.
    for (int i = 0; i < subgraph.size(); i++) {
      int x = subgraph[i];
      TryNeighbors(
          graph,
          graph.node(x).children,
          matched,
          subgraph_ptr,
          best_subgraph_ptr);
      CAFFE_ENFORCE(
          size_before == subgraph.size(),
          "Subgraph size should not change after returning from recursive call.");
      TryNeighbors(
          graph,
          graph.node(x).parents,
          matched,
          subgraph_ptr,
          best_subgraph_ptr);
      CAFFE_ENFORCE(
          size_before == subgraph.size(),
          "Subgraph size should not change after returning from recursive call.");
    }
  } else if (pattern_match_type_ == SORTED_WRT_EXECUTION_ORDER) {
    // Sorted Execution Order Pattern matching
    // We want to be able to match subgraphs in sorted execution order

    // We can safely assume our subgraph is already sorted.
    // This means, we only need to consider nodes that come after the LAST
    // node in our current subgraph.
    // Thus, we simply iterate over the nodes that come AFTER the last node of
    // our current subgraph.
    int start_idx = 0;
    if (subgraph.size() > 0) {
      start_idx = subgraph.back() + 1;
    }
    for (int i = start_idx; i < graph.size(); i++) {
      if (!matched.at(i) && PatternRule(graph, subgraph, i)) {
        subgraph.push_back(i);
        PatternMatchHelper(graph, matched, subgraph_ptr, best_subgraph_ptr);
        subgraph.pop_back();
      }
    }
  } else if (pattern_match_type_ == GENERAL) {
    // General Pattern matching
    // We want to be able to match any ordered subgraph

    // For every current subgraph, we consider all nodes to be
    // the next candidate node, as long as it isn't already matched.
    for (int i = 0; i < graph.size(); i++) {
      if (std::find(subgraph.begin(), subgraph.end(), i) == subgraph.end()) {
        // Then we try appending it to the subgraph.
        if (!matched.at(i) && PatternRule(graph, subgraph, i)) {
          subgraph.push_back(i);
          PatternMatchHelper(graph, matched, subgraph_ptr, best_subgraph_ptr);
          subgraph.pop_back();
        }
      }
    }
  } else {
    CAFFE_NOT_IMPLEMENTED;
  }
}

void Transform::ReplacePattern(
    const std::vector<vector<int>>& matches,
    Graph* graph) {
  for (const auto& match : matches) {
    // Make sure each matched node is still active (not overwritten)
    bool is_match_active = true;
    for (int idx : match) {
      if (!graph->is_node_active(idx)) {
        is_match_active = false;
      }
    }

    // Simply try to apply the replace rule upon every match.
    if (is_match_active && !ReplaceRule(match, graph)) {
      CAFFE_THROW("Replace failed!");
    }
  }
}

// The simple interface - performs the transformation upon a NetDef, and returns
// the result.
NetDef Transform::ApplyTo(const NetDef& orig_net) {
  Graph g(orig_net);
  const auto matches = PatternMatch(g);
  ReplacePattern(matches, &g);
  return g.GetNetDef();
}

unique_ptr<Transform> CreateTransform(string key) {
  return TransformRegistry()->Create(key);
}

} // namespace Caffe2
