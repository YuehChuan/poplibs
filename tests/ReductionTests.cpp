#define BOOST_TEST_MODULE ReductionTests

#include <boost/test/unit_test.hpp>
#include <popstd/TileMapping.hpp>
#include <poplar/Engine.hpp>
#include <poplar/HalfFloat.hpp>
#include <popstd/codelets.hpp>
#include <popreduce/codelets.hpp>
#include <popreduce/Reduce.hpp>
#include <poplib_test/Util.hpp>
#include <iostream>
#include <functional>
#include <limits>
#include <boost/multi_array.hpp>

// Tolerances used in tests
#define FLOAT_REL_TOL  0.01
#define HALF_REL_TOL   0.1
#define FLOAT_ABS_TOL  1e-6
#define HALF_ABS_TOL   1e-5

using namespace poplar;
using namespace poplar::program;
using namespace popstd;
using namespace popreduce;
using namespace poplib_test::util;

namespace utf = boost::unit_test;
namespace fpc = boost::test_tools::fpc;

// Initialise value for a given type of computation
static double initValue(popreduce::Operation operation) {
  double val = 0;
  switch (operation) {
  case popreduce::Operation::ADD:
    val = 0;
    break;
  case popreduce::Operation::MUL:
    val = 1.0;
    break;
  case popreduce::Operation::MIN:
    val = std::numeric_limits<double>::max();
    break;
  case popreduce::Operation::MAX:
    val = std::numeric_limits<double>::min();
    break;
  case popreduce::Operation::AND:
    val = 1.0;
    break;
  case popreduce::Operation::OR:
    val = 0.0;
    break;
  }
  return val;
}

// Perform a binary operation
static double doComputation(double x, double y, popreduce::Operation comp) {
  double res = y;
  switch (comp) {
  case popreduce::Operation::ADD:
    res += x;
    break;
  case popreduce::Operation::MUL:
    res *= x;
    break;
  case popreduce::Operation::MIN:
    res = std::min(res, x);
    break;
  case popreduce::Operation::MAX:
    res = std::max(res, x);
    break;
  case popreduce::Operation::AND:
    res = res && x;
    break;
  case popreduce::Operation::OR:
    res = res || x;
    break;
  }
  return res;
}

// Reduce across given dimensions. The general case where there is no
// restriction on outDims is not coded yet (i.e. dimensions to reduce
// must be less than the number of dimensions to reduce
static void reduceTensor(boost::multi_array_ref<double, 3> in,
                         boost::multi_array_ref<double, 1> out,
                         const std::vector<std::size_t> &outDims,
                         popreduce::Operation operation) {

  if (outDims.size() == 3) {
    for (auto i2 = 0U; i2 != in.shape()[2]; ++i2) {
      for (auto i1 = 0U; i1 != in.shape()[1]; ++i1) {
        for (auto i0 = 0U; i0 != in.shape()[0]; ++i0) {
          out[i0 * in.shape()[1] * in.shape()[2] + i1 * in.shape()[2] + i2] =
            in[i0][i1][i2];
        }
      }
    }
  } else if (outDims.size() == 2) {
    for (auto i2 = 0U; i2 != in.shape()[2]; ++i2) {
      for (auto i1 = 0U; i1 != in.shape()[1]; ++i1) {
        auto res = initValue(operation);
        for (auto i0 = 0U; i0 != in.shape()[0]; ++i0) {
          res = doComputation(in[i0][i1][i2], res, operation);
        }
        out[i1 * in.shape()[2] + i2] = res;
      }
    }
  } else if (outDims.size() == 1) {
    for (auto i2 = 0U; i2 != in.shape()[2]; ++i2) {
      auto res = initValue(operation);
      for (auto i1 = 0U; i1 != in.shape()[1]; ++i1) {
        for (auto i0 = 0U; i0 != in.shape()[0]; ++i0) {
          res = doComputation(in[i0][i1][i2], res, operation);
        }
      }
      out[i2] = res;
    }
  } else if (outDims.size() == 0) {
    double res = initValue(operation);
    for (auto i2 = 0U; i2 != in.shape()[2]; ++i2) {
      for (auto i1 = 0U; i1 != in.shape()[1]; ++i1) {
        for (auto i0 = 0U; i0 != in.shape()[0]; ++i0) {
          res = doComputation(in[i0][i1][i2], res, operation);
        }
      }
    }
    out[0] = res;
  }
}

// Reduce across 1st dimension
static void reduceTensor(boost::multi_array_ref<double, 2> in,
                         boost::multi_array_ref<double, 1> out) {
  assert(in.shape()[1] == out.shape()[0]);
  std::size_t rows = in.shape()[0];
  std::size_t cols = in.shape()[1];
  for (auto c = 0U; c != cols; ++c) {
    double sum = 0;
    for (auto r = 0U; r != rows; ++r) {
      sum += in[r][c];
    }
    out[c] = sum;
  }
}

