// Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <numeric>

#include "../../shared/arithmetic.h"
#include "kernel_launch.h"
#include "rtc_realcomplex_gen.h"
#include "rtc_realcomplex_kernel.h"
#include "tree_node.h"

RTCKernel::RTCGenerator RTCKernelRealComplex::generate_from_node(const TreeNode&    node,
                                                                 const std::string& gpu_arch,
                                                                 bool enable_callbacks)
{
    RTCGenerator generator;

    if(node.scheme != CS_KERNEL_COPY_R_TO_CMPLX && node.scheme != CS_KERNEL_COPY_CMPLX_TO_HERM
       && node.scheme != CS_KERNEL_COPY_HERM_TO_CMPLX && node.scheme != CS_KERNEL_COPY_CMPLX_TO_R)
    {
        return generator;
    }

    // input_size is the innermost dimension
    unsigned int input_size = node.length[0];
    // hermitian size is used for hermitian->complex copy
    if(node.scheme == CS_KERNEL_COPY_HERM_TO_CMPLX)
        input_size = node.outputLength[0] / 2 + 1;
    unsigned int batch          = node.batch;
    unsigned int high_dimension = std::accumulate(
        node.length.begin() + 1, node.length.end(), 1, std::multiplies<unsigned int>());
    unsigned int blocks = (input_size - 1) / LAUNCH_BOUNDS_R2C_C2R_KERNEL + 1;

    generator.gridDim  = {blocks, high_dimension, batch};
    generator.blockDim = {LAUNCH_BOUNDS_R2C_C2R_KERNEL, 1, 1};

    RealComplexSpecs specs{node.scheme,
                           node.length.size(),
                           node.precision,
                           node.inArrayType,
                           node.outArrayType,
                           enable_callbacks,
                           node.IsScalingEnabled()};

    generator.generate_name = [=]() { return realcomplex_rtc_kernel_name(specs); };

    generator.generate_src
        = [=](const std::string& kernel_name) { return realcomplex_rtc(kernel_name, specs); };

    generator.construct_rtckernel = [=](const std::string&       kernel_name,
                                        const std::vector<char>& code,
                                        dim3                     gridDim,
                                        dim3                     blockDim) {
        return std::unique_ptr<RTCKernel>(
            new RTCKernelRealComplex(kernel_name, code, gridDim, blockDim));
    };
    return generator;
}

RTCKernelArgs RTCKernelRealComplex::get_launch_args(DeviceCallIn& data)
{
    // explode lengths/strides out to pass to the kernel
    std::array<size_t, 3> kern_lengths{1, 1, 1};
    std::array<size_t, 4> kern_stride_in{1, 1, 1, 1};
    std::array<size_t, 4> kern_stride_out{1, 1, 1, 1};
    auto                  dim = data.node->length.size();

    std::copy(data.node->length.begin(), data.node->length.end(), kern_lengths.begin());
    std::copy(data.node->inStride.begin(), data.node->inStride.end(), kern_stride_in.begin());
    kern_stride_in[dim] = data.node->iDist;
    std::copy(data.node->outStride.begin(), data.node->outStride.end(), kern_stride_out.begin());
    kern_stride_out[dim] = data.node->oDist;

    RTCKernelArgs kargs;
    if(data.node->scheme == CS_KERNEL_COPY_HERM_TO_CMPLX)
    {
        // dim_0 is the innermost dimension
        kern_lengths[0]       = data.node->outputLength[0];
        size_t hermitian_size = kern_lengths[0] / 2 + 1;
        kargs.append_unsigned_int(hermitian_size);
    }
    kargs.append_unsigned_int(kern_lengths[0]);
    kargs.append_unsigned_int(kern_lengths[1]);
    kargs.append_unsigned_int(kern_lengths[2]);
    kargs.append_unsigned_int(kern_stride_in[0]);
    kargs.append_unsigned_int(kern_stride_in[1]);
    kargs.append_unsigned_int(kern_stride_in[2]);
    kargs.append_unsigned_int(kern_stride_in[3]);
    kargs.append_unsigned_int(kern_stride_out[0]);
    kargs.append_unsigned_int(kern_stride_out[1]);
    kargs.append_unsigned_int(kern_stride_out[2]);
    kargs.append_unsigned_int(kern_stride_out[3]);

    kargs.append_ptr(data.bufIn[0]);
    if(array_type_is_planar(data.node->inArrayType))
        kargs.append_ptr(data.bufIn[1]);
    kargs.append_ptr(data.bufOut[0]);
    if(array_type_is_planar(data.node->outArrayType))
        kargs.append_ptr(data.bufOut[1]);

    // callback params
    kargs.append_ptr(data.callbacks.load_cb_fn);
    kargs.append_ptr(data.callbacks.load_cb_data);
    kargs.append_unsigned_int(data.callbacks.load_cb_lds_bytes);
    kargs.append_ptr(data.callbacks.store_cb_fn);
    kargs.append_ptr(data.callbacks.store_cb_data);
    if(data.node->scheme == CS_KERNEL_COPY_CMPLX_TO_HERM
       || data.node->scheme == CS_KERNEL_COPY_CMPLX_TO_R)
    {
        if(data.node->precision == rocfft_precision_single)
            kargs.append_float(data.node->scale_factor);
        else
            kargs.append_double(data.node->scale_factor);
    }

    return kargs;
}

