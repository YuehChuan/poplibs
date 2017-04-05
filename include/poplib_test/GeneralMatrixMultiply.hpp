#ifndef _poplib_test_gemm_hpp_
#define _poplib_test_gemm_hpp_

#include<boost/multi_array.hpp>

namespace poplib_test {
namespace gemm {

/*
 * Computes matC = alpha * op(matA) .* op(matB)
 *
 * where .* is an element-wise multiplication
 *
 * where op(matA) = A     if transposeA = false
 *       op(matA) = A'    if transposeA = true
 *
 *
 *       op(matB) = B     if transposeB = false
 *       op(matB) = B'    if transposeB = true
 *
 * Matrix dimensions of op(A) must be equal to op(B)
 */
void hadamardProduct(const boost::multi_array_ref<double, 2> matA,
                     const boost::multi_array_ref<double, 2> matB,
                     boost::multi_array_ref<double, 2> matC,
                     float alpha = 1.0,
                     bool  transposeA = false,
                     bool  transposeB = false);

/*
 * Computes matC = alpha * op(vecA) .* op(vecB)
 *
 * where .* is an element-wise multiplication
 *
 * where op(matA) = A     if transposeA = false
 *       op(matA) = A'    if transposeA = true
 *
 *
 *       op(matB) = B     if transposeB = false
 *       op(matB) = B'    if transposeB = true
 *
 * Vector dimensions of A must be equal to B
 */
void hadamardProduct(const boost::multi_array_ref<double, 1> matA,
                     const boost::multi_array_ref<double, 1> matB,
                     boost::multi_array_ref<double, 1> matC,
                     float alpha = 1.0);

/*
 * Computes matD = beta * matC + alpha * op(matA) * matB
 *
 * where matB, matC and matD are one dimensional matrices
 *
 * where op(matA) = A     if transposeA = false
 *       op(matA) = A'    if transposeA = true
 *
 */
void generalMatrixMultiply(const boost::multi_array_ref<double, 2> matA,
                           const boost::multi_array_ref<double, 1> vecB,
                           const boost::multi_array_ref<double, 1> vecC,
                           boost::multi_array_ref<double, 1> vecD,
                           float alpha = 1.0,
                           float beta = 1.0,
                           bool  transposeA = false);

/*
 * Computes matD = beta * matC + alpha * op(matA) * op(matB)
 *
 * where op(matA) = A     if transposeA = false
 *       op(matA) = A'    if transposeA = true
 *
 *
 *       op(matB) = B     if transposeB = false
 *       op(matB) = B'    if transposeB = true
 *
 */
void generalMatrixMultiply(const boost::multi_array_ref<double, 2> matA,
                           const boost::multi_array_ref<double, 2> matB,
                           const boost::multi_array_ref<double, 2> matC,
                           boost::multi_array_ref<double, 2> matD,
                           float alpha = 1.0,
                           float beta = 1.0,
                           bool  transposeA = false,
                           bool  transposeB = false);
} // End namespace gemm.
} // End namespace ref.

#endif  // _poplib_test_FullyConnected_hpp_
