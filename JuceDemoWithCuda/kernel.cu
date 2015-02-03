#include "cuda_runtime.h"
#include "device_launch_parameters.h"

#include <stdio.h>
#include <math.h>
#include <string.h> //for memset
#include <assert.h>
#include <stdlib.h> //for atexit

#include "defines.h"

#define CIRCULAR_BUFFER_LEN MAX_DELAY_EFFECT_LENGTH

// When summing the outputs at a specific frame for each partial, we use a reduction method.
// This reduction method requires a temporary array in shared memory.
__shared__ float partialReductionOutputs[NUM_PARTIALS*NUM_CH];

// this is a circular buffer of sample data (interleaved by channel number) stored on the device
// It is persistent and lengthy, in order to accomodate the delay effect.
float *d_sampleBuffer = NULL;

void checkCudaError(cudaError_t e) {
	if (e != cudaSuccess) {
		printf("Cuda Error: %s\n", cudaGetErrorString(e));
		printf("Aborting\n");
		exit(1);
	}
}

bool _hasCudaDevice() {
	int deviceCount;
	cudaError_t err = cudaGetDeviceCount(&deviceCount);
	// if we get a cuda error, it may be because the system has no cuda dlls.
	bool useCuda = (err == cudaSuccess && deviceCount != 0);
	printf("Using Cuda? %i\n", useCuda);
	return useCuda;
}

bool hasCudaDevice() {
	//only check for the presence of a device once.
	static bool hasDevice = _hasCudaDevice();
	return hasDevice;
}

// code to run at shutdown (free buffers, etc)
void teardown() {
	// free the sample buffer if we allocated it and it hasn't already been freed.
	if (d_sampleBuffer != NULL) {
		if (hasCudaDevice()) {
			checkCudaError(cudaFree(d_sampleBuffer));
		} else {
			free(d_sampleBuffer);
		}
		// avoid double-frees
		d_sampleBuffer = NULL;
	}
}

// code to run on first-time audio calculation
void startup() {
	atexit(&teardown);
	if (hasCudaDevice()) {
		// allocate sample buffer on device
		checkCudaError(cudaMalloc(&d_sampleBuffer, CIRCULAR_BUFFER_LEN*NUM_CH*sizeof(float)));
	} else {
		// allocate sample buffer on cpu
		d_sampleBuffer = (float*)malloc(CIRCULAR_BUFFER_LEN*NUM_CH*sizeof(float));
	}
}

void doStartupOnce() {
	static bool hasInit = false;
	if (!hasInit) {
		startup();
		hasInit = true;
	}
}


__device__ __host__ void reduceOutputs(float *buffer, unsigned partialIdx, int sampleIdx, float outputL, float outputR) {
	//algorithm: given 8 outputs, [0, 1, 2, 3, 4, 5, 6, 7]
	//first iteration: 4 active threads. 
	//  Thread 0 adds i0 to i(0+4). Thread 1 adds i1 to i(1+4). Thread 2 adds i2 to i(2+4). Thread 3 adds i3 to i(3+4)
	//  Output now: [4, 6, 8, 10,   4, 5, 6, 7]
	//second iteration: 2 active threads.
	//  Thread 0 adds i0 to i(0+2). Thread 1 adds i1 to i(1+2)
	//  Output now: [12, 16,   8, 10, 4, 5, 6, 7]
	//third iteration: 1 active thread.
	//  Thread 0 adds i0 to i(0+1).
	//  Output now: [28,   16, 8, 10, 4, 5, 6, 7]
	//fourth iteration: 0 active threads -> exit
	unsigned bufferIdx = NUM_CH * (sampleIdx % CIRCULAR_BUFFER_LEN);
	#ifdef __CUDA_ARCH__
		//device code
		partialReductionOutputs[NUM_CH*partialIdx + 0] = outputL;
		partialReductionOutputs[NUM_CH*partialIdx + 1] = outputR;
		unsigned numActiveThreads = NUM_PARTIALS / 2;
		while (numActiveThreads > 0) {
			__syncthreads();
			partialReductionOutputs[NUM_CH*partialIdx + 0] += partialReductionOutputs[NUM_CH*partialIdx + numActiveThreads*NUM_CH + 0];
			partialReductionOutputs[NUM_CH*partialIdx + 1] += partialReductionOutputs[NUM_CH*partialIdx + numActiveThreads*NUM_CH + 1];
			numActiveThreads /= 2;
		}
		if (partialIdx == 0) {
			buffer[bufferIdx + 0] = partialReductionOutputs[0];
			buffer[bufferIdx + 1] = partialReductionOutputs[1];
		}
	#else
		//host code
		//Since everything's computed iteratively, we can just add our outputs directly to the buffer.
		//First write to this sample must zero-initialize the buffer (not required in the GPU code).
		if (partialIdx == 0) {
			buffer[bufferIdx + 0] = 0;
			buffer[bufferIdx + 1] = 0;
		}
		buffer[bufferIdx + 0] += outputL;
		buffer[bufferIdx + 1] += outputR;
	#endif
}


