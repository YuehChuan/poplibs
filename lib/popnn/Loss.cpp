#include "popnn/Loss.hpp"

#include "poplar/Graph.hpp"
#include "poplibs_support/Algorithm.hpp"
#include "popops/Cast.hpp"
#include "popops/ElementWise.hpp"
#include "popops/Encoding.hpp"
#include "popops/Reduce.hpp"
#include "poputil/Broadcast.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/Util.hpp"
#include "poputil/VertexTemplates.hpp"
#include "poputil/exceptions.hpp"

#include <boost/optional.hpp>
#include <cassert>
#include <limits>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;

namespace popnn {

namespace {

// Just a shorthand to compute ceiling of the quotient (a/b)
inline unsigned quotCeiling(unsigned a, unsigned b) { return (a + b - 1) / b; }

// Per-element of model outputs, on the tile these are mapped to
// compute the gradient and the contribution to loss.
Tensor onTileTransform(Graph &graph, const Tensor &modelOutputs,
                       const Tensor &expected, const Tensor &deltas,
                       boost::optional<Tensor> &_deltasScale,
                       boost::optional<Tensor> &_modelOutputScaling,
                       const std::string &vertexClassTemplate,
                       LossType lossType, Sequence &prog,
                       const std::string &debugPrefix = "") {
  const auto &target = graph.getTarget();
  const auto &dType = modelOutputs.elementType();

  Tensor deltasScale, modelOutputScaling;
  if (_deltasScale.is_initialized()) {
    if (lossType == LossType::CROSS_ENTROPY_LOSS) {
      deltasScale = _deltasScale.get();
      modelOutputScaling = _modelOutputScaling.get();
    } else {
      throw poplibs_error("Loss scaling not implemented for this loss type");
    }
  } else if (lossType == LossType::CROSS_ENTROPY_LOSS) {
    deltasScale = graph.addConstant(deltas.elementType(), {}, 1.0f);
    modelOutputScaling = graph.addConstant(deltas.elementType(), {}, 1.0f);
    graph.setTileMapping(deltasScale, 0);
    graph.setTileMapping(modelOutputScaling, 0);
  }

  const auto vertexClass = templateVertex(vertexClassTemplate, dType);
  auto transformCS = graph.addComputeSet(debugPrefix + "/on_tile_transform");
  const auto batchSize = modelOutputs.dim(0);
  const auto perBatch = modelOutputs.numElements() / batchSize;
  auto transformed = graph.addVariable(dType, {batchSize, perBatch},
                                       debugPrefix + "/Transformed");
  auto mapping = graph.getTileMapping(modelOutputs);
  for (std::size_t tile = 0; tile < mapping.size(); ++tile) {
    const auto &tileMapping = mapping[tile];
    const auto grainSize = target.getVectorWidth(dType);
    auto contiguousRegions =
        graph.getSortedContiguousRegions(modelOutputs, tileMapping);
    // Split delta and transformed calculation between workers on
    // each tile if it's advantageous
    // Optimisation: Focus point for memory usage - more vertices = more
    // memory. Number of vertices needed depends on the spread of
    // modelOutputs over tiles, and grain size for split among
    // workers. Currently inputs are gathered via on-tile copy before
    // this transform. Were this gather particularly expensive specialised
    // single/multiple/2D transforms could be added but doesn't currently
    // seem worth it.

    // The maximum size of the region is 2^12 - 1
    auto workerRegions =
        splitRegionsBetweenWorkers(target, contiguousRegions, grainSize, 0xFFF);

    for (const auto &vertexRegions : workerRegions) {
      auto vertexTransformed =
          concat(transformed.flatten().slices(vertexRegions));
      auto vertexDeltas = concat(deltas.flatten().slices(vertexRegions));

      auto transformV = graph.addVertex(
          transformCS, vertexClass,
          {{"probs", concat(modelOutputs.flatten().slices(vertexRegions))},
           {"expected", concat(expected.flatten().slices(vertexRegions))},
           {"deltas", vertexDeltas},
           {"transformed", vertexTransformed}});
      if (lossType == LossType::CROSS_ENTROPY_LOSS) {
        graph.connect(transformV["deltasScale"], deltasScale);
        graph.connect(transformV["modelOutputScaling"], modelOutputScaling);
      }
      graph.setInitialValue(transformV["size"],
                            vertexTransformed.numElements());
      graph.setTileMapping(vertexTransformed, tile);
      graph.setTileMapping(vertexDeltas, tile);
      graph.setTileMapping(transformV, tile);
    }
  }
  prog.add(Execute(transformCS));
  return transformed;
}

// Parameters needed to create one ReduceXxxClassGather vertex, for the first
// stage reduction in argMinOrMax().
struct ClassGatherVertexInfo {
  unsigned tile;       // In which tile to place the vertex
  unsigned row;        // Which row the elements belogs to
  unsigned offsIn;     // Offset (in elements) inside 'row'
  unsigned size;       // Total size (in elements)
  unsigned offsOut;    // Offset where to put the results in the partials
  unsigned workerSize; // Processed by one worker (except possibly the last one)
  unsigned workerNum;  // How many worker (i.e. how many partials)
};

/// Generate the work partition for the first stage reduction of argMinOrMax.
/// Tries to spread the work uniformly among all IPU tiles.
/// Each tile will have one or more vertices, so that the total number of
/// elements processed per tile is balanced.
/// Also we don't want to assign too little work per worker (and supervisor)
/// and not too much per worker (as the workers use the RPT instrucion.)
///
/// \param[in] target         Target wqhere we are running the graph.
/// \param[in] nRows          Number of rows (batches) in the input matrix
/// \param[in] nCols          Number of columns (classes) in the input matrix
///
/// \param[out] partialsPerRow one element per row, specifying how many
//                             partial outputs will be generated for this row
///                            (i.e. how many workers per row)
/// \param[out] vertexInfo     one element per vertex to create, containing all
//                             parameters for the vertex
void argMinMaxSplitFirstReduction(
    const Target &target, unsigned nRows, unsigned nCols,
    std::vector<unsigned> &partialsPerRow,
    std::vector<ClassGatherVertexInfo> &vertexInfo) {

  const auto numTileWorkers = target.getNumWorkerContexts();
  // Min and max elements to be processed by one worker.
  const unsigned workerMin = 32;
  const unsigned workerMax = target.getRptCountMax();
  // Min elements to be processed by one supervisor vertex
  const unsigned tileMin = numTileWorkers * workerMin;
  const uint64_t totalSize = nRows * nCols; // elements in the matrix
  auto elemsPerTile =
      std::max(tileMin, quotCeiling(totalSize, target.getTilesPerIPU()));
  auto tilesToUse = quotCeiling(totalSize, elemsPerTile);
  // Starting up.
  unsigned row = 0;
  unsigned offsIn = 0;  // offset (from row start) of the vertex input data
  unsigned offsOut = 0; // offset (from row start) of the vertex output partials
  unsigned numPartials = 0; // how many partials for the current row
  // Distribute the elements among tilesToUse tiles.
  for (unsigned tile = 0; tile < tilesToUse; ++tile) {
    const uint64_t elemBegin = (tile * totalSize) / tilesToUse;
    const uint64_t elemEnd = ((tile + 1) * totalSize) / tilesToUse;
    // Total size in this tile.
    uint64_t tileSize = elemEnd - elemBegin;
    // While there are still elements to add to this tile...
    while (tileSize > 0) {
      // Are we finished with this row?
      if (offsIn == nCols) {
        partialsPerRow.push_back(numPartials);
        numPartials = 0;
        row++;
        offsIn = 0;
        offsOut = 0;
      }
      // Try to give one vertex all that is left in tileSize (or whatever
      // is left to the end of the row)
      unsigned vertexSize = std::min<uint64_t>(tileSize, nCols - offsIn);
      unsigned workerSize, numWorkers;
      // Make sure each worker does a minimum of work
      if (vertexSize / numTileWorkers >= workerMin) {
        // Enough work for all 6 workers
        workerSize = quotCeiling(vertexSize, numTileWorkers);
        // but not too much work (RPT counter is limited)
        if (workerSize > workerMax) {
          workerSize = workerMax;
          vertexSize = numTileWorkers * workerSize;
        }
        numWorkers = numTileWorkers;
      } else {
        // Cannot give enough work to all 6 worker
        workerSize = workerMin;
        numWorkers = quotCeiling(vertexSize, workerMin);
      }
      // Store away the parameters for this vertex
      vertexInfo.push_back(
          {tile, row, offsIn, vertexSize, offsOut, workerSize, numWorkers});
      numPartials += numWorkers;
      offsIn += vertexSize;
      offsOut += numWorkers;
      tileSize -= vertexSize;
    }                                    // while (tileSize > 0)
  }                                      // for (tile)
  partialsPerRow.push_back(numPartials); // add last one
}

} // end anonymous namespace

Program calcLoss(Graph &graph, const Tensor &modelOutputs,
                 const Tensor &expected, const Tensor &loss,
                 const Tensor &deltas, boost::optional<Tensor> &deltasScale,
                 boost::optional<Tensor> &modelOutputScaling, LossType lossType,
                 const std::string &debugPrefix) {
  std::string layerPrefix = debugPrefix;
  std::string transformVertexClass;
  switch (lossType) {
  case LossType::SUM_SQUARED_LOSS:
    layerPrefix += "/LossSumSquared";
    transformVertexClass = "popnn::LossSumSquaredTransform";
    break;
  case LossType::CROSS_ENTROPY_LOSS:
    layerPrefix += "/LossCrossEntropy";
    transformVertexClass = "popnn::LossCrossEntropyTransform";
    break;
  default:
    throw poplibs_error("Unknown loss type requested in calcLoss");
    break;
  }
  Sequence prog;
  const auto &target = graph.getTarget();
  const auto &dType = modelOutputs.elementType();
  const unsigned atomicStoreGranularity = target.getAtomicStoreGranularity();
  const unsigned exchangeBytesPerCycles = target.getExchangeBytesPerCycle();
  auto minBytes = std::max(atomicStoreGranularity, exchangeBytesPerCycles);
  auto dTypeSize = target.getTypeSize(dType);

  // Determine if we have to layout the one hot encoded tensor with the
  // innermost dimension having contiguous elements on a tile.
  const auto dimGrouping = detectInnermostGrouping(graph, modelOutputs);
  Tensor oneHot;
  if (dimGrouping * dTypeSize < minBytes) {
    oneHot = graph.addVariable(dType, modelOutputs.shape(),
                               layerPrefix + "/OneHotEncoded");
    mapTensorLinearly(graph, oneHot, 0, target.getVectorWidth(dType));
  } else {
    oneHot = graph.clone(modelOutputs.elementType(), modelOutputs,
                         layerPrefix + "/OneHotEncoded");
  }
  popops::encodeOneHot(graph, expected, oneHot, prog, layerPrefix);

  // Compute loss partials and deltas
  auto transformed = onTileTransform(
      graph, modelOutputs, oneHot, deltas, deltasScale, modelOutputScaling,
      transformVertexClass, lossType, prog, layerPrefix);
  // the gradients for masked labels are not masked out to 0 by the on tile
  // transform. This does this explicitly here for such label.
  if (lossType == CROSS_ENTROPY_LOSS && deltas.rank() == 2 &&
      expected.numElements() > 1) {
    auto maskedLabelCode =
        graph.addConstant(expected.elementType(), {}, MASKED_LABEL_CODE);
    graph.setTileMapping(maskedLabelCode, 0);
    auto nonMaskedLabels =
        popops::neq(graph, expected, maskedLabelCode, prog, debugPrefix);
    auto maskScale = popops::cast(graph, nonMaskedLabels, deltas.elementType(),
                                  prog, debugPrefix);
    popops::mulInPlace(graph, deltas.transpose(), maskScale, prog, debugPrefix);
  }
  // Reduce values in each batch
  popops::reduceWithOutput(graph, transformed, loss, {1},
                           popops::Operation::ADD, prog,
                           layerPrefix + "/reduce_loss");
  return prog;
}

Program calcLoss(Graph &graph, const Tensor &modelOutputs,
                 const Tensor &expected, const Tensor &loss,
                 const Tensor &deltas, const Tensor &_deltasScale,
                 const Tensor &_modelOutputScaling, LossType lossType,
                 const std::string &debugPrefix) {
  boost::optional<Tensor> deltasScale = _deltasScale;
  boost::optional<Tensor> modelOutputScaling = _modelOutputScaling;
  return calcLoss(graph, modelOutputs, expected, loss, deltas, deltasScale,
                  modelOutputScaling, lossType, debugPrefix);
}

Program calcLoss(Graph &graph, const Tensor &modelOutputs,
                 const Tensor &expected, const Tensor &loss,
                 const Tensor &deltas, LossType lossType,
                 const std::string &debugPrefix) {
  boost::optional<Tensor> deltasScale, modelOutputScaling;
  return calcLoss(graph, modelOutputs, expected, loss, deltas, deltasScale,
                  modelOutputScaling, lossType, debugPrefix);
}

/// Returns the indices of the max (or min) values for each row of a 2-D tensor.
///
/// \param[in] graph       the graph for the tensor
/// \param[in] input       the (2-D) tensor to examine
/// \param[in] resultType  type to use for the result elements
/// \param[in] prog        the sequence to add compute sets to
/// \param[in] resultTile  the tile where the final result must be placed
/// \param[in] debugPrefix as the name says
/// \param[in] max         if True find max, else find min
///
/// \return a 1-D tensor of integral type with as many elements as the rows of
///         'input', each one being the index where the max (or min) value for
///          that row is in 'input'.
static Tensor argMinOrMax(Graph &graph, const Tensor &input,
                          const Type &resultType, Sequence &prog,
                          unsigned resultTile, const std::string &debugPrefix,
                          bool max = true) {
  const std::string lowerCase = max ? "max" : "min";
  const std::string capitalized = max ? "Max" : "Min";
  const auto layerPrefix = debugPrefix + "/argMinOrMax(" + lowerCase + ")/";
  const auto &target = graph.getTarget();
  const auto tilesPerIPU = target.getTilesPerIPU();
  const size_t nRows = input.dim(0);
  const size_t nCols = input.numElements() / nRows;
  const auto inputType = input.elementType();
  // We set the partial values (max/min) to always be 32-bit floats. This works
  // both if the inputs are half or floats, and avoids half-word writes for the
  // partials values. Memory cost is considered negligible as there are few of
  // these partials (2nd stage will have 1/workerMin of initial elements etc).
  const auto partialsType =
      (inputType == HALF || inputType == FLOAT) ? FLOAT : inputType;

  // First stage of reductions (a single compute set).
  // In this stage we use supervisor vertices that will produce as output
  // multiple pairs of partial result, one for each worker processing a chunk of
  // data.
  // The outputs are the max (or min) value for that chunk and the index for the
  // max/min.

  const auto cs = graph.addComputeSet(layerPrefix + "ReduceClass[0]");
  std::vector<unsigned> numPartials;
  std::vector<ClassGatherVertexInfo> vertexInfo;
  argMinMaxSplitFirstReduction(target, nRows, nCols, numPartials, vertexInfo);
  // How many rows will be fully reduced to a single element by this stage.
  unsigned rowsFullyReduced =
      std::count_if(numPartials.begin(), numPartials.end(),
                    [](unsigned count) { return count == 1; });
  // The partials generated by this first stage, input for second stage. Each
  // row might have a different number of partials.
  std::vector<Tensor> valuePartials(nRows);
  std::vector<Tensor> indexPartials(nRows);
  for (unsigned row = 0; row < nRows; row++) {
    valuePartials[row] = graph.addVariable(partialsType, {numPartials[row]},
                                           layerPrefix + "ValuePartials[0][" +
                                               std::to_string(row) + "]");
    indexPartials[row] = graph.addVariable(resultType, {numPartials[row]},
                                           layerPrefix + "IndexPartials[0][" +
                                               std::to_string(row) + "]");
  }
  const auto vertexGather = templateVertex(
      "popnn::Reduce" + capitalized + "ClassGather", inputType, resultType);
  // Create all vertices for first stage.
  for (auto vi : vertexInfo) {
    const auto v = graph.addVertex(cs, vertexGather);
    auto inputPartials = input[vi.row].slice(vi.offsIn, vi.offsIn + vi.size);
    auto vertexValuePartials =
        valuePartials[vi.row].slice(vi.offsOut, vi.offsOut + vi.workerNum);
    auto vertexIndexPartials =
        indexPartials[vi.row].slice(vi.offsOut, vi.offsOut + vi.workerNum);
    graph.connect(v["activations"], inputPartials);
    graph.setInitialValue(v["index"], vi.offsIn);
    graph.connect(v[lowerCase + "Value"], vertexValuePartials);
    graph.connect(v[lowerCase + "Index"], vertexIndexPartials);
    graph.setInitialValue(v["size"], vi.size);
    graph.setInitialValue(v["workerSize"], vi.workerSize);
    graph.setTileMapping(vertexValuePartials, vi.tile);
    graph.setTileMapping(vertexIndexPartials, vi.tile);
    graph.setTileMapping(v, vi.tile);
  }
  prog.add(Execute(cs));

  // The second and successive stages (each one is one compute set) will divide
  // the partials in batches of 'partialsSize' elements to be processed each by
  // a single worker vertex.
  // For these stages, both the input and the output of each stage are the
  // 1D tensors of max/min (float) values and their corresponding indices.

  unsigned tile = 0;
  std::size_t reduceIndex = 1; // stage of the reduction
  // How many data element (max) will be processed by one worker vertex.
  const std::size_t partialsSize = 32;
  const auto vertexSparse = templateVertex(
      "popnn::Reduce" + capitalized + "ClassSparse", partialsType, resultType);
  // Do it until we have reduced to a single element (per row) on all rows.
  while (rowsFullyReduced < nRows) {
    const auto stageStr = "[" + std::to_string(reduceIndex) + "]";
    const auto cs = graph.addComputeSet(layerPrefix + "ReduceClass" + stageStr);
    for (std::size_t row = 0; row < nRows; ++row) {
      // if rows was already reduced, nothing to do
      if (numPartials[row] > 1) {
        const std::string suffix =
            "Partials" + stageStr + "[" + std::to_string(row) + "]";
        unsigned nextNumPartials = quotCeiling(numPartials[row], partialsSize);
        // New partials for this row (output from this stage)
        auto nextValuePartials = graph.addVariable(
            partialsType, {nextNumPartials}, layerPrefix + "Value" + suffix);
        auto nextIndexPartials = graph.addVariable(
            resultType, {nextNumPartials}, layerPrefix + "Index" + suffix);
        // All vertices for this row
        for (size_t i = 0, offs = 0; offs < numPartials[row];
             i++, offs += partialsSize) {
          const auto v = graph.addVertex(cs, vertexSparse);
          const auto size = std::min(numPartials[row] - offs, partialsSize);
          // Input values/indices for this vertex
          auto splitValuePartials = valuePartials[row].slice(offs, offs + size);
          auto splitIndexPartials = indexPartials[row].slice(offs, offs + size);
          graph.connect(v["activations"], splitValuePartials);
          graph.connect(v["labels"], splitIndexPartials);
          graph.connect(v[lowerCase + "Value"], nextValuePartials[i]);
          graph.connect(v[lowerCase + "Index"], nextIndexPartials[i]);
          graph.setTileMapping(nextValuePartials[i], tile);
          graph.setTileMapping(nextIndexPartials[i], tile);
          graph.setTileMapping(v, tile);
          tile = (tile + 1) % tilesPerIPU;
        } // for (i,offs)
        // the outputs just generated become the inputs of next stage
        valuePartials[row] = nextValuePartials;
        indexPartials[row] = nextIndexPartials;
        numPartials[row] = nextNumPartials;
        if (nextNumPartials == 1) {
          rowsFullyReduced++;
        }
      } // row was not reduced yet
    }   // for (nRows)
    prog.add(Execute(cs));
    reduceIndex++;
  } // while (rowsFullyReduced < nRows)
  return concat(indexPartials);
}

static Tensor TopKImpl(Graph &graph, const poplar::Tensor &input,
                       poplar::Tensor &indices, const std::size_t k, bool sort,
                       const Type &argminType, Sequence &prog,
                       unsigned numCorrectTile,
                       const std::string &debugPrefix) {
  const auto layerPrefix = debugPrefix + "/topk";
  const auto &target = graph.getTarget();
  const auto tilesPerIPU = target.getTilesPerIPU();
  const auto batchSize = input.dim(0);

  std::string reduceGatherVertexClass =
      templateVertex("popnn::ReduceMaxNClassGather", input.elementType(), sort);

  std::string reduceSparseVertexClass =
      templateVertex("popnn::ReduceMaxNClassSparse", input.elementType(), sort);

  const auto numWorkers = target.getNumWorkerContexts();
  std::vector<ComputeSet> reductionCS;
  Tensor lastValuePartials = input;
  Tensor lastIndexPartials;
  std::size_t lastBatchPartials = lastValuePartials.numElements() / batchSize;

  std::size_t reduceIndex = 0;
  unsigned nextTile = 0;
  while (lastBatchPartials > 1) {
    bool isFirstReduce = (reduceIndex == 0);

    std::size_t numGatherThreads =
        target.getNumTiles() * target.getNumWorkerContexts();
    std::size_t numSparseThreads = target.getNumTiles();
    std::size_t partialsFactor = 0;

    const std::size_t minFactor = std::max<std::size_t>(k * 2, 32);
    if (isFirstReduce) {
      partialsFactor =
          std::max(minFactor, (lastBatchPartials + numGatherThreads - 1) /
                                  numGatherThreads);
    } else {

      partialsFactor =
          std::max(minFactor, (lastBatchPartials + numSparseThreads - 1) /
                                  numSparseThreads);
    }

    const auto divisorLog2 = poplibs_support::ceilLog2(partialsFactor);
    const auto divisor = (1u << divisorLog2);

    const auto batchPartials = (lastBatchPartials + divisor - 1) / divisor;

    bool isLastReduce = (batchPartials == 1);
    std::string vertexClass =
        isFirstReduce ? reduceGatherVertexClass : reduceSparseVertexClass;
    reductionCS.push_back(graph.addComputeSet(
        layerPrefix + "/ReduceNMaxClass[" + std::to_string(reduceIndex) + "]"));
    const auto &cs = reductionCS.back();

    poplar::Tensor valuePartials =
        graph.addVariable(input.elementType(), {batchSize, batchPartials, k},
                          layerPrefix + "/NMaxValuePartials[" +
                              std::to_string(reduceIndex) + "]");
    poplar::Tensor indexPartials =
        graph.addVariable(UNSIGNED_INT, {batchSize, batchPartials, k},
                          layerPrefix + "/NMaxIndexPartials[" +
                              std::to_string(reduceIndex) + "]");
    for (std::size_t b = 0; b < batchSize; ++b) {
      std::size_t batchOffset = 0;
      std::size_t partialsIndex = 0;
      while (batchOffset != lastBatchPartials) {
        // If this is the last reduction, put the reduction on the tile where
        // the final accuracy will be calculated.
        const auto tile = isLastReduce ? numCorrectTile : nextTile;

        const auto v = graph.addVertex(cs, vertexClass);

        // If this is the last reduction and the user specified they wanted the
        // output to be sorted.
        graph.setInitialValue(v["shouldSort"], sort && isLastReduce);

        if (isFirstReduce) {
          // This first reduction uses a supervisor vertex, so try and give it
          // a grain of splits per-worker each time.
          const auto supervisorPartials = divisor * numWorkers;
          const auto partialsThisSplit = std::min<size_t>(
              lastBatchPartials - batchOffset, supervisorPartials);

          const auto nOutputs = (partialsThisSplit + divisor - 1) / divisor;

          auto splitValuePartials = lastValuePartials[b].slice(
              batchOffset, batchOffset + partialsThisSplit);

          // Split the partials into a series of
          poplar::Tensor splitMaxValue =
              valuePartials[b].slice(partialsIndex, partialsIndex + nOutputs);
          splitMaxValue = splitMaxValue.flatten();

          poplar::Tensor splitMinIndex =
              indexPartials[b].slice(partialsIndex, partialsIndex + nOutputs);
          splitMinIndex = splitMinIndex.flatten();

          graph.setInitialValue(v["index"], batchOffset);
          graph.connect(v["activations"], splitValuePartials);
          graph.connect(v["maxValues"], splitMaxValue);
          graph.connect(v["maxValuesIndices"], splitMinIndex);
          graph.setInitialValue(v["size"], partialsThisSplit);
          graph.setInitialValue(v["numK"], k);
          graph.setInitialValue(v["divisorLog2"], divisorLog2);
          graph.setTileMapping(splitMaxValue, tile);
          graph.setTileMapping(splitMinIndex, tile);
          partialsIndex += nOutputs;
          batchOffset += partialsThisSplit;
        } else {
          const auto partialsThisSplit =
              std::min<size_t>(lastBatchPartials - batchOffset, divisor);

          // Turn the previous output into this input.
          poplar::Tensor splitValuePartials = lastValuePartials[b].slice(
              batchOffset, batchOffset + partialsThisSplit);

          poplar::Tensor splitIndexPartials = lastIndexPartials[b].slice(
              batchOffset, batchOffset + partialsThisSplit);

          splitIndexPartials = splitIndexPartials.flatten();
          splitValuePartials = splitValuePartials.flatten();
          graph.connect(v["labels"], splitIndexPartials);
          graph.connect(v["activations"], splitValuePartials);
          graph.connect(v["maxValues"], valuePartials[b][partialsIndex]);
          graph.connect(v["maxValuesIndices"], indexPartials[b][partialsIndex]);
          graph.setInitialValue(v["numK"], k);

          graph.setTileMapping(valuePartials[b][partialsIndex], tile);
          graph.setTileMapping(indexPartials[b][partialsIndex], tile);
          ++partialsIndex;
          batchOffset += partialsThisSplit;
        }
        graph.setTileMapping(v, tile);
        nextTile = (nextTile + 1) % tilesPerIPU;
        assert(batchOffset <= lastBatchPartials);
      }
    }

    lastValuePartials = valuePartials;
    lastIndexPartials = indexPartials;
    lastBatchPartials = batchPartials;
    ++reduceIndex;
  }

  for (const auto &cs : reductionCS) {
    prog.add(Execute(cs));
  }

  indices = lastIndexPartials;
  return lastValuePartials;
}

Tensor topK(Graph &graph, const Tensor &input, Tensor &indices, unsigned K,
            bool sort, Sequence &prog, const std::string &debugPrefix) {

  if (input.rank() != 2) {
    throw poplibs_error("Topk: input tensor must be of rank 2");
  }

  if (input.elementType() != FLOAT && input.elementType() != INT &&
      input.elementType() != UNSIGNED_INT) {
    throw poplibs_error("TopK on input type is not supported");
  }

  if (input.dim(1) < K) {
    throw poplibs_error("K must be smaller or equal to the size of the "
                        "dimensions which the TopK is being calculated for.");
  }

  // TODO: map the tensor to which the output goes correctly
  unsigned numCorrectTile = 0;
  auto output = TopKImpl(graph, input, indices, K, sort, UNSIGNED_INT, prog,
                         numCorrectTile, debugPrefix);
  return output;
}

Tensor argMax(Graph &graph, const Tensor &input, Sequence &prog,
              const std::string &debugPrefix) {
  // TODO: map the tensor to which the output goes correctly
  unsigned numCorrectTile = 0;

  if (input.rank() != 2) {
    throw poplibs_error("input tensor must be of rank 2");
  }

  if (input.elementType() != FLOAT && input.elementType() != HALF &&
      input.elementType() != INT && input.elementType() != UNSIGNED_INT) {
    throw poplibs_error("arg max on input type is not supported");
  }
  auto output = argMinOrMax(graph, input, UNSIGNED_INT, prog, numCorrectTile,
                            debugPrefix);
  return output;
}

Tensor argMin(Graph &graph, const Tensor &input, Sequence &prog,
              const std::string &debugPrefix) {
  // TODO: map the tensor to which the output goes correctly
  unsigned numCorrectTile = 0;

  if (input.rank() != 2) {
    throw poplibs_error("input tensor must be of rank 2");
  }

  if (input.elementType() != FLOAT && input.elementType() != HALF &&
      input.elementType() != INT && input.elementType() != UNSIGNED_INT) {
    throw poplibs_error("arg min on input type is not supported");
  }
  auto output = argMinOrMax(graph, input, UNSIGNED_INT, prog, numCorrectTile,
                            debugPrefix, false);
  return output;
}

/// Compute the number of correct outputs against expected.
///
/// \param[in] graph        the graph for the tensor
/// \param[in] modelOutputs a 2D Tensor.
/// \param[in] expected     a 1D Tensor of integral type with the same number of
///                         elements as rows in 'modelOutputs'. Each element
///                         contains the index into the corresponding row of
///                         'modelOutputs' where we expect to find the maximum
///                         value for that row.
/// \param[out] numCorrect  a tensor containing a single element where this will
///                         place the result: the number of elements in
///                         'expected' that correctly indicate the max for their
//                          rows
/// \param[in] debugPrefix  as the name says
Program calcAccuracy(Graph &graph, const Tensor &modelOutputs,
                     const Tensor &expected, const Tensor &numCorrect,
                     const std::string &debugPrefix) {
  const auto layerPrefix = debugPrefix + "/Accuracy";
  // Normalize shape of numCorrect
  auto flatNumCorrect = numCorrect.flatten();
  if (flatNumCorrect.dim(0) != 1) {
    throw poplibs_error("numCorrect must be scalar or single element tensor");
  }
  const auto batchSize = modelOutputs.dim(0);
  if (expected.shape().size() > 1) {
    throw poplibs_error("expected must be a 1-dimensional tensor");
  }
  if (expected.dim(0) != batchSize) {
    throw poplibs_error("expected tensor must be of length equal the number of "
                        "batches given in modelOutputs tensor");
  }

  // Find out which tile `numCorrect` sits on
  auto numCorrectMapping = graph.getTileMapping(numCorrect);
  boost::optional<unsigned> numCorrectTile;
  for (const auto &tileMapping : numCorrectMapping) {
    if (!tileMapping.empty()) {
      assert(tileMapping.size() == 1 && tileMapping[0].size() == 1);
      numCorrectTile = tileMapping[0].begin();
    }
  }
  assert(numCorrectTile);

  // Get the indices of the max value of each row of 'modelOutput'
  Sequence prog;
  auto maxIndices = argMinOrMax(graph, modelOutputs, expected.elementType(),
                                prog, *numCorrectTile, layerPrefix);

  // This would ideally be calculated with a popops::eq followed by a
  // popops::reduceWithOutput. At the moment popops::eq outputs bool
  // so this requires some ugly casting. For now this last step is its
  // own vertex. Doesn't particularly matter while batch size is generally
  // so small.
  const auto calcAccuracyVertexClass =
      templateVertex("popnn::CalcAccuracy", expected.elementType());

  const auto calcAccuracyCS =
      graph.addComputeSet(layerPrefix + "/CalcAccuracy");
  auto v = graph.addVertex(calcAccuracyCS, calcAccuracyVertexClass,
                           {{"maxPerBatch", maxIndices.flatten()},
                            {"expected", expected},
                            {"numCorrect", flatNumCorrect[0]}});
  graph.setTileMapping(v, *numCorrectTile);
  // Add all the reductions and final accuracy.
  prog.add(Execute(calcAccuracyCS));

  return std::move(prog);
}

Program calcLoss(Graph &graph, const Tensor &modelOutputs,
                 const Tensor &expected, const Tensor &loss,
                 const Tensor &deltas, const Tensor &_deltasScale,
                 const Tensor &_modelOutputScaling, const Tensor &numCorrect,
                 LossType lossType, const std::string &debugPrefix) {
  boost::optional<Tensor> deltasScale = _deltasScale;
  boost::optional<Tensor> modelOutputScaling = _modelOutputScaling;
  Sequence prog(
      calcLoss(graph, modelOutputs, expected, loss, deltas, deltasScale,
               modelOutputScaling, lossType, debugPrefix),
      calcAccuracy(graph, modelOutputs, expected, numCorrect, debugPrefix));
  return prog;
}

Program calcLoss(Graph &graph, const Tensor &modelOutputs,
                 const Tensor &expected, const Tensor &loss,
                 const Tensor &deltas, const Tensor &numCorrect,
                 LossType lossType, const std::string &debugPrefix) {
  boost::optional<Tensor> deltasScale, modelOutputScaling;
  Sequence prog(
      calcLoss(graph, modelOutputs, expected, loss, deltas, deltasScale,
               modelOutputScaling, lossType, debugPrefix),
      calcAccuracy(graph, modelOutputs, expected, numCorrect, debugPrefix));
  return prog;
}

} // end namespace popnn
