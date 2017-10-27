#define BOOST_TEST_MODULE StdOperatorTest
#include <popstd/CircBuf.hpp>
#include <popstd/TileMapping.hpp>
#include <popstd/exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <limits>
#include <poplar/Engine.hpp>
#include <popstd/codelets.hpp>
#include <iostream>

using namespace poplar;
using namespace poplar::program;
using namespace popstd;

BOOST_AUTO_TEST_CASE(CircBufIncrIndex) {
  Graph graph(createIPUModelDevice());
  popstd::addCodelets(graph);
  const unsigned circBufSize = 20;
  const unsigned indexBufSize = 25;
  auto cb = CircBuf(graph, "float", circBufSize, {1});
  auto indexStore = graph.addTensor("unsigned", {indexBufSize});
  mapTensorLinearly(graph, indexStore);
  auto dummy = graph.addTensor("float", {1});
  mapTensorLinearly(graph, dummy);

  auto prog = Sequence();
  for (auto i = 0U; i != indexBufSize; ++i) {
    prog.add(Copy(cb.getIndex(), indexStore[i]));
    cb.add(dummy, prog);
  }
  graph.createHostRead("out", indexStore);

  unsigned cbOut[indexBufSize];

  Engine eng(graph, prog);
  eng.run();
  eng.readTensor("out", cbOut);

  for (unsigned i = 0; i != indexBufSize; ++i) {
    BOOST_TEST(i % circBufSize == cbOut[i]);
  }
}

BOOST_AUTO_TEST_CASE(CircBufIncrIndex2d) {
  Graph graph(createIPUModelDevice());
  popstd::addCodelets(graph);
  const unsigned circBufSize = 20;
  const unsigned indexBufSize = 25;
  auto cb = CircBuf(graph, "float", circBufSize, {5, 3});
  auto indexStore = graph.addTensor("unsigned", {indexBufSize});
  mapTensorLinearly(graph, indexStore);
  auto dummy = graph.addTensor("float", {5, 3});
  mapTensorLinearly(graph, dummy);

  auto prog = Sequence();
  for (auto i = 0U; i != indexBufSize; ++i) {
    prog.add(Copy(cb.getIndex(), indexStore[i]));
    cb.add(dummy, prog);
  }
  graph.createHostRead("out", indexStore);

  unsigned cbOut[indexBufSize];

  Engine eng(graph, prog);
  eng.run();
  eng.readTensor("out", cbOut);

  for (unsigned i = 0; i != indexBufSize; ++i) {
    BOOST_TEST(i % circBufSize == cbOut[i]);
  }
}

BOOST_AUTO_TEST_CASE(CircBufCheckAdd) {
  DeviceInfo info;
  info.tilesPerIPU = 16;

  Graph graph(createIPUModelDevice(info));
  popstd::addCodelets(graph);
  const unsigned circBufSize = 20;
  const unsigned srcBufSize = 25;
  const unsigned numElemsA = 33, numElemsB = 2;
  auto cb = CircBuf(graph, "float", circBufSize, {numElemsA, numElemsB});

  auto src = graph.addTensor("float", {srcBufSize, numElemsA, numElemsB});
  mapTensorLinearly(graph, src);
  auto dst = graph.addTensor("float", {circBufSize, numElemsA, numElemsB});
  mapTensorLinearly(graph, dst);


  auto prog = Sequence();
  for (auto i = 0U; i != srcBufSize; ++i) {
    cb.add(src[i], prog);
  }

 for (auto i = 0U; i != circBufSize; ++i) {
    prog.add(Copy(cb.prev(i, prog), dst[i]));
  }

  graph.createHostWrite("in", src);
  graph.createHostRead("out", dst);

  float cbSrc[srcBufSize][numElemsA][numElemsB];
  float cbDst[circBufSize][numElemsA][numElemsB];

  for (auto s = 0U; s != srcBufSize; ++s) {
    for (auto r = 0U; r != numElemsA; ++r) {
      for (auto c = 0U; c != numElemsB; ++c) {
        cbSrc[s][r][c] = 1000 * s + 10 * r + c;
      }
    }
  }

  Engine eng(graph, prog);
  eng.writeTensor("in", cbSrc);
  eng.run();
  eng.readTensor("out", cbDst);

  for (unsigned i = 0; i != circBufSize; ++i) {
    for (unsigned j = 0; j != numElemsA; ++j) {
      for (unsigned k = 0; k != numElemsB; ++k) {
        BOOST_TEST(cbDst[i][j][k] == (srcBufSize - 1 - i) * 1000 + j * 10 + k);
      }
    }
  }
}
