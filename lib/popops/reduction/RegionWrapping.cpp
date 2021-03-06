// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
#include "RegionWrapping.hpp"

#include <boost/icl/split_interval_map.hpp>
#include <boost/icl/split_interval_set.hpp>

#include <poplibs_support/Algorithms.hpp>
#include <poplibs_support/vv_iterator.hpp>
#include <poputil/Util.hpp>
#include <poputil/exceptions.hpp>

using boost::icl::interval_map;
using boost::icl::interval_set;
using boost::icl::split_interval_map;
using boost::icl::split_interval_set;
using poplar::Interval;
using std::size_t;
using std::vector;
using namespace poplibs;

namespace popops {

split_interval_set<size_t> getSplitWrappedRegions(const vector<Interval> &ivals,
                                                  size_t wrapSize) {
  split_interval_set<size_t> wrapped;

  wrapRegions(
      ivals.begin(), ivals.end(), wrapSize, [&](size_t begin, size_t end) {
        wrapped.add(boost::icl::interval<size_t>::right_open(begin, end));
      });

  return wrapped;
}

interval_set<size_t> getWrappedRegions(const vector<Interval> &ivals,
                                       size_t wrapSize) {
  interval_set<size_t> wrapped;

  wrapRegions(
      ivals.begin(), ivals.end(), wrapSize, [&](size_t begin, size_t end) {
        wrapped.add(boost::icl::interval<size_t>::right_open(begin, end));
      });

  return wrapped;
}

std::vector<poplar::Interval>
splitOutputRegionsForWorkers(const unsigned grainSize, unsigned numPartitions,
                             poplar::Type partialType,
                             const std::vector<poplar::Interval> &regions) {

  if (grainSize == 0)
    throw poputil::poplibs_error("Zero vector width for type " +
                                 partialType.toString());

  unsigned minElementsPerPartition = 1;

  // splitRegions is used here rather than splitRegionsBetweenWorkers
  // because the former never merges regions, and the latter might merge them.
  auto split = poputil::splitRegions(regions, grainSize, numPartitions,
                                     minElementsPerPartition);

  // Flatten it because we take care of distributing them later.
  return flatten(split);
}

boost::icl::split_interval_set<std::size_t>
getSplitWrappedRegions(const std::vector<std::vector<poplar::Interval>> &ivals,
                       std::size_t wrapSize) {

  auto begin = vv_const_iterator<Interval>::begin(ivals);
  auto end = vv_const_iterator<Interval>::end(ivals);

  boost::icl::split_interval_set<size_t> colRegions;

  wrapRegions(begin, end, wrapSize, [&](size_t begin, size_t end) {
    colRegions.add(boost::icl::interval<size_t>::right_open(begin, end));
  });

  return colRegions;
}

boost::icl::interval_set<std::size_t>
getWrappedRegions(const std::vector<std::vector<poplar::Interval>> &ivals,
                  std::size_t wrapSize) {

  auto begin = vv_const_iterator<Interval>::begin(ivals);
  auto end = vv_const_iterator<Interval>::end(ivals);

  boost::icl::interval_set<size_t> colRegions;

  wrapRegions(begin, end, wrapSize, [&](size_t begin, size_t end) {
    colRegions.add(boost::icl::interval<size_t>::right_open(begin, end));
  });

  return colRegions;
}

} // namespace popops
