
#include <stdlib.h>
#include <cuda.h>
// #include <cuda_runtime.h>
#include <stdio.h>
#include <inttypes.h>

#include <math.h>

#include "cct.hpp"

#include "main.hpp"
// #include "update.hpp"
// #include "cuStinger.hpp"
#include "modified.hpp"


using namespace std;

#define CUDA(call, ...) do {                        \
        cudaError_t _e = (call);                    \
        if (_e == cudaSuccess) break;               \
        fprintf(stdout,                             \
                "CUDA runtime error: %s (%d)\n",    \
                cudaGetErrorString(_e), _e);        \
        return -1;                                  \
    } while (0)

void callDeviceNewTriangles(cuStinger& custing, BatchUpdate& bu,
    triangle_t * const __restrict__ outPutTriangles, const int threads_per_block,
    const int number_blocks, const int shifter, const int thread_blocks, const int blockdim);

void initHostTriangleArray(triangle_t* h_triangles, vertexId_t nv){	
	for(vertexId_t sd=0; sd<(nv);sd++){
		h_triangles[sd]=0;
	}
}

//RNG using Lehmer's Algorithm
#define RNG_A 16807
#define RNG_M 2147483647
#define RNG_Q 127773
#define RNG_R 2836
#define RNG_SCALE (1.0 / RNG_M)

// Seed can always be changed manually
static int seed = 1;
double getRand(){
    
    int k = seed / RNG_Q;
    seed = RNG_A * (seed - k * RNG_Q) - k * RNG_R;
    
    if (seed < 0) {
        seed += RNG_M;
    }
    
    return seed * (double) RNG_SCALE;
}

// Search a value in a range of sorted values
template<typename T>
T* search(T* start, int32_t size, T value)
{
	if(size == 1 && start[0] != value) {
		return NULL;
	}

	if(start[size/2] > value) {
		return search(start, size/2, value);
	}
	else if(start[size/2] < value) {
		return search(start+size/2, ceil((float)size/2), value);
	}
	else if(start[size/2] == value) {
		return start+size/2;
	}
	else {
		return NULL;
	}
}

void printcuStingerUtility(cuStinger custing, bool allInfo){
	length_t used,allocated;

	used     =custing.getNumberEdgesUsed();
	allocated=custing.getNumberEdgesAllocated();
	if (allInfo)
		cout << ", " << used << ", " << allocated << ", " << (float)used/(float)allocated; 	
	else
		cout << ", " << (float)used/(float)allocated;

}

void generateEdgeUpdates(length_t nv, length_t numEdges, vertexId_t* edgeSrc, vertexId_t* edgeDst){
	for(int32_t e=0; e<numEdges; e++){
		edgeSrc[e] = rand()%nv;
		edgeDst[e] = rand()%nv;
	}
}

typedef struct dxor128_env {
  unsigned x,y,z,w;
} dxor128_env_t;


// double dxor128(dxor128_env_t * e);
// void dxor128_init(dxor128_env_t * e);
// void dxor128_seed(dxor128_env_t * e, unsigned seed);
void rmat_edge (int64_t * iout, int64_t * jout, int SCALE, double A, double B, double C, double D, dxor128_env_t * env);

void generateEdgeUpdatesRMAT(length_t nv, length_t numEdges, vertexId_t* edgeSrc, vertexId_t* edgeDst,double A, double B, double C, double D, dxor128_env_t * env){
	int64_t src,dst;
	int scale = (int)log2(double(nv));
	for(int32_t e=0; e<numEdges; e++){
		rmat_edge(&src,&dst,scale, A,B,C,D,env);
		edgeSrc[e] = src;
		edgeDst[e] = dst;
	}
}


