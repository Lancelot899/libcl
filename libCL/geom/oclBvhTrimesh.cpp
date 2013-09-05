// Copyright [2011] [Geist Software Labs Inc.]
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <math.h>

#include "oclBvhTrimesh.h"

const size_t oclBvhTrimesh::cWarpSize = 32;

oclBvhTrimesh::oclBvhTrimesh(oclContext& iContext, oclProgram* iParent)
  : oclProgram(iContext, "oclBvhTrimesh", iParent)
    // buffers
  , bfAABB(iContext, "bfAABB")
  , bfMortonKey(iContext, "bfMortonKey")
  , bfMortonVal(iContext, "bfMortonVal")
  , bfBvhRoot(iContext, "bfBvhRoot")
  , bfBvhNode(iContext, "bfBvhNode")
    // kernels
  , clAABB(*this, "clAABB")
  , clMorton(*this, "clMorton")
  , clCreateNodes(*this, "clCreateNodes")
  , clLinkNodes(*this, "clLinkNodes")
  , clCreateLeaves(*this, "clCreateLeaves")
  , clComputeAABBs(*this, "clComputeAABBs")
    // programs
  , mRadixSort(iContext, this)
    // members
  , mRootNode(0)
{
    bfAABB.create<srtAABB>(CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, 1);
    bfMortonKey.create<cl_uint>(CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, 256);
    bfMortonVal.create<cl_uint>(CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, 256);
    bfBvhNode.create<BVHNode>(CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, 1);
    bfBvhRoot.create<cl_uint>(CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, 1,  &mRootNode);

    addSourceFile("geom/oclBvhTrimesh.cl");
}

oclBvhTrimesh::~oclBvhTrimesh()
{
}

int oclBvhTrimesh::compute(oclDevice& iDevice, oclBuffer& bfVertex, oclBuffer& bfIndex)
{
    if (strcmp(mContext.getVendor(),oclContext::VENDOR_AMD) == 0){
        return false;
    }
    
    cl_uint lVertices = bfVertex.count<cl_float4>();
    size_t lTriangles = bfIndex.count<unsigned int>()/3;

    if (bfMortonKey.count<cl_uint>() != lTriangles)
    {
        bfMortonKey.resize<cl_uint>(lTriangles);
    }
    if (bfMortonVal.count<cl_uint>() != lTriangles)
    {
        bfMortonVal.resize<cl_uint>(lTriangles);
    }
    if (bfBvhNode.count<BVHNode>() != 2*lTriangles-1)
    {
		Log(INFO) << 2*lTriangles-1;
        bfBvhNode.resize<BVHNode>(2*lTriangles-1);
		Log(INFO) << bfBvhNode.count<BVHNode>();
    }


    //
    // compute global AABB
    //
    size_t lBatchSize = ceil(ceil(lVertices/8.0)/cWarpSize)*cWarpSize;
    clSetKernelArg(clAABB, 0, sizeof(cl_mem), bfVertex);
    clSetKernelArg(clAABB, 1, sizeof(cl_mem), bfAABB);
    clSetKernelArg(clAABB, 2, sizeof(cl_uint), &lVertices);
    sStatusCL = clEnqueueNDRangeKernel(
        iDevice, 
        clAABB, 
        1, 
        NULL, 
        &lBatchSize, 
        &cWarpSize, 
        0, 
        NULL, 
        clAABB.getEvent());
    ENQUEUE_VALIDATE


    //
    // Compute morton curve
    //	 
    clSetKernelArg(clMorton, 0, sizeof(cl_mem), bfIndex);
    clSetKernelArg(clMorton, 1, sizeof(cl_mem), bfVertex);
    clSetKernelArg(clMorton, 2, sizeof(cl_mem), bfMortonKey);
    clSetKernelArg(clMorton, 3, sizeof(cl_mem), bfMortonVal);
    clSetKernelArg(clMorton, 4, sizeof(cl_mem), bfAABB);
    sStatusCL = clEnqueueNDRangeKernel(iDevice, clMorton, 1, NULL, &lTriangles, NULL, 0, NULL, clMorton.getEvent());
    ENQUEUE_VALIDATE

    // 
    // Sort morton curve
    //	
    if (!mRadixSort.compute(iDevice, bfMortonKey, bfMortonVal, 0, 32))
    {
        return 0;
    }

    //
    // Create BVH
    //	
    size_t lGlobalSize = lTriangles-1;

    clSetKernelArg(clCreateNodes, 0, sizeof(cl_mem), bfMortonKey);
    clSetKernelArg(clCreateNodes, 1, sizeof(cl_mem), bfMortonVal);
    clSetKernelArg(clCreateNodes, 2, sizeof(cl_mem), bfBvhNode);
    sStatusCL = clEnqueueNDRangeKernel(iDevice, clCreateNodes, 1, NULL, &lGlobalSize, NULL, 0, NULL, clCreateNodes.getEvent());
    ENQUEUE_VALIDATE

    clSetKernelArg(clLinkNodes, 0, sizeof(cl_mem), bfMortonKey);
    clSetKernelArg(clLinkNodes, 1, sizeof(cl_mem), bfBvhNode);
    clSetKernelArg(clLinkNodes, 2, sizeof(cl_mem), bfBvhRoot);
    sStatusCL = clEnqueueNDRangeKernel(iDevice, clLinkNodes, 1, NULL, &lGlobalSize, NULL, 0, NULL, clLinkNodes.getEvent());
    ENQUEUE_VALIDATE

    clSetKernelArg(clCreateLeaves, 0, sizeof(cl_mem), bfMortonVal);
    clSetKernelArg(clCreateLeaves, 1, sizeof(cl_mem), bfBvhNode);
    sStatusCL = clEnqueueNDRangeKernel(iDevice, clCreateLeaves, 1, NULL, &lTriangles, NULL, 0, NULL, clCreateLeaves.getEvent());
    ENQUEUE_VALIDATE


    clSetKernelArg(clComputeAABBs, 0, sizeof(cl_mem), bfIndex);
    clSetKernelArg(clComputeAABBs, 1, sizeof(cl_mem), bfVertex);
    clSetKernelArg(clComputeAABBs, 2, sizeof(cl_mem), bfBvhNode);
    clSetKernelArg(clComputeAABBs, 3, sizeof(cl_mem), bfBvhRoot);
    sStatusCL = clEnqueueNDRangeKernel(iDevice, clComputeAABBs, 1, 
                                       NULL, &lTriangles, NULL, 0, NULL, 
                                       clComputeAABBs.getEvent());
    ENQUEUE_VALIDATE

		/*
	if (bfBvhNode.map(CL_MAP_READ))
	{
		BVHNode* lPtr = bfBvhNode.ptr<BVHNode>();

		for (int i=0; i<2*lTriangles-1; i++)
		{
			Log(INFO) << i << " " << lPtr[i].bbMin << "," << lPtr[i].bbMax << " :: " << lPtr[i].left << "," << lPtr[i].right << "::" << lPtr[i].trav << "::" << lPtr[i].bit;
		}
	}
	Log(INFO) << "TRIANGLES " << lTriangles;
		*/

    bfBvhRoot.map(CL_MAP_READ);
    bfBvhRoot.unmap();

    return true;
}
//

cl_uint oclBvhTrimesh::getRootNode()
{
    return mRootNode;
};

oclBuffer& oclBvhTrimesh::getNodeBuffer()
{
    return bfBvhNode;
};

cl_uint oclBvhTrimesh::getNodeCount()
{
    return bfBvhNode.count<BVHNode>();
};