static bool reduceAddTest(const std::vector<std::size_t> &dims,
                          const std::string &partialsTypeStr,
                          const std::string &outTypeStr,
                          float k,
                          bool update,
                          bool scale) {
  Graph graph(createIPUModelDevice());
  popstd::addCodelets(graph);
  popreduce::addCodelets(graph);

  assert(!(scale && update));
  assert(dims.size() == 2);

  auto in = graph.addTensor(partialsTypeStr, {dims}, "in");
  popstd::mapTensorLinearly(graph, in);

  auto prev = graph.addTensor(outTypeStr, {dims[1]}, "prev");
  popstd::mapTensorLinearly(graph, prev);

  auto prog = Sequence();
  Tensor out;

  if (scale) {
    out = popreduce::reduceScale(graph, k, in, outTypeStr, prog);
  } else if (update) {
    out = graph.clone(prev);
    prog.add(Copy(prev, out));
    popreduce::reduceAcc(graph, out, k, in, prog);
  } else {
    out = popreduce::reduce(graph, in, prog);
  }

  std::vector<std::pair<std::string, char *>> tmap;
  auto rawHostPrev =
          allocateHostMemoryForTensor(prev, "prev", graph, tmap);
  auto rawHostIn =
          allocateHostMemoryForTensor(in, "in", graph, tmap);
  auto rawHostOut =
          allocateHostMemoryForTensor(out, "out", graph, tmap);

  boost::multi_array<double, 1>
      hostPrev(boost::extents[dims[1]]);
  boost::multi_array<double, 2>
      hostIn(boost::extents[dims[0]][dims[1]]);
  boost::multi_array<double, 1>
      hostOut(boost::extents[dims[1]]);

  std::mt19937 randomEngine;
  std::fill(hostOut.data(), hostOut.data() + hostOut.num_elements(), 0);
  writeRandomValues(hostPrev, -1.0, +5.0, randomEngine);
  writeRandomValues(hostIn, 1.5, 1.6, randomEngine);

  copy(hostOut, outTypeStr, rawHostOut.get());
  copy(hostPrev, outTypeStr, rawHostPrev.get());
  copy(hostIn, partialsTypeStr, rawHostIn.get());

  Engine engine(graph, prog);

  upload(engine, tmap);
  engine.run(0); // Run.
  download(engine, tmap);

  copy(outTypeStr, rawHostOut.get(), hostOut);

  boost::multi_array<double, 1>
      modelReduced(boost::extents[dims[1]]);

  reduceTensor(hostIn, modelReduced);

  boost::multi_array<double, 1>
      modelOut(boost::extents[dims[1]]);

  double kp = 0, kn = 1.0;
  if (scale) {
    kn = k;
  } else if (update) {
    kp = 1.0; kn = k;
  }

  for (auto c = 0U; c != dims[1]; ++c) {
    modelOut[c] = kp * hostPrev[c] + kn * modelReduced[c];
  }

  const double absoluteTolerance = outTypeStr == "float" ? FLOAT_ABS_TOL :
                                                           HALF_ABS_TOL;
  const double relativeTolerance = outTypeStr == "float" ? FLOAT_REL_TOL :
                                                           HALF_REL_TOL;

  auto matchesModel = checkIsClose("out", hostOut, modelOut,
                                   relativeTolerance, absoluteTolerance);
  return matchesModel;
}