int main(const int argc, char *argv[])
{
	int device=0;
    cudaSetDevice(device);
	cudaDeviceProp prop;
	cudaGetDeviceProperties(&prop, device);
 
    length_t nv, ne,*off;
    vertexId_t *adj;
    int isRmat=0;
	int numEdges=10000;
	if(argc>2)
		numEdges=atoi(argv[2]);
	if(argc>3)
		isRmat  =atoi(argv[3]);
	srand(100);
	bool isDimacs,isSNAP;
	string filename(argv[1]);
	isDimacs = filename.find(".graph")==std::string::npos?false:true;
	isSNAP   = filename.find(".txt")==std::string::npos?false:true;

	if(isDimacs){
	    readGraphDIMACS(argv[1],&off,&adj,&nv,&ne);
	}
	else if(isSNAP){
	    readGraphSNAP(argv[1],&off,&adj,&nv,&ne);
	}
	else{ 
		cout << "Unknown graph type" << endl;
	}
	cout << nv << ", " << ne;

	cudaEvent_t ce_start,ce_stop;

	cuStinger custing2(defaultInitAllocater,defaultUpdateAllocater);


	cuStingerInitConfig cuInit;
	cuInit.initState =eInitStateCSR;
	cuInit.maxNV = nv+1;
	cuInit.useVWeight = false;
	cuInit.isSemantic = false;  // Use edge types and vertex types
	cuInit.useEWeight = false;

	// CSR data
	cuInit.csrNV 			= nv;
	cuInit.csrNE	   		= ne;
	cuInit.csrOff 			= off;
	cuInit.csrAdj 			= adj;
	cuInit.csrVW 			= NULL;
	cuInit.csrEW			= NULL;

	start_clock(ce_start, ce_stop);
	custing2.initializeCuStinger(cuInit);
	// cout << "Allocation and Copy Time : " << end_clock(ce_start, ce_stop) << endl;
	cout << ", " << end_clock(ce_start, ce_stop);

	printcuStingerUtility(custing2, false);

	triangle_t *d_triangles = NULL;
	CUDA(cudaMalloc(&d_triangles, sizeof(triangle_t)*(nv+1)));
	triangle_t* h_triangles = (triangle_t *) malloc (sizeof(triangle_t)*(nv+1));	
	initHostTriangleArray(h_triangles,nv);

	int tsp = 4; // Threads per intersection
	int shifter = 2; // left shift to multiply threads per intersection
	int sps = 128; // Block size
	int nbl = sps/tsp; // Number of concurrent intersections in block
	int blocks = 16000; // Number of blocks

	CUDA(cudaMemcpy(d_triangles, h_triangles, sizeof(triangle_t)*(nv+1), cudaMemcpyHostToDevice));
	callDeviceAllTriangles(custing2, d_triangles, tsp,nbl,shifter,blocks, sps);
	CUDA(cudaMemcpy(h_triangles, d_triangles, sizeof(triangle_t)*(nv+1), cudaMemcpyDeviceToHost));

	length_t numEdgesL = numEdges;
	BatchUpdateData bud(numEdgesL,true);
	if(isRmat){
		double a = 0.55, b = 0.15, c = 0.15,d = 0.25;
		dxor128_env_t env;// dxor128_seed(&env, 0);
		generateEdgeUpdatesRMAT(nv, numEdges, bud.getSrc(),bud.getDst(),a,b,c,d,&env);
	}
	else{	
		generateEdgeUpdates(nv, numEdges, bud.getSrc(),bud.getDst());
	}
	BatchUpdate bu(bud);

	custing2.checkDuplicateEdges();
	custing2.verifyEdgeInsertions(bu);

	start_clock(ce_start, ce_stop);
		custing2.edgeInsertions(bu);
	// cout << "Update time     : " << end_clock(ce_start, ce_stop) << endl;
	cout << ", " << end_clock(ce_start, ce_stop);

	/*
	
	#  #           #   #      #    #             #        #  #
	####           #         # #                 #        #  #
	####   ##    ###  ##     #    ##     ##    ###        #  #
	#  #  #  #  #  #   #    ###    #    # ##  #  #        #  #
	#  #  #  #  #  #   #     #     #    ##    #  #         ##
	#  #   ##    ###  ###    #    ###    ##    ###         ##
	============================================================================
	*/

	vertexModification(bu, nv, custing2);

	// ###                                  #          ###          #
	// #  #                                 #           #
	// #  #   ##    ##    ##   #  #  ###   ###          #    ###   ##
	// ###   # ##  #     #  #  #  #  #  #   #           #    #  #   #
	// # #   ##    #     #  #  #  #  #  #   #           #    #      #
	// #  #   ##    ##    ##    ###  #  #    ##         #    #     ###

	triangle_t *d_triangles_new = NULL;
	CUDA(cudaMalloc(&d_triangles_new, sizeof(triangle_t)*(nv+1)));
	triangle_t* h_triangles_new = (triangle_t *) malloc (sizeof(triangle_t)*(nv+1));	
	initHostTriangleArray(h_triangles_new,nv);

	CUDA(cudaMemcpy(d_triangles_new, h_triangles_new, sizeof(triangle_t)*(nv+1), cudaMemcpyHostToDevice));
	callDeviceNewTriangles(custing2, bu, d_triangles_new, tsp,nbl,shifter,blocks, sps);
	CUDA(cudaMemcpy(h_triangles_new, d_triangles_new, sizeof(triangle_t)*(nv+1), cudaMemcpyDeviceToHost));

	// =========================================================================


	custing2.checkDuplicateEdges();	
	custing2.verifyEdgeInsertions(bu);

	printcuStingerUtility(custing2, false);

	start_clock(ce_start, ce_stop);
		custing2.edgeDeletions(bu);
	cout << ", " << end_clock(ce_start, ce_stop);
	// cout << "Update time     : " << end_clock(ce_start, ce_stop) << endl;
	custing2.verifyEdgeDeletions(bu);

	printcuStingerUtility(custing2, false);

	custing2.freecuStinger();

	free(off);
	free(adj);
    return 0;	
}       



