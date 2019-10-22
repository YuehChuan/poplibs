// Copyright (c) 2018, Graphcore Ltd, All rights reserved.

#ifndef popops_Reduce_hpp
#define popops_Reduce_hpp

#include "popops/Operation.hpp"
#include <poputil/exceptions.hpp>

#include "poplar/Graph.hpp"
#include "poplar/Program.hpp"
#include <poplar/OptionFlags.hpp>
#include <vector>

namespace popops {

/// A reduce operation can optionally scale the output, and can also be an
/// "update", i.e. out += reduce(in) rather than out = reduce(in).
///
/// ReduceParams stores that information, as well as the basic operation
/// being performed (add, mul, etc).

struct ReduceParams {
  ReduceParams() = default;
  // Allow implicit convertion from a popops::Operation.
  ReduceParams(popops::Operation op, bool update = false)
      : op(op), update(update) {
    useScale = false;
  }

  ReduceParams(popops::Operation op, bool update, poplar::Tensor scale)
      : op(op), update(update), scale(scale) {
    useScale = true;
  }

  // Explicitly disable the old API to avoid accidental type conversion
  ReduceParams(popops::Operation op, float constantScale,
               bool update = false) = delete;

  popops::Operation op;
  bool update;
  bool useScale;
  poplar::Tensor scale;
};

// Debug information about the reduction. This is internal currently.
struct ReductionDebug;

/// Reduce `in` in dimensions `dims`. params specifies the operation. Note that
/// currently scale and update are only valid with the ADD or SQUARE_ADD
/// operations.
///
/// Optionally a ReductionDebug object can be filled in with debug information
/// to help visualise and debug the reduction.
///
/// Internally this creates a new variable for the output then calls
/// reduceWithOutput(). The type of the output will be `outType`.
///
/// The options parameter accepts the following:
///
///    * accumType.interTile: The type to used for intermediate values
///                            between tiles (either 'float' or 'half').
///    * accumType.inVertex: The type to used for intermediate values within
///                           a vertex (either 'float' or 'half').
///
/// If either of the above options are not set then the intermediate type will
/// default to either the input tensor element type or `float` if the input
/// is of type 'half' and the reduction operation benefits from
/// higher precision (e.g. add).
///
/// The input and output types that are supported depend on the operation:
///
/// | Operation               | Types                              |
/// |-------------------------|------------------------------------|
/// | ADD, SQUARE_ADD, MUL    | float->float, half->half, int->int |
/// |                         | float->half, half->float           |
/// | MAX, MIN                | float->float, half->half, int->int |
/// | LOGICAL_AND, LOGICAL_OR | bool->bool                         |
///
poplar::Tensor reduce(poplar::Graph &graph, const poplar::Tensor &in,
                      const poplar::Type &outType,
                      const std::vector<std::size_t> &dims, ReduceParams params,
                      poplar::program::Sequence &prog,
                      const std::string &debugPrefix = "",
                      const poplar::OptionFlags &options = {},
                      ReductionDebug *debug = nullptr);

// An alias for reduce(graph, in, in.elementType(), ...)
poplar::Tensor reduce(poplar::Graph &graph, const poplar::Tensor &in,
                      const std::vector<std::size_t> &dims, ReduceParams params,
                      poplar::program::Sequence &prog,
                      const std::string &debugPrefix = "",
                      const poplar::OptionFlags &options = {},
                      ReductionDebug *debug = nullptr);

/// This is similar to reduce() but allows you to specify the output.
/// If the tile mapping of `out` is not complete it will be set. Otherwise it
/// won't be changed.
void reduceWithOutput(poplar::Graph &graph, const poplar::Tensor &in,
                      const poplar::Tensor &out,
                      const std::vector<std::size_t> &dims, ReduceParams params,
                      poplar::program::Sequence &prog,
                      const std::string &debugPrefix = "",
                      const poplar::OptionFlags &options = {},
                      ReductionDebug *debug = nullptr);

/// The following are alternate forms that add their vertices to a vector
/// of compute sets instead of a Sequence. The caller is expected to add
/// each compute set to a Sequence (in an Execute) themselves, like this:
///
///   Sequence seq;
///   std::vector<ComputeSet> css;
///   auto A = reduce(..., css);
///   auto B = reduce(..., css);
///   for (const auto &cs : css) {
///     seq.add(Execute(cs));
///
/// This allows you to do multiple reductions in parallel. Note that the
/// reductions are not aware of each other, so it may be more efficient
/// to concatenate tensors and do a single reduction instead if they have the
/// same shape, operation, and input and output types.
poplar::Tensor reduce(poplar::Graph &graph, const poplar::Tensor &in,
                      const poplar::Type &outType,
                      const std::vector<std::size_t> &dims, ReduceParams params,
                      std::vector<poplar::ComputeSet> &css,
                      const std::string &debugPrefix = "",
                      const poplar::OptionFlags &options = {},
                      ReductionDebug *debug = nullptr);

poplar::Tensor reduce(poplar::Graph &graph, const poplar::Tensor &in,
                      const std::vector<std::size_t> &dims, ReduceParams params,
                      std::vector<poplar::ComputeSet> &css,
                      const std::string &debugPrefix = "",
                      const poplar::OptionFlags &options = {},
                      ReductionDebug *debug = nullptr);

void reduceWithOutput(poplar::Graph &graph, const poplar::Tensor &in,
                      const poplar::Tensor &out,
                      const std::vector<std::size_t> &dims, ReduceParams params,
                      std::vector<poplar::ComputeSet> &css,
                      const std::string &debugPrefix = "",
                      const poplar::OptionFlags &options = {},
                      ReductionDebug *debug = nullptr);

} // namespace popops

#endif // popops_Reduce_hpp