static bool reduceOpsTest(const std::vector<std::size_t> &dims,
                          const std::vector<std::size_t> &redVect,
                          const std::string &outTypeStr,
                          popreduce::Operation operation) {
  Graph graph(createIPUModelDevice());
  popstd::addCodelets(graph);
  popreduce::addCodelets(graph);

  assert(dims.size() == 3);
  assert(redVect.size() <= 3);

  auto in = graph.addTensor(outTypeStr, {dims}, "in");
  popstd::mapTensorLinearly(graph, in);

  auto prog = Sequence();
  Tensor out =  popreduce::reduce(graph, in, redVect, operation, prog);

  std::vector<std::pair<std::string, char *>> tmap;
  auto rawHostIn =
          allocateHostMemoryForTensor(in, "in", graph, tmap);
  auto rawHostOut =
          allocateHostMemoryForTensor(out, "out", graph, tmap);

  // check reduction dimensions: restricted set allowed
#ifndef NDEBUG
  for (const auto i : redVect) {
    assert(i < redVect.size());
  }
#endif

  // find output dims
  std::vector<std::size_t> outDims;
  std::size_t numOutElements = 1ULL;
  for (std::size_t i = 0; i != in.rank(); ++i) {
    if (std::find(redVect.begin(), redVect.end(), i)
        == redVect.end()) {
      numOutElements *= in.dim(i);
      outDims.push_back(i);
    }
  }

  boost::multi_array<double, 3>
      hostIn(boost::extents[dims[0]][dims[1]][dims[2]]);

  // keep flattened outputs
  boost::multi_array<double, 1> hostOut(boost::extents[numOutElements]);

  std::mt19937 randomEngine;
  std::fill(hostOut.data(), hostOut.data() + hostOut.num_elements(), 0);
  writeRandomValues(hostIn, -2, 2, randomEngine);

  if (outTypeStr == "bool") {
    for (auto it = hostIn.data(); it != hostIn.data() + hostIn.num_elements();
          ++it) {
      *it = *it <= 0 ? 0 : 1;
    }
  }

  copy(hostOut, outTypeStr, rawHostOut.get());
  copy(hostIn, outTypeStr, rawHostIn.get());

  Engine engine(graph, prog);

  upload(engine, tmap);
  engine.run(0); // Run.
  download(engine, tmap);

  copy(outTypeStr, rawHostOut.get(), hostOut);

  boost::multi_array<double, 1>
      modelReduced(boost::extents[numOutElements]);

  reduceTensor(hostIn, modelReduced, outDims, operation);

  const double absoluteTolerance = outTypeStr == "float" ? FLOAT_ABS_TOL :
                                                           HALF_ABS_TOL;
  const double relativeTolerance = outTypeStr == "float" ? FLOAT_REL_TOL :
                                                           HALF_REL_TOL;

  auto matchesModel = checkIsClose("out", hostOut, modelReduced,
                                   relativeTolerance, absoluteTolerance);
  return matchesModel;
}

BOOST_AUTO_TEST_CASE(Reduce_100x100_float_float_noupdate) {
  auto matchesModel = reduceAddTest({100, 100}, "float", "float",
                                     1.0, false, false);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_10x200_half_half) {
  auto matchesModel = reduceAddTest({10, 200}, "half", "half",
                                     2.0, false, false);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_31x201_scale_half_half) {
  auto matchesModel = reduceAddTest({31, 201}, "half", "half",
                                     3.0, false, true);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_31x201_scale_float_half) {
  auto matchesModel = reduceAddTest({31, 201}, "float", "half",
                                    -1.5, false, true);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_1x201_scale_float_half) {
  auto matchesModel = reduceAddTest({1, 201}, "float", "half",
                                    -1.5, false, true);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_1x201_scale_half_half) {
  auto matchesModel = reduceAddTest({1, 201}, "half", "half",
                                    -1.5, false, true);
  BOOST_TEST(matchesModel == true);
}


BOOST_AUTO_TEST_CASE(Reduce_31x201_update_float_float) {
  auto matchesModel = reduceAddTest({31, 201}, "float", "float",
                                    -1.5, true, false);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_31x201_update_half_half) {
  auto matchesModel = reduceAddTest({31, 201}, "half", "half",
                                    2.0, true, false);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_31x201_update_float_half) {
  auto matchesModel = reduceAddTest({31, 201}, "float", "half",
                                    -1.5, true, false);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_Add_float) {
  auto matchesModel = reduceOpsTest({10, 20, 30}, {0}, "float",
                                    popreduce::Operation::ADD);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_Mul_float) {
  auto matchesModel = reduceOpsTest({33, 22, 11}, {0}, "float",
                                    popreduce::Operation::MUL);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_Max_half) {
  auto matchesModel = reduceOpsTest({20, 30, 40}, {0, 1}, "half",
                                    popreduce::Operation::MAX);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_Min_float) {
  auto matchesModel = reduceOpsTest({20, 30, 40}, {0, 1}, "float",
                                    popreduce::Operation::MIN);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_And_bool) {
  auto matchesModel = reduceOpsTest({20, 30, 40}, {0, 1}, "bool",
                                    popreduce::Operation::AND);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_Or_bool) {
  auto matchesModel = reduceOpsTest({20, 30, 40}, {0, 1}, "bool",
                                    popreduce::Operation::OR);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_All_ADD_float) {
  auto matchesModel = reduceOpsTest({20, 30, 11}, {1, 0, 2}, "float",
                                    popreduce::Operation::ADD);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_None_ADD_float) {
  auto matchesModel = reduceOpsTest({20, 30, 11}, {}, "float",
                                    popreduce::Operation::ADD);
  BOOST_TEST(matchesModel == true);
}

BOOST_AUTO_TEST_CASE(Reduce_Skip_ADD_float) {
  auto matchesModel = reduceOpsTest({1, 1, 11}, {0, 1}, "float",
                                    popreduce::Operation::ADD);
  BOOST_TEST(matchesModel == true);
}