void rmat_edge (int64_t * iout, int64_t * jout, int SCALE, double A, double B, double C, double D, dxor128_env_t * env)
{
  int64_t i = 0, j = 0;
  int64_t bit = ((int64_t) 1) << (SCALE - 1);

  while (1) {
    const double r =  ((double) rand() / (RAND_MAX));//dxor128(env);
    if (r > A) {                /* outside quadrant 1 */
      if (r <= A + B)           /* in quadrant 2 */
        j |= bit;
      else if (r <= A + B + C)  /* in quadrant 3 */
        i |= bit;
      else {                    /* in quadrant 4 */
        j |= bit;
        i |= bit;
      }
    }
    if (1 == bit)
      break;

    /*
      Assuming R is in (0, 1), 0.95 + 0.1 * R is in (0.95, 1.05).
      So the new probabilities are *not* the old +/- 10% but
      instead the old +/- 5%.
    */
    A *= (9.5 + ((double) rand() / (RAND_MAX))) / 10;
    B *= (9.5 + ((double) rand() / (RAND_MAX))) / 10;
    C *= (9.5 + ((double) rand() / (RAND_MAX))) / 10;
    D *= (9.5 + ((double) rand() / (RAND_MAX))) / 10;
    /* Used 5 random numbers. */

    {
      const double norm = 1.0 / (A + B + C + D);
      A *= norm;
      B *= norm;
      C *= norm;
    }
    /* So long as +/- are monotonic, ensure a+b+c+d <= 1.0 */
    D = 1.0 - (A + B + C);

    bit >>= 1;
  }
  /* Iterates SCALE times. */
  *iout = i;
  *jout = j;
}


double dxor128(dxor128_env_t * e) {
  unsigned t=e->x^(e->x<<11);
  e->x=e->y; e->y=e->z; e->z=e->w; e->w=(e->w^(e->w>>19))^(t^(t>>8));
  return e->w*(1.0/4294967296.0);
}

void dxor128_init(dxor128_env_t * e) {
  e->x=123456789;
  e->y=362436069;
  e->z=521288629;
  e->w=88675123;
}

void dxor128_seed(dxor128_env_t * e, unsigned seed) {
  e->x=123456789;
  e->y=362436069;
  e->z=521288629;
  e->w=seed;
}