RTCKernel::RTCGenerator RTCKernelRealComplexEven::generate_from_node(const TreeNode&    node,
                                                                     const std::string& gpu_arch,
                                                                     bool enable_callbacks)
{
    RTCGenerator generator;

    if(node.scheme != CS_KERNEL_R_TO_CMPLX && node.scheme != CS_KERNEL_CMPLX_TO_R)
    {
        return generator;
    }

    // Input_size is the innermost dimension
    size_t half_N;
    if(node.scheme == CS_KERNEL_R_TO_CMPLX)
    {
        // The upper level provides always N/2, that is regular complex fft size
        half_N = node.length[0];
    }
    else
    {
        // Length on the node is complex fft size.  Compute half_N as
        // half of the real size.
        half_N = node.length[0] - 1;
    }
    const bool Ndiv4 = half_N % 2 == 0;

    const unsigned int batch = node.batch;

    const unsigned int high_dimension = std::accumulate(
        node.length.begin() + 1, node.length.end(), 1, std::multiplies<unsigned int>());

    const unsigned int block_size = LAUNCH_BOUNDS_R2C_C2R_KERNEL;
    const unsigned int blocks     = ((half_N + 1) / 2 + block_size - 1) / block_size;

    generator.gridDim  = {blocks, high_dimension, batch};
    generator.blockDim = {block_size, 1, 1};

    RealComplexEvenSpecs specs{{node.scheme,
                                node.length.size(),
                                node.precision,
                                node.inArrayType,
                                node.outArrayType,
                                enable_callbacks,
                                node.IsScalingEnabled()},
                               Ndiv4};

    generator.generate_name = [=]() { return realcomplex_even_rtc_kernel_name(specs); };

    generator.generate_src
        = [=](const std::string& kernel_name) { return realcomplex_even_rtc(kernel_name, specs); };

    generator.construct_rtckernel = [=](const std::string&       kernel_name,
                                        const std::vector<char>& code,
                                        dim3                     gridDim,
                                        dim3                     blockDim) {
        return std::unique_ptr<RTCKernel>(
            new RTCKernelRealComplexEven(kernel_name, half_N, code, gridDim, blockDim));
    };
    return generator;
}

RTCKernelArgs RTCKernelRealComplexEven::get_launch_args(DeviceCallIn& data)
{
    RTCKernelArgs kargs;

    kargs.append_unsigned_int(half_N);
    if(data.node->length.size() > 1)
    {
        kargs.append_unsigned_int(data.node->inStride[1]);
        kargs.append_unsigned_int(data.node->outStride[1]);
    }
    kargs.append_ptr(data.bufIn[0]);
    if(array_type_is_planar(data.node->inArrayType))
        kargs.append_ptr(data.bufIn[1]);
    kargs.append_unsigned_int(data.node->iDist);
    kargs.append_ptr(data.bufOut[0]);
    if(array_type_is_planar(data.node->outArrayType))
        kargs.append_ptr(data.bufOut[1]);
    kargs.append_unsigned_int(data.node->oDist);
    kargs.append_ptr(data.node->twiddles);
    // callback params
    kargs.append_ptr(data.callbacks.load_cb_fn);
    kargs.append_ptr(data.callbacks.load_cb_data);
    kargs.append_unsigned_int(data.callbacks.load_cb_lds_bytes);
    kargs.append_ptr(data.callbacks.store_cb_fn);
    kargs.append_ptr(data.callbacks.store_cb_data);
    if(data.node->IsScalingEnabled())
    {
        if(data.node->precision == rocfft_precision_single)
            kargs.append_float(data.node->scale_factor);
        else
            kargs.append_double(data.node->scale_factor);
    }

    return kargs;
}
