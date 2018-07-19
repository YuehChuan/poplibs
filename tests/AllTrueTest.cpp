#define BOOST_TEST_MODULE AllTrueTest
#include <popops/AllTrue.hpp>
#include <poputil/exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <poplar/Engine.hpp>
#include <popops/codelets.hpp>
#include "TestDevice.hpp"

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace popops;

#define DIM_SIZE 4

bool allTrueTest(bool in[DIM_SIZE]) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device);
  popops::addCodelets(graph);

  Tensor tIn = graph.addVariable(BOOL, {DIM_SIZE}, "t1");
  graph.setTileMapping(tIn, 0);

  auto seq = Sequence();
  const auto tOut = allTrue(graph, tIn, seq, "");

  graph.createHostWrite("in", tIn);
  graph.createHostRead("out", tOut);

  bool out;
  Engine eng(graph, seq);
  eng.load(device);
  eng.writeTensor("in", in);
  eng.run();
  eng.readTensor("out", &out);
  return out;
}

BOOST_AUTO_TEST_CASE(AllTrue) {
  bool in[DIM_SIZE] = {true, true, true, true};
  BOOST_CHECK_EQUAL(allTrueTest(in), true);
}

BOOST_AUTO_TEST_CASE(NotAllTrue) {
  bool in[DIM_SIZE] = {true, false, false, true};
  BOOST_CHECK_EQUAL(allTrueTest(in), false);
}