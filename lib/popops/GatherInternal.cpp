// Copyright (c) 2019, Graphcore Ltd, All rights reserved.
#include "GatherInternal.hpp"

#include "popops/DynamicSlice.hpp"
#include "popops/ElementWise.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/exceptions.hpp"

#include <algorithm>
#include <vector>

namespace popops {
namespace internal {

namespace {
template <typename T> std::string serializeVector(const std::vector<T> &vs) {
  if (vs.size() > 0) {
    return "[" +
           std::accumulate(vs.begin() + 1, vs.end(), std::to_string(vs.front()),
                           [](const std::string &accum, T v) {
                             return accum + "," + std::to_string(v);
                           }) +
           "]";
  } else {
    return "[]";
  }
}

/**
 * Compute the output shape of a gather operation
 *
 *  \param inputShape   The shape of the input tensor
 *  \param indicesShape The shape of the indices tensor, which is assumed to be
 *                      2D
 *  \param slicesizes   The size of the slices from the input tensor
 *
 *  \returns The shape of output tensor of a gather
 *
 *  \note The output will have shape [indicesShape[0], slicesizes[0..k],
 *        inputShape[k..n]], where k is the size of indicesShape[1] and n is the
 *        rank of the input.
 */
std::vector<std::size_t>
outputShape(const std::vector<std::size_t> &inputShape,
            const std::vector<std::size_t> &indicesShape,
            std::vector<std::size_t> slicesizes) {
  if (indicesShape.size() != 2) {
    throw poputil::poplibs_error("Unexpected indices rank " +
                                 std::to_string(indicesShape.size()) +
                                 ", but expected 2");
  }
  std::vector<std::size_t> outputShape;

  outputShape.push_back(indicesShape[0]);
  std::copy(slicesizes.begin(), slicesizes.end(),
            std::back_inserter(outputShape));
  std::copy(inputShape.begin() + indicesShape[1], inputShape.end(),
            std::back_inserter(outputShape));

  if (outputShape.size() !=
      1 + slicesizes.size() + inputShape.size() - indicesShape[1]) {
    throw poputil::poplibs_error("Unexpected output rank");
  }

  return outputShape;
}

// Returns nothing, but throws an exception if the inputs are not valid
void checkGatherInputs(const poplar::Tensor &input,
                       const poplar::Tensor &indices,
                       const std::vector<std::size_t> &sliceSizes) {
  // This is required for the dynamic slices
  if (indices.elementType() != poplar::UNSIGNED_INT) {
    throw poputil::poplibs_error("Gather indices type must be unsigned int");
  }

  // We expect the indices to be MxN
  if (indices.rank() != 2) {
    throw poputil::poplibs_error(
        "Gather indices must be rank 2, but it is rank " +
        std::to_string(indices.rank()) + ": " +
        serializeVector(indices.shape()));
  }

  // In the MxN indices tensor,  we are taking M slices on N dimensions of the
  // input
  if (input.rank() < indices.dim(1)) {
    throw poputil::poplibs_error(
        "Gather input rank is " + std::to_string(input.rank()) +
        ", but it must be at least rank " + std::to_string(indices.dim(1)));
  }

  // For each sliced dimension, we must have a slice size
  if (sliceSizes.size() != indices.dim(1)) {
    throw poputil::poplibs_error(
        "Gather sliceSizes are size " + std::to_string(sliceSizes.size()) +
        ", but it must be size " + std::to_string(indices.dim(1)));
  }

  // Slice sizes cannot be larger than the input dimension size
  for (unsigned i = 0; i < sliceSizes.size(); ++i) {
    if (input.dim(i) < sliceSizes[i]) {
      throw poputil::poplibs_error(
          "Gather sliceSize on dimension " + std::to_string(i) + " is size " +
          std::to_string(sliceSizes[i]) + ", but it must be at most " +
          std::to_string(input.dim(i)));
    }
  }
}
} // namespace

poplar::Tensor
createGatherInputTensor(poplar::Graph &graph, poplar::Type type,
                        const std::vector<std::size_t> &inputShape,
                        const std::vector<std::size_t> &sliceSizes,
                        const std::string &name) {
  std::vector<std::size_t> slicedDims;
  for (unsigned d = 0; d != sliceSizes.size(); ++d)
    slicedDims.emplace_back(d);
  // TODO: if nonSlicedDimProduct is small we should add an outer stage
  return createSliceableTensor(graph, type, inputShape, slicedDims, sliceSizes,
                               0, name);
}

poplar::Tensor gather(poplar::Graph &graph, const poplar::Tensor &input,
                      const poplar::Tensor &indices,
                      const std::vector<std::size_t> &sliceSizes,
                      poplar::program::Sequence &prog,
                      const std::string &debugPrefix) {
  checkGatherInputs(input, indices, sliceSizes);

  // The dimensions that will be sliced
  std::vector<std::size_t> dims(indices.dim(1));
  std::iota(dims.begin(), dims.end(), input.rank() - indices.dim(1));

  std::vector<unsigned> permutation(input.rank());
  std::iota(permutation.begin(), permutation.end(), 0);
  std::rotate(permutation.begin(), permutation.begin() + indices.dim(1),
              permutation.end());

  auto result = multiSlice(graph, input.dimShuffle(permutation), indices, dims,
                           sliceSizes, prog, debugPrefix);

  std::iota(permutation.begin(), permutation.end(), 1);
  std::rotate(permutation.begin(),
              permutation.begin() + (input.rank() - indices.dim(1)),
              permutation.end());
  permutation.insert(permutation.begin(), 0);

  return result.dimShuffle(permutation);
}

} // namespace internal
} // namespace popops