#include "popstd/SubtractFrom.hpp"

#include "popstd/Add.hpp"
#include "popstd/Util.hpp"
#include "popstd/VertexTemplates.hpp"

using namespace poplar;
using namespace poplar::program;

namespace popstd {

  void subtractFrom(Graph &graph, Tensor A, Tensor B, float k,
             Sequence &prog, const std::string &debugPrefix) {

    addTo(graph, A, B, -k, prog, debugPrefix);
  }

  void subtractFrom(Graph &graph, Tensor A, Tensor B,
               Sequence &prog, const std::string &debugPrefix) {

    subtractFrom(graph, A, B, 1.0, prog, debugPrefix);
  }

}
