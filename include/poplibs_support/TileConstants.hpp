// Copyright (c) 2018, Graphcore Ltd, All rights reserved.

#ifndef poplibs_support_TileConstants_hpp
#define poplibs_support_TileConstants_hpp

// TODO: include tilearch.h and tileconstants.h and replace all uses of
// those headers with this header
#define NUM_WORKERS                      (6)

#define CONV_UNIT_INPUT_LOAD_ELEMS_FLOAT (1)
#define CONV_UNIT_INPUT_LOAD_ELEMS_HALF  (4)


// end namespace poplibs

#endif // poplibs_support_TileConstants_hpp