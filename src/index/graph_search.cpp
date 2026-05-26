#include "index/graph_search.h"

#include <algorithm>
#include <vector>

#include "core/distance.h"
#include "core/topk.h"
#include "io/prefetch_scheduler.h"
#include "pq/pq_codec.h"

namespace vindex {

struct Candidate {
  uint32_t id = 0;
  Distance dist = 0.0f;
};

std::vector<SearchResult> GraphSearcher::Search(const float* query, uint32_t dim,
                                                const SearchParams& params,
                                                SearchStats* stats) const {
  if (segment_.count() == 0 || segment_.dim() != dim) {
    return {};
  }

  bool use_pq = segment_.HasPQ();
  std::vector<float> adc_table;

  if (use_pq) {
    PQCodec::BuildADCTable(query, dim, segment_.codebook(), adc_table);
  }

  std::vector<uint8_t> visited(segment_.count(), 0);
  std::vector<Candidate> frontier;

  // Map from node id to prefetch request id.
  std::vector<uint64_t> prefetch_requests(segment_.count(), 0);

  std::vector<float> entry_vec;
  std::vector<uint32_t> entry_neighbors;
  if (!segment_.ReadNode(segment_.entry(), entry_vec, entry_neighbors)) {
    return {};
  }

  Distance entry_dist = L2Squared(query, entry_vec.data(), dim);
  frontier.push_back({segment_.entry(), entry_dist});

  TopK topk(params.top_k);

  uint32_t visited_count = 0;
  uint32_t pq_dist_total = 0;
  uint32_t exact_read_total = 0;
  uint32_t prefetch_hits = 0;

  while (!frontier.empty() && visited_count < params.max_visits) {
    std::sort(frontier.begin(), frontier.end(),
              [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });

    Candidate current = frontier.front();
    frontier.erase(frontier.begin());

    if (current.id >= segment_.count()) {
      continue;
    }
    if (visited[current.id]) {
      continue;
    }
    visited[current.id] = 1;
    ++visited_count;
    ++exact_read_total;

    std::vector<float> cur_vec;
    std::vector<uint32_t> cur_neighbors;

    // Try prefetch first.
    bool got_from_prefetch = false;
    if (prefetch_ && prefetch_requests[current.id] != 0) {
      std::vector<uint8_t> prefetch_buf;
      if (prefetch_->TryGet(prefetch_requests[current.id], prefetch_buf)) {
        SegmentReader::ParseNode(prefetch_buf.data(), segment_.dim(),
                                 segment_.degree(), cur_vec, cur_neighbors);
        got_from_prefetch = true;
        ++prefetch_hits;
      }
    }

    if (!got_from_prefetch) {
      if (!segment_.ReadNode(current.id, cur_vec, cur_neighbors)) {
        continue;
      }
    }

    Distance exact_dist = L2Squared(query, cur_vec.data(), dim);
    topk.Add({static_cast<VectorId>(segment_.id_offset() + current.id), exact_dist});

    for (uint32_t neighbor : cur_neighbors) {
      if (neighbor >= segment_.count()) {
        continue;
      }
      if (visited[neighbor]) {
        continue;
      }

      Distance dist;
      if (use_pq) {
        dist = segment_.PQDistance(adc_table.data(), neighbor);
        ++pq_dist_total;
      } else {
        std::vector<float> neighbor_vec;
        std::vector<uint32_t> neighbor_neighbors;

        // Try prefetch for neighbor.
        bool neighbor_from_prefetch = false;
        if (prefetch_ && prefetch_requests[neighbor] != 0) {
          std::vector<uint8_t> prefetch_buf;
          if (prefetch_->TryGet(prefetch_requests[neighbor], prefetch_buf)) {
            SegmentReader::ParseNode(prefetch_buf.data(), segment_.dim(),
                                     segment_.degree(), neighbor_vec,
                                     neighbor_neighbors);
            neighbor_from_prefetch = true;
            ++prefetch_hits;
          }
        }

        if (!neighbor_from_prefetch) {
          if (!segment_.ReadNode(neighbor, neighbor_vec, neighbor_neighbors)) {
            continue;
          }
        }
        dist = L2Squared(query, neighbor_vec.data(), dim);
      }

      frontier.push_back({neighbor, dist});
    }

    if (params.beam_width > 0 && frontier.size() > params.beam_width) {
      auto mid = frontier.begin() + static_cast<std::ptrdiff_t>(params.beam_width);
      std::nth_element(frontier.begin(), mid, frontier.end(),
                       [](const Candidate& a, const Candidate& b) {
                         return a.dist < b.dist;
                       });
      frontier.resize(params.beam_width);
    }

    // Submit prefetch for top unvisited frontier nodes.
    if (prefetch_) {
      uint32_t prefetch_count = 0;
      for (const auto& c : frontier) {
        if (prefetch_count >= params.beam_width) break;
        if (prefetch_requests[c.id] != 0) continue;  // Already requested.
        uint32_t lid = c.id;
        uint64_t offset = segment_.NodeOffset(lid);
        uint32_t size = segment_.NodeSize();
        uint64_t req_id = prefetch_->SubmitPrefetch(
            segment_.path(), offset, size);
        if (req_id != 0) {
          prefetch_requests[lid] = req_id;
          ++prefetch_count;
        }
      }
    }
  }

  if (stats) {
    stats->visited = visited_count;
    stats->pq_distances = pq_dist_total;
    stats->exact_reads = exact_read_total;
    stats->prefetch_hits = prefetch_hits;
  }

  return topk.results();
}

}  // namespace vindex
