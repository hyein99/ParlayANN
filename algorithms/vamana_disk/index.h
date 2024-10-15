// This code is part of the Problem Based Benchmark Suite (PBBS)
// Copyright (c) 2011 Guy Blelloch and the PBBS team
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights (to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include <math.h>

#include <algorithm>
#include <random>
#include <set>

#include "../utils/NSGDist.h"
#include "../utils/point_range.h"
#include "../utils/graph.h"
#include "../utils/graph_disk.h"
#include "../utils/types.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/delayed.h"
#include "parlay/random.h"
// #include "../utils/beamSearch.h"
#include "../utils/beamSearch_disk.h"

#include <sys/resource.h>
#include <iostream>


template<typename PointRange, typename indexType>
struct knn_index {
  using Point = typename PointRange::Point;
  using distanceType = typename Point::distanceType;
  using pid = std::pair<indexType, distanceType>;
  using PR = PointRange;
  using GraphI = Graph_disk<indexType>;

  BuildParams BP;
  std::set<indexType> delete_set;
  indexType start_point;

  knn_index(BuildParams &BP) : BP(BP) {}

  indexType get_start() { return start_point; }

  //robustPrune routine as found in DiskANN paper, with the exception
  //that the new candidate set is added to the field new_nbhs instead
  //of directly replacing the out_nbh of p
  std::pair<parlay::sequence<std::pair<indexType, distanceType>>, long>
  robustPrune(indexType p, parlay::sequence<pid>& cand,
              GraphI &G, PR &Points, double alpha, int offset, bool add = true) {
    // add out neighbors of p to the candidate set.
    size_t out_size = G[p].size();
    std::vector<pid> candidates;
    long distance_comps = 0;
    for (auto x : cand) candidates.push_back(x);

    if(add){
      for (size_t i=0; i<out_size; i++) {
        // distance_comps++;
        candidates.push_back(std::make_pair(G[p][i].first - offset, G[p][i].second));
      }
    }

    // Sort the candidate set according to distance from p
    auto less = [&](pid a, pid b) { return a.second < b.second; };
    std::sort(candidates.begin(), candidates.end(), less);

    // remove any duplicates
    auto new_end =std::unique(candidates.begin(), candidates.end(),
			      [&] (auto x, auto y) {return x.first == y.first;});
    candidates = std::vector(candidates.begin(), new_end);

    std::vector<std::pair<indexType, distanceType>> new_nbhs;
    new_nbhs.reserve(BP.R);

    size_t candidate_idx = 0;

    while (new_nbhs.size() < BP.R && candidate_idx < candidates.size()) {
      // Don't need to do modifications.
      auto p_star = candidates[candidate_idx];
      candidate_idx++;
      if (p_star.first == p || p_star.first == -1) {
        continue;
      }

      new_nbhs.push_back(p_star);

      for (size_t i = candidate_idx; i < candidates.size(); i++) {
        int p_prime = candidates[i].first;
        if (p_prime != -1) {
          distance_comps++;
          distanceType dist_starprime = Points[p_star.first].distance(Points[p_prime]);
          distanceType dist_pprime = candidates[i].second;
          if (alpha * dist_starprime <= dist_pprime) {
            candidates[i].first = -1;
          }
        }
      }
    }

    auto new_neighbors_seq = parlay::to_sequence(new_nbhs);
    return std::pair(new_neighbors_seq, distance_comps);
  }

  //wrapper to allow calling robustPrune on a sequence of candidates
  //that do not come with precomputed distances
  // std::pair<parlay::sequence<indexType>, long>
  // robustPrune(indexType p, parlay::sequence<indexType> candidates,
  //             GraphI &G, PR &Points, double alpha, bool add = true){

  //   parlay::sequence<pid> cc;
  //   long distance_comps = 0;
  //   cc.reserve(candidates.size()); // + size_of(p->out_nbh));
  //   for (size_t i=0; i<candidates.size(); ++i) {
  //     distance_comps++;
  //     cc.push_back(std::make_pair(candidates[i], Points[candidates[i]].distance(Points[p])));
  //   }
  //   auto [ngh_seq, dc] = robustPrune(p, cc, G, Points, alpha, add);
  //   return std::pair(ngh_seq, dc + distance_comps);
  // }

  // add ngh to candidates without adding any repeats
  template<typename rangeType1, typename rangeType2>
  void add_neighbors_without_repeats(const rangeType1 &ngh, rangeType2& candidates, int offset = 0) {
    std::unordered_set<indexType> a;
    // for (auto c : candidates) a.insert(c);
    for (const auto& [neighbor, distance] : candidates) a.insert(neighbor);
    for (int i=0; i < ngh.size(); i++)
      if (a.count(ngh[i].first - offset) == 0) candidates.push_back(std::make_pair(ngh[i].first - offset, ngh[i].second));
  }

  void set_start(){start_point = 0;}

  // 메모리 사용량을 출력하는 함수
  long get_memory_usage() {
      struct rusage usage;
      getrusage(RUSAGE_SELF, &usage);
      return usage.ru_maxrss;  // 현재 메모리 사용량 (KB)
  }

  void build_index(GraphI &G, PR &Points, stats<indexType> &BuildStats, bool sort_neighbors = true) {
    std::cout << "Building graph..." << std::endl;
    set_start();
    parlay::sequence<indexType> inserts = parlay::tabulate(Points.size(), [&] (size_t i){
					    return static_cast<indexType>(i);});
    std::cout << "number of points = " << Points.size() << std::endl;

    // if (BP.single_batch != 0) {
    //   int degree = BP.single_batch;
    //   std::cout << "Using single batch per round with " << degree << " random start edges" << std::endl;
    //   parlay::random_generator gen;
    //   std::uniform_int_distribution<long> dis(0, G.size());
    //   parlay::parallel_for(0, G.size(), [&] (long i) {
    //     std::vector<indexType> outEdges(degree);
    //     for (int j = 0; j < degree; j++) {
    //       auto r = gen[i*degree + j];
    //       outEdges[j] = dis(r);
    //     }
    //     G[i].update_neighbors(outEdges);
    //   });
    // }

    // last pass uses alpha
    std::cout << "number of passes = " << BP.num_passes << std::endl;
    for (int i=0; i < BP.num_passes; i++) {
      if (i == BP.num_passes - 1)
        batch_insert(inserts, G, Points, BuildStats, BP.alpha, true, 2, .02);
      else
        batch_insert(inserts, G, Points, BuildStats, 1.0, true, 2, .02);
    }

    if (sort_neighbors) {
      std::cout << "Sorting neighbors..." << std::endl;
      parlay::parallel_for (0, G.size(), [&] (long i) {
        // auto less = [&] (indexType j, indexType k) {
        //               return Points[i].distance(Points[j]) < Points[i].distance(Points[k]);};
        auto less = [&] (const std::pair<indexType, distanceType>& j, 
                        const std::pair<indexType, distanceType>& k) {
          return j.second < k.second;
        };
        G[i].sort(less);});
    }
  }

  void batch_insert(parlay::sequence<indexType> &inserts,
                    GraphI &G, PR &Points, stats<indexType> &BuildStats, double alpha,
                    bool random_order = false, double base = 2,
                    double max_fraction = .02, bool print=true) {
    // initial memory usage
    long max_memory_usage = get_memory_usage();

    for(int p : inserts){
      if(p < 0 || p > (int) G.size()){
        std::cout << "ERROR: invalid point "
                  << p << " given to batch_insert" << std::endl;
        abort();
      }
    }
    size_t n = G.size();
    size_t m = inserts.size();
    size_t inc = 0;
    size_t count = 0;
    float frac = 0.0;
    float progress_inc = .1;
    size_t max_batch_size = std::min(
        static_cast<size_t>(max_fraction * static_cast<float>(n)), 1000000ul);
    //fix bug where max batch size could be set to zero
    if(max_batch_size == 0) max_batch_size = n;
    parlay::sequence<int> rperm;
    if (random_order)
      rperm = parlay::random_permutation<int>(static_cast<int>(m));
    else
      rperm = parlay::tabulate(m, [&](int i) { return i; });
    auto shuffled_inserts =
        parlay::tabulate(m, [&](size_t i) { return inserts[rperm[i]]; });
    parlay::internal::timer t_beam("beam search time");
    parlay::internal::timer t_bidirect("bidirect time");
    parlay::internal::timer t_prune("prune time");
    t_beam.stop();
    t_bidirect.stop();
    t_prune.stop();
    while (count < m) {
      size_t floor;
      size_t ceiling;
      if (pow(base, inc) <= max_batch_size) {
        floor = static_cast<size_t>(pow(base, inc)) - 1;
        ceiling = std::min(static_cast<size_t>(pow(base, inc + 1)) - 1, m);
        count = std::min(static_cast<size_t>(pow(base, inc + 1)) - 1, m);
      } else {
        floor = count;
        ceiling = std::min(count + static_cast<size_t>(max_batch_size), m);
        count += static_cast<size_t>(max_batch_size);
      }

      if (BP.single_batch != 0) {
        floor = 0;
        ceiling = m;
        count = m;
      }
      
      // std::cout << "Batch insert: " << floor << " to " << ceiling << "(" << ceiling-floor << ")" << std::endl;

      // parlay::sequence<parlay::sequence<indexType>> new_out_(ceiling-floor);
      parlay::sequence<parlay::sequence<std::pair<indexType, distanceType>>> new_out_(ceiling-floor);

      // search for each node starting from the start_point, then call
      // robustPrune with the visited list as its candidㅋate set
      t_beam.start();

      parlay::parallel_for(floor, ceiling, [&](size_t i) {
        size_t index = shuffled_inserts[i];
        int sp = BP.single_batch ? i : start_point;
        QueryParams QP((long) 0, BP.L, (double) 0.0, (long) Points.size(), (long) G.max_degree());
        auto [beam_visited, bs_distance_comps] =
          beam_search<Point, PointRange, indexType>(Points[index], G, Points, sp, QP, 0);
        auto [beam, visited] = beam_visited;
        BuildStats.increment_dist(index, bs_distance_comps);
        BuildStats.increment_visited(index, visited.size());

        long rp_distance_comps;
        std::tie(new_out_[i-floor], rp_distance_comps) = robustPrune(index, visited, G, Points, alpha, 0);
        BuildStats.increment_dist(index, rp_distance_comps);
      });
      t_beam.stop();

      // make each edge bidirectional by first adding each new edge
      //(i,j) to a sequence, then semisorting the sequence by key values
      t_bidirect.start();

      auto flattened = parlay::delayed::flatten(parlay::tabulate(ceiling - floor, [&](size_t i) {
        indexType index = shuffled_inserts[i + floor];
        // return parlay::delayed::map(new_out_[i], [=] (indexType ngh) {
        //                               return std::pair(ngh, index);});}));
        return parlay::delayed::map(new_out_[i], [=] (std::pair<indexType, distanceType> ngh_dst) {
                                      return std::make_pair(ngh_dst.first, std::make_pair(index, ngh_dst.second));
                                      });}));
      auto grouped_by = parlay::group_by_key(parlay::delayed::to_sequence(flattened));

      // std::cout << "new_out_" << std::endl;
      // for (size_t i = 0; i < new_out_.size(); i++) {
      //   std::cout << shuffled_inserts[floor + i] << ") ";
      //   for (size_t j = 0; j < new_out_[i].size(); j++) {
      //     std::cout << new_out_[i][j].first << " ";
      //   }
      //   std::cout << std::endl;
      // }

      // std::cout << "grouped_by" << std::endl;
      // for (size_t i = 0; i < grouped_by.size(); i++) {
      //   std::cout << grouped_by[i].first << ") ";
      //   for (size_t j = 0; j < grouped_by[i].second.size(); j++) {
      //     std::cout << grouped_by[i].second[j].first << " ";
      //   }
      //   std::cout << std::endl;
      // }

      parlay::parallel_for(floor, ceiling, [&](size_t i) {
         G[shuffled_inserts[i]].update_neighbors(new_out_[i-floor]);
      });

      t_bidirect.stop();
      
      t_prune.start();
      // finally, add the bidirectional edges; if they do not make
      // the vertex exceed the degree bound, just add them to out_nbhs;
      // otherwise, use robustPrune on the vertex with user-specified alpha
      parlay::parallel_for(0, grouped_by.size(), [&](size_t j) {
        auto &[index, candidates] = grouped_by[j];
	      size_t newsize = candidates.size() + G[index].size();
        if (newsize <= BP.R) {
          add_neighbors_without_repeats(G[index], candidates);
          G[index].update_neighbors(candidates);
        } else {
          // auto [new_out_2_, distance_comps] = robustPrune(index, std::move(candidates), G, Points, alpha);
          auto [new_out_2_, distance_comps] = robustPrune(index, candidates, G, Points, alpha, 0);
          G[index].update_neighbors(new_out_2_);
          BuildStats.increment_dist(index, distance_comps);
        }
      });
      t_prune.stop();
      if (print && BP.single_batch == 0) {
        auto ind = frac * n;
        if (floor <= ind && ceiling > ind) {
          frac += progress_inc;
          std::cout << "Pass " << 100 * frac << "% complete"
                    << std::endl;
        }
      }
      inc += 1;

      // current memory usage
      long current_memory_usage = get_memory_usage();
      if (current_memory_usage > max_memory_usage) {
          max_memory_usage = current_memory_usage;
      }
    }

    // 최종적으로 기록된 최대 메모리 사용량 출력
    std::cout << "Max memory usage during batch insert: " << max_memory_usage << " KB" << std::endl;
    
    t_beam.total();
    t_bidirect.total();
    t_prune.total();
  }


  void build_index_disk(GraphI &G, PR &Points, stats<indexType> &BuildStats, double alpha, int start,
                        bool random_order = false, double base = 2, double max_fraction = .02, 
                        bool sort_neighbors = true, bool print=true){
    std::cout << "Building graph from " << start << std::endl;
    set_start();
    parlay::sequence<indexType> inserts = parlay::tabulate(Points.size(), [&] (size_t i){
					    return static_cast<indexType>(i);});
    std::cout << "number of points = " << Points.size() << std::endl;

    // // last pass uses alpha
    // std::cout << "number of passes = " << BP.num_passes << std::endl;
    // for (int i=0; i < BP.num_passes; i++) {
    //   if (i == BP.num_passes - 1)
    //     batch_insert(inserts, G, Points, BuildStats, BP.alpha, true, 2, .02);
    //   else
    //     batch_insert(inserts, G, Points, BuildStats, 1.0, true, 2, .02);
    // }

    // ========================== batch insert
    // initial memory usage
    // long max_memory_usage = get_memory_usage();

    for(int p : inserts){
      if(p < 0 || p > (int) G.size()){
        std::cout << "ERROR: invalid point "
                  << p << " given to batch_insert" << std::endl;
        abort();
      }
    }

    size_t n = G.size();
    size_t m = inserts.size();
    size_t inc = 0;
    size_t count = 0;
    float frac = 0.0;
    float progress_inc = .1;
    size_t max_batch_size = std::min(
        static_cast<size_t>(max_fraction * static_cast<float>(n)), 1000000ul);
    //fix bug where max batch size could be set to zero
    if(max_batch_size == 0) max_batch_size = n;
    parlay::sequence<int> rperm;
    if (random_order)
      rperm = parlay::random_permutation<int>(static_cast<int>(m));
    else
      rperm = parlay::tabulate(m, [&](int i) { return i; });
    auto shuffled_inserts =
        parlay::tabulate(m, [&](size_t i) { return inserts[rperm[i]]; });
    parlay::internal::timer t_beam("beam search time");
    parlay::internal::timer t_bidirect("bidirect time");
    parlay::internal::timer t_prune("prune time");
    t_beam.stop();
    t_bidirect.stop();
    t_prune.stop();
    while (count < m) {
      size_t floor;
      size_t ceiling;
      if (pow(base, inc) <= max_batch_size) {
        floor = static_cast<size_t>(pow(base, inc)) - 1;
        ceiling = std::min(static_cast<size_t>(pow(base, inc + 1)) - 1, m);
        count = std::min(static_cast<size_t>(pow(base, inc + 1)) - 1, m);
      } else {
        floor = count;
        ceiling = std::min(count + static_cast<size_t>(max_batch_size), m);
        count += static_cast<size_t>(max_batch_size);
      }

      if (BP.single_batch != 0) {
        floor = 0;
        ceiling = m;
        count = m;
      }
      
      std::cout << "Batch insert: " << floor << " to " << ceiling << "(" << ceiling-floor << ")" << std::endl;

      // parlay::sequence<parlay::sequence<indexType>> new_out_(ceiling-floor);
      parlay::sequence<parlay::sequence<std::pair<indexType, distanceType>>> new_out_(ceiling-floor);

      // search for each node starting from the start_point, then call
      // robustPrune with the visited list as its candidate set
      t_beam.start();

      parlay::parallel_for(floor, ceiling, [&](size_t i) {
        size_t index = shuffled_inserts[i];
        int sp = BP.single_batch ? i : start_point;
        // int sp = BP.single_batch ? i : start;
        QueryParams QP((long) 0, BP.L, (double) 0.0, (long) Points.size(), (long) G.max_degree());
        auto [beam_visited, bs_distance_comps] =
          beam_search<Point, PointRange, indexType>(Points[index], G, Points, sp, QP, start);
        auto [beam, visited] = beam_visited;
        BuildStats.increment_dist(index, bs_distance_comps);
        BuildStats.increment_visited(index, visited.size());

        long rp_distance_comps;
        std::tie(new_out_[i-floor], rp_distance_comps) = robustPrune(index, visited, G, Points, alpha, start);
        BuildStats.increment_dist(index, rp_distance_comps);
      });
      t_beam.stop();

      // make each edge bidirectional by first adding each new edge
      //(i,j) to a sequence, then semisorting the sequence by key values
      t_bidirect.start();

      auto flattened = parlay::delayed::flatten(parlay::tabulate(ceiling - floor, [&](size_t i) {
        indexType index = shuffled_inserts[i + floor];
        // return parlay::delayed::map(new_out_[i], [=] (indexType ngh) {
        //                               return std::pair(ngh, index);});}));
        return parlay::delayed::map(new_out_[i], [=] (std::pair<indexType, distanceType> ngh_dst) {
                                      return std::make_pair(ngh_dst.first, std::make_pair(index, ngh_dst.second));
                                      });}));
      auto grouped_by = parlay::group_by_key(parlay::delayed::to_sequence(flattened));

      // std::cout << "new_out_" << std::endl;
      // for (size_t i = 0; i < new_out_.size(); i++) {
      //   std::cout << shuffled_inserts[floor + i] << ") ";
      //   for (size_t j = 0; j < new_out_[i].size(); j++) {
      //     std::cout << new_out_[i][j].first << " ";
      //   }
      //   std::cout << std::endl;
      // }

      // std::cout << "grouped_by" << std::endl;
      // for (size_t i = 0; i < grouped_by.size(); i++) {
      //   std::cout << grouped_by[i].first << ") ";
      //   for (size_t j = 0; j < grouped_by[i].second.size(); j++) {
      //     std::cout << grouped_by[i].second[j].first << " ";
      //   }
      //   std::cout << std::endl;
      // }

      parlay::parallel_for(floor, ceiling, [&](size_t i) {
        // G[shuffled_inserts[i]].update_neighbors(new_out_[i-floor]);
        G[shuffled_inserts[i]].update_neighbors_global(new_out_[i-floor], start);
        // if (start > 0) {
        //   std::cout << "index " << shuffled_inserts[i] << " neighbors:";
        //   for (int j=0; j<G[shuffled_inserts[i]].size(); i++) {
        //     std::cout << " " << G[shuffled_inserts[i]][j].first;
        //   }
        //   std::cout << std::endl;
        // }
        
      });

      t_bidirect.stop();
      
      t_prune.start();
      // finally, add the bidirectional edges; if they do not make
      // the vertex exceed the degree bound, just add them to out_nbhs;
      // otherwise, use robustPrune on the vertex with user-specified alpha
      parlay::parallel_for(0, grouped_by.size(), [&](size_t j) {
        auto &[index, candidates] = grouped_by[j];
        size_t newsize = candidates.size() + G[index].size();
        if (newsize <= BP.R) {
          add_neighbors_without_repeats(G[index], candidates, start);
          G[index].update_neighbors_global(candidates, start);
          // if (start > 0){
          //   std::cout << "index " << index << " neighbors:";
          //   for (int i=0; i<G[index].size(); i++) {
          //     std::cout << " " << G[index][i].first;
          //   }
          //   std::cout << std::endl;
          // }
        } else {
          // auto [new_out_2_, distance_comps] = robustPrune(index, std::move(candidates), G, Points, alpha);
          auto [new_out_2_, distance_comps] = robustPrune(index, candidates, G, Points, alpha, start);
          G[index].update_neighbors_global(new_out_2_, start);
          BuildStats.increment_dist(index, distance_comps);
        }
      });
      t_prune.stop();
      if (print && BP.single_batch == 0) {
        auto ind = frac * n;
        if (floor <= ind && ceiling > ind) {
          frac += progress_inc;
          std::cout << "Pass " << 100 * frac << "% complete"
                    << std::endl;
        }
      }
      inc += 1;

      // // current memory usage
      // long current_memory_usage = get_memory_usage();
      // if (current_memory_usage > max_memory_usage) {
      //     max_memory_usage = current_memory_usage;
      // }
    }

    // // 최종적으로 기록된 최대 메모리 사용량 출력
    // std::cout << "Max memory usage during batch insert: " << max_memory_usage << " KB" << std::endl;
    
    t_beam.total();
    t_bidirect.total();
    t_prune.total();
    

    if (sort_neighbors) {
      std::cout << "Sorting neighbors..." << std::endl;
      parlay::parallel_for (0, G.size(), [&] (long i) {
        // auto less = [&] (indexType j, indexType k) {
        //               return Points[i].distance(Points[j]) < Points[i].distance(Points[k]);};
        auto less = [&] (const std::pair<indexType, distanceType>& j, 
                        const std::pair<indexType, distanceType>& k) {
          return j.second < k.second;
        };
        G[i].sort(less);});
    }
  }

};