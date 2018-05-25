/* ************************************************************************
 * Copyright 2018 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#include "rocsparse.h"
#include "definitions.h"
#include "handle.h"
#include "utility.h"
#include "coomv_device.h"
#include "ellmv_device.h"

#include <hip/hip_runtime.h>

template <typename T>
__global__ void ellmvn_kernel_host_pointer(rocsparse_int m,
                                           rocsparse_int n,
                                           rocsparse_int ell_width,
                                           T alpha,
                                           const rocsparse_int* ell_col_ind,
                                           const T* ell_val,
                                           const T* x,
                                           T beta,
                                           T* y,
                                           rocsparse_index_base idx_base)
{
    ellmvn_device(m, n, ell_width, alpha, ell_col_ind, ell_val, x, beta, y, idx_base);
}

template <typename T>
__global__ void ellmvn_kernel_device_pointer(rocsparse_int m,
                                             rocsparse_int n,
                                             rocsparse_int ell_width,
                                             const T* alpha,
                                             const rocsparse_int* ell_col_ind,
                                             const T* ell_val,
                                             const T* x,
                                             const T* beta,
                                             T* y,
                                             rocsparse_index_base idx_base)
{
    ellmvn_device(m, n, ell_width, *alpha, ell_col_ind, ell_val, x, *beta, y, idx_base);
}

template <typename T, rocsparse_int BLOCKSIZE, rocsparse_int WARPSIZE>
__global__ void coomvn_warp_host_pointer(rocsparse_int nnz,
                                         rocsparse_int loops,
                                         T alpha,
                                         const rocsparse_int* coo_row_ind,
                                         const rocsparse_int* coo_col_ind,
                                         const T* coo_val,
                                         const T* x,
                                         T* y,
                                         rocsparse_int* row_block_red,
                                         T* val_block_red,
                                         rocsparse_index_base idx_base)
{
    coomvn_general_warp_reduce<T, BLOCKSIZE, WARPSIZE>(nnz,
                                                       loops,
                                                       alpha,
                                                       coo_row_ind,
                                                       coo_col_ind,
                                                       coo_val,
                                                       x,
                                                       y,
                                                       row_block_red,
                                                       val_block_red,
                                                       idx_base);
}

template <typename T>
rocsparse_status rocsparse_hybmv_template(rocsparse_handle handle,
                                          rocsparse_operation trans,
                                          const T* alpha,
                                          const rocsparse_mat_descr descr,
                                          const rocsparse_hyb_mat hyb,
                                          const T* x,
                                          const T* beta,
                                          T* y)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(hyb == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging TODO bench logging
    if(handle->pointer_mode == rocsparse_pointer_mode_host)
    {
        log_trace(handle,
                  replaceX<T>("rocsparse_Xhybmv"),
                  trans,
                  *alpha,
                  (const void*&)descr,
                  (const void*&)hyb,
                  (const void*&)x,
                  *beta,
                  (const void*&)y);
    }
    else
    {
        log_trace(handle,
                  replaceX<T>("rocsparse_Xhybmv"),
                  trans,
                  (const void*&)alpha,
                  (const void*&)descr,
                  (const void*&)hyb,
                  (const void*&)x,
                  (const void*&)beta,
                  (const void*&)y);
    }

    // Check index base
    if(descr->base != rocsparse_index_base_zero && descr->base != rocsparse_index_base_one)
    {
        return rocsparse_status_invalid_value;
    }
    // Check matrix type
    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }
// TODO check partition type
//    if(hyb->partition != rocsparse_hyb_partition_max)
//    {
//        return rocsparse_status_not_implemented;
//    }

    // Check sizes
    if(hyb->m < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(hyb->n < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(hyb->ell_nnz + hyb->coo_nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    // Check ELL-HYB structure
    if(hyb->ell_nnz > 0)
    {
        if(hyb->ell_width < 0)
        {
            return rocsparse_status_invalid_size;
        }
        else if(hyb->ell_col_ind == nullptr)
        {
            return rocsparse_status_invalid_pointer;
        }
        else if(hyb->ell_val == nullptr)
        {
            return rocsparse_status_invalid_pointer;
        }
    }

    // Check COO-HYB structure
    if(hyb->coo_nnz > 0)
    {
        if(hyb->coo_row_ind == nullptr)
        {
            return rocsparse_status_invalid_pointer;
        }
        else if(hyb->coo_col_ind == nullptr)
        {
            return rocsparse_status_invalid_pointer;
        }
        else if(hyb->coo_val == nullptr)
        {
            return rocsparse_status_invalid_pointer;
        }
    }

    // Check pointer arguments
    if(x == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(y == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(alpha == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(beta == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Quick return if possible
    if(hyb->m == 0 || hyb->n == 0 || hyb->ell_nnz + hyb->coo_nnz == 0)
    {
        return rocsparse_status_success;
    }

    // Stream
    hipStream_t stream = handle->stream;

    // Run different hybmv kernels
    if(trans == rocsparse_operation_none)
    {
#define ELLMVN_DIM 512
        dim3 ellmvn_blocks((hyb->m - 1) / ELLMVN_DIM + 1);
        dim3 ellmvn_threads(ELLMVN_DIM);

        if(handle->pointer_mode == rocsparse_pointer_mode_device)
        {
        }
        else
        {
            if(*alpha == 0.0 && *beta == 1.0)
            {
                return rocsparse_status_success;
            }

            // ELL part
            if(hyb->ell_nnz > 0)
            {
                hipLaunchKernelGGL((ellmvn_kernel_host_pointer<T>),
                                   ellmvn_blocks,
                                   ellmvn_threads,
                                   0,
                                   stream,
                                   hyb->m,
                                   hyb->n,
                                   hyb->ell_width,
                                   *alpha,
                                   hyb->ell_col_ind,
                                   (T*)hyb->ell_val,
                                   x,
                                   *beta,
                                   y,
                                   descr->base);
            }

            // COO part
            if(hyb->coo_nnz > 0)
            {
// TODO
#define COOMVN_DIM 128
        rocsparse_int maxthreads = handle->properties.maxThreadsPerBlock;
        rocsparse_int nprocs     = handle->properties.multiProcessorCount;
        rocsparse_int maxblocks  = (nprocs * maxthreads - 1) / COOMVN_DIM + 1;
        rocsparse_int minblocks  = (hyb->coo_nnz - 1) / COOMVN_DIM + 1;

        rocsparse_int nblocks = maxblocks < minblocks ? maxblocks : minblocks;
        rocsparse_int nwarps  = nblocks * (COOMVN_DIM / handle->warp_size);
        rocsparse_int nloops  = (hyb->coo_nnz / handle->warp_size + 1) / nwarps + 1;

        dim3 coomvn_blocks(nblocks);
        dim3 coomvn_threads(COOMVN_DIM);

        rocsparse_int* row_block_red = NULL;
        T* val_block_red             = NULL;

        RETURN_IF_HIP_ERROR(hipMalloc((void**)&row_block_red, sizeof(rocsparse_int) * nwarps));
        RETURN_IF_HIP_ERROR(hipMalloc((void**)&val_block_red, sizeof(T) * nwarps));

                hipLaunchKernelGGL((coomvn_warp_host_pointer<T, COOMVN_DIM, 64>),
                                   coomvn_blocks,
                                   coomvn_threads,
                                   0,
                                   stream,
                                   hyb->coo_nnz,
                                   nloops,
                                   *alpha,
                                   hyb->coo_row_ind,
                                   hyb->coo_col_ind,
                                   hyb->coo_val,
                                   x,
                                   y,
                                   row_block_red,
                                   val_block_red,
                                   descr->base);

        hipLaunchKernelGGL((coomvn_general_block_reduce<T, COOMVN_DIM>),
                           dim3(1),
                           coomvn_threads,
                           0,
                           stream,
                           nwarps,
                           row_block_red,
                           val_block_red,
                           y);

        RETURN_IF_HIP_ERROR(hipFree(row_block_red));
        RETURN_IF_HIP_ERROR(hipFree(val_block_red));
#undef COOMVN_DIM








            }
        }
#undef ELLMVN_DIM
    }
    else
    {
        // TODO
        return rocsparse_status_not_implemented;
    }
    return rocsparse_status_success;
}

/*
 * ===========================================================================
 *    C wrapper
 * ===========================================================================
 */

extern "C" rocsparse_status rocsparse_shybmv(rocsparse_handle handle,
                                             rocsparse_operation trans,
                                             const float* alpha,
                                             const rocsparse_mat_descr descr,
                                             const rocsparse_hyb_mat hyb,
                                             const float* x,
                                             const float* beta,
                                             float* y)
{
    return rocsparse_hybmv_template(handle, trans, alpha, descr, hyb, x, beta, y);
}

extern "C" rocsparse_status rocsparse_dhybmv(rocsparse_handle handle,
                                             rocsparse_operation trans,
                                             const double* alpha,
                                             const rocsparse_mat_descr descr,
                                             const rocsparse_hyb_mat hyb,
                                             const double* x,
                                             const double* beta,
                                             double* y)
{
    return rocsparse_hybmv_template(handle, trans, alpha, descr, hyb, x, beta, y);
}
