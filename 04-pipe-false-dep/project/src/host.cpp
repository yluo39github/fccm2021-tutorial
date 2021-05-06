/**
* Copyright (C) 2020 Xilinx, Inc
*
* Licensed under the Apache License, Version 2.0 (the "License"). You may
* not use this file except in compliance with the License. A copy of the
* License is located at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
* License for the specific language governing permissions and limitations
* under the License.
*/

#include "xcl2.hpp"
#include <algorithm>
#include <vector>
#define IMAGE_SIZE 4096
#define HISTOGRAM_SIZE 256

int main(int argc, char** argv) {
    if(argc != 2) {
        std::cout << "Usage: " << argv[0] << " <XCLBIN File>" << std::endl;
        return EXIT_FAILURE;
    }

    std::string binaryFile = argv[1];
    size_t image_size_bytes = IMAGE_SIZE;
    size_t histogram_size_bytes = sizeof(unsigned int) * HISTOGRAM_SIZE;
    cl_int err;
    cl::Context context;
    cl::Kernel krnl;
    cl::CommandQueue q;
    // Allocate Memory in Host Memory
    // When creating a buffer with user pointer (CL_MEM_USE_HOST_PTR), under the
    // hood user ptr
    // is used if it is properly aligned. when not aligned, runtime had no choice
    // but to create
    // its own host side buffer. So it is recommended to use this allocator if
    // user wish to
    // create buffer using CL_MEM_USE_HOST_PTR to align user buffer to page
    // boundary. It will
    // ensure that user buffer is used when user create Buffer/Mem object with
    // CL_MEM_USE_HOST_PTR
    std::vector<char, aligned_allocator<char> > source_image(IMAGE_SIZE);
    std::vector<unsigned int, aligned_allocator<unsigned int> > source_hist_hw(HISTOGRAM_SIZE);
    std::vector<unsigned int, aligned_allocator<unsigned int> > source_hist_sw(HISTOGRAM_SIZE);

    // Create the test data
    std::generate(source_image.begin(), source_image.end(), std::rand);
    for(int i = 0; i < HISTOGRAM_SIZE; i++) {
        source_hist_hw[i] = 0;
        source_hist_sw[i] = 0;
    }
    for(int i = 0; i < IMAGE_SIZE; i++)
        source_hist_sw[source_image[i]] += 1;

    // OPENCL HOST CODE AREA START
    // get_xil_devices() is a utility API which will find the xilinx
    // platforms and will return list of devices connected to Xilinx platform
    auto devices = xcl::get_xil_devices();
    // read_binary_file() is a utility API which will load the binaryFile
    // and will return the pointer to file buffer.
    auto fileBuf = xcl::read_binary_file(binaryFile);
    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    bool valid_device = false;
    for(unsigned int i = 0; i < devices.size(); i++) {
        auto device = devices[i];
        // Creating Context and Command Queue for selected Device
        OCL_CHECK(err, context = cl::Context(device, nullptr, nullptr, nullptr, &err));
        OCL_CHECK(err, q = cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err));
        std::cout << "Trying to program device[" << i << "]: " << device.getInfo<CL_DEVICE_NAME>() << std::endl;
        cl::Program program(context, {device}, bins, nullptr, &err);
        if(err != CL_SUCCESS) {
            std::cout << "Failed to program device[" << i << "] with xclbin file!\n";
        } else {
            std::cout << "Device[" << i << "]: program successful!\n";
            OCL_CHECK(err, krnl = cl::Kernel(program, "hist", &err));
            valid_device = true;
            break; // we break because we found a valid device
        }
    }
    if(!valid_device) {
        std::cout << "Failed to program any device found, exit!\n";
        exit(EXIT_FAILURE);
    }

    // Allocate Buffer in Global Memory
    // Buffers are allocated using CL_MEM_USE_HOST_PTR for efficient memory and
    // Device-to-host communication
    OCL_CHECK(err, cl::Buffer buffer_image(context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, image_size_bytes, source_image.data(), &err));
    OCL_CHECK(err, cl::Buffer buffer_hist(context, CL_MEM_USE_HOST_PTR, histogram_size_bytes, source_hist_hw.data(), &err));

    int size = IMAGE_SIZE;
    OCL_CHECK(err, err = krnl.setArg(0, buffer_image));
    OCL_CHECK(err, err = krnl.setArg(1, buffer_hist));
    OCL_CHECK(err, err = krnl.setArg(2, size));

    // Copy input data to device global memory
    OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_image, buffer_hist}, 0 /* 0 means from host*/));

    // Launch the Kernel
    // For HLS kernels global and local size is always (1,1,1). So, it is
    // recommended
    // to always use enqueueTask() for invoking HLS kernel
    OCL_CHECK(err, err = q.enqueueTask(krnl));

    // Copy Result from Device Global Memory to Host Local Memory
    OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_hist}, CL_MIGRATE_MEM_OBJECT_HOST));
    q.finish();
    // OPENCL HOST CODE AREA END

    // Compare the results of the Device to the simulation
    bool match = true;
    for(int i = 0; i < IMAGE_SIZE; i++) {
        if(source_hist_sw[i] != source_hist_hw[i]) {
            std::cout << "Error: Result mismatch" << std::endl;
            std::cout << "i = " << i << " CPU result = " << source_hist_sw[i]
                      << " Device result = " << source_hist_hw[i] << std::endl;
            match = false;
            break;
        }
    }

    std::cout << "TEST " << (match ? "PASSED" : "FAILED") << std::endl;
    return (match ? EXIT_SUCCESS : EXIT_FAILURE);
}
