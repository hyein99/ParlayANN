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

#include <algorithm>

#include "../utils/NSGDist.h"
#include "../utils/beamSearch.h"
#include "../utils/check_nn_recall.h"
#include "../utils/parse_results.h"
#include "../utils/mips_point.h"
#include "../utils/euclidian_point.h"
#include "../utils/stats.h"
#include "../utils/types.h"
#include "../utils/graph.h"
#include "../utils/graph_disk.h"
#include "index.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/random.h"

template<typename Point, typename PointRange, typename QPointRange, typename indexType>
void ANN_(Graph<indexType> &G, long k, BuildParams &BP,
          PointRange &Query_Points, QPointRange &Q_Query_Points,
          groundTruth<indexType> GT, char *res_file,
          bool graph_built, PointRange &Points, QPointRange &Q_Points) {
}

template<typename Point, typename PointRange_, typename indexType>
void ANN(Graph<indexType> &G, long k, BuildParams &BP,
         PointRange_ &Query_Points,
         groundTruth<indexType> GT, char *res_file,
         bool graph_built, PointRange_ &Points) {
}


template<typename Point, typename PointRange_, typename indexType>
void ANN_disk(char *input_file, char *graph_file, char *out_file, char *res_file, bool graph_built,
              long k, BuildParams &BP, groundTruth<indexType> GT, int partition_size) {
  std::ifstream reader(input_file);
  //read num points and max degree
  unsigned int num_points;
  unsigned int d;
  reader.read((char*)(&num_points), sizeof(unsigned int));
  reader.read((char*)(&d), sizeof(unsigned int));
  int start;
  int end;

  for (int i = 0; i < partition_size; i++) {
    parlay::internal::timer t("ANN");

    start = i * num_points / partition_size;
    end = (i + 1) * num_points / partition_size;
    std::cout << "Reading partition " << i << " from " << start << " to " << end << std::endl;
    PointRange<float, Euclidian_Point<float>> Points = PointRange<float, Euclidian_Point<float>>(input_file, start, end);
    std::cout << "Read " << Points.size() << " points" << std::endl;

    Graph_disk<unsigned int> G;
    if(graph_file == NULL) G = Graph_disk<unsigned int>(BP.max_degree(), Points.size());
    else G = Graph_disk<unsigned int>(graph_file);

    bool verbose = BP.verbose;
    using findex = knn_index<PointRange_, indexType>;
    findex I(BP);
    indexType start_point;
    double idx_time;
    stats<unsigned int> BuildStats(G.size());
    if(graph_built){
      idx_time = 0;
      start_point = 0;
    } else{
      I.build_index_disk(G, Points, BuildStats, BP.alpha, start, true, 2, .02, true, true);
      // I.build_index(G, Points, BuildStats);
      start_point = I.get_start();
      // start_point = start;
      // idx_time = t.next_time();
    }
    std::cout << "start index = " << start_point << std::endl;

    // print graph examples
    for (int i = 0; i < 10; i++) {
      std::cout << "Example of vertex " << i << "(total " << G[i].size() << ")" << std::endl;
      for (int j = 0; j < G[i].size(); j++) {
        std::cout << "(" << G[i][j].first << " " << G[i][j].second << ") ";
      }
      std::cout << std::endl;
    }

    std::string name = "Vamana";
    std::string params =
      "R = " + std::to_string(BP.R) + ", L = " + std::to_string(BP.L);
    // auto [avg_deg, max_deg] = graph_stats_(G);
    auto od = parlay::delayed_seq<size_t>(
        G.size(), [&](size_t i) { return G[i].size(); });
    size_t j = parlay::max_element(od) - od.begin();
    int max_deg = od[j];
    size_t sum1 = parlay::reduce(od);
    double avg_deg = sum1 / ((double)G.size());
    auto vv = BuildStats.visited_stats();
    std::cout << "Average visited: " << vv[0] << ", Tail visited: " << vv[1]
              << std::endl;

    Graph_ G_(name, params, G.size(), avg_deg, max_deg, idx_time);
    G_.print();

    
    if(out_file != NULL) {
      std::string outfile(out_file);
      outfile += "_sub_" + std::to_string(i);
      std::cout << "Saving graph to " << outfile << std::endl;
      // G.save(outfile.c_str());
      G.save_subgraph(const_cast<char*>(outfile.c_str()));
    }

  }

}

template<typename Point, typename PointRange_, typename indexType>
void ANN(Graph_disk<indexType> &G, long k, BuildParams &BP,
         PointRange_ &Query_Points,
         groundTruth<indexType> GT, char *res_file,
         bool graph_built, PointRange_ &Points, char* outFile) {
  parlay::internal::timer t("ANN");

  bool verbose = BP.verbose;
  using findex = knn_index<PointRange_, indexType>;
  findex I(BP);
  indexType start_point;
  double idx_time;
  stats<unsigned int> BuildStats(G.size());
  if(graph_built){
    idx_time = 0;
    start_point = 0;
  } else{
    I.build_index(G, Points, BuildStats);
    start_point = I.get_start();
    idx_time = t.next_time();
  }
  std::cout << "start index = " << start_point << std::endl;

  std::string name = "Vamana";
  std::string params =
    "R = " + std::to_string(BP.R) + ", L = " + std::to_string(BP.L);
  // auto [avg_deg, max_deg] = graph_stats_(G);
  auto od = parlay::delayed_seq<size_t>(
      G.size(), [&](size_t i) { return G[i].size(); });
  size_t j = parlay::max_element(od) - od.begin();
  int max_deg = od[j];
  size_t sum1 = parlay::reduce(od);
  double avg_deg = sum1 / ((double)G.size());
  auto vv = BuildStats.visited_stats();
  std::cout << "Average visited: " << vv[0] << ", Tail visited: " << vv[1]
            << std::endl;

  Graph_ G_(name, params, G.size(), avg_deg, max_deg, idx_time);
  G_.print();

  long build_num_distances = parlay::reduce(parlay::map(BuildStats.distances,
                                                        [] (auto x) {return (long) x;}));

  if(outFile != NULL) {
    G.save(outFile);
  }

  Graph<indexType> G_r; 
  G_r = Graph<indexType>(outFile);

  if(Query_Points.size() != 0) {
    search_and_parse<Point, PointRange_, PointRange_, indexType>(G_, G_r, Points, Query_Points,
                                                                Points, Query_Points, GT,
                                                                res_file, k, false, start_point,
                                                                verbose);
  }
}