__device__ __host__ void computePartialOutput(float *buffer, unsigned baseIdx, unsigned partialIdx, float fundamentalFreq) {
	float angleDelta = fundamentalFreq * INV_SAMPLE_RATE * (partialIdx+1);
	for (int sampleIdx = 0; sampleIdx < BUFFER_BLOCK_SIZE; ++sampleIdx) {
		float outputL, outputR;
		outputL = outputR = (1.0/NUM_PARTIALS)*sinf((baseIdx + sampleIdx) * angleDelta);
		reduceOutputs(buffer, partialIdx, baseIdx+sampleIdx, outputL, outputR);
	}
}

__global__ void fillSineWaveKernel(float *buffer, unsigned baseIdx, float fundamentalFreq) {
	int partialNum = threadIdx.x;
	computePartialOutput(buffer, baseIdx, partialNum, fundamentalFreq);
}

__host__ void fillSineWaveOnCpu(float *bufferB, unsigned sampleIdx, float fundamentalFreq) {
	for (int partialIdx = 0; partialIdx < NUM_PARTIALS; ++partialIdx) {
		computePartialOutput(d_sampleBuffer, sampleIdx, partialIdx, fundamentalFreq);
	}
	unsigned bufferStartIdx = NUM_CH * (sampleIdx % CIRCULAR_BUFFER_LEN);
	memcpy(bufferB, &d_sampleBuffer[bufferStartIdx], BUFFER_BLOCK_SIZE*NUM_CH*sizeof(float));
}

__host__ void fillSineWaveCuda(float *bufferB, unsigned sampleIdx, float fundamentalFreq) {
	fillSineWaveKernel << <1, NUM_PARTIALS >> >(d_sampleBuffer, sampleIdx, fundamentalFreq);

	checkCudaError(cudaGetLastError()); //check if error in kernel launch
	checkCudaError(cudaDeviceSynchronize()); //check for error INSIDE the kernel

	//copy memory into the cpu buffer
	//Note: this will wait for the kernel to complete first.
	unsigned bufferStartIdx = NUM_CH * (sampleIdx % CIRCULAR_BUFFER_LEN);
	checkCudaError(cudaMemcpy(bufferB, &d_sampleBuffer[bufferStartIdx], BUFFER_BLOCK_SIZE*NUM_CH*sizeof(float), cudaMemcpyDeviceToHost));
}

void fillSineWaveVoice(float *bufferB, unsigned baseIdx, float fundamentalFreq) {
	doStartupOnce();
	if (hasCudaDevice()) {
		fillSineWaveCuda(bufferB, baseIdx, fundamentalFreq);
	} else {
		fillSineWaveOnCpu(bufferB, baseIdx, fundamentalFreq);
	}
}
