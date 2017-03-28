/*
 * Pre-processor to refine signed distance mesh
 * this is a good way to increase the resolution 
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include "common/Array.h"
#include "common/Communication.h"
#include "common/Domain.h"
#include "common/pmmc.h"

int main(int argc, char **argv)
{
	// Initialize MPI
	int rank, nprocs;
	MPI_Init(&argc,&argv);
    MPI_Comm comm = MPI_COMM_WORLD;
	MPI_Comm_rank(comm,&rank);
	MPI_Comm_size(comm,&nprocs);

	{
	//.......................................................................
	// Reading the domain information file
	//.......................................................................
	int nprocx, nprocy, nprocz, nx, ny, nz, nspheres;
	double Lx, Ly, Lz;
	int i,j,k,n;
	int BC=0;

	if ( rank==0 ) {
		printf("lbpm_refine_pp: Refining signed distance mesh (x2) \n");
	}
	if (rank==0){
		ifstream domain("Domain.in");
		domain >> nprocx;
		domain >> nprocy;
		domain >> nprocz;
		domain >> nx;
		domain >> ny;
		domain >> nz;
		domain >> nspheres;
		domain >> Lx;
		domain >> Ly;
		domain >> Lz;

	}
	MPI_Barrier(comm);
	// Computational domain
	MPI_Bcast(&nx,1,MPI_INT,0,comm);
	MPI_Bcast(&ny,1,MPI_INT,0,comm);
	MPI_Bcast(&nz,1,MPI_INT,0,comm);
	MPI_Bcast(&nprocx,1,MPI_INT,0,comm);
	MPI_Bcast(&nprocy,1,MPI_INT,0,comm);
	MPI_Bcast(&nprocz,1,MPI_INT,0,comm);
	MPI_Bcast(&nspheres,1,MPI_INT,0,comm);
	MPI_Bcast(&Lx,1,MPI_DOUBLE,0,comm);
	MPI_Bcast(&Ly,1,MPI_DOUBLE,0,comm);
	MPI_Bcast(&Lz,1,MPI_DOUBLE,0,comm);
	//.................................................
	MPI_Barrier(comm);

	// Check that the number of processors >= the number of ranks
	if ( rank==0 ) {
		printf("Number of MPI ranks required: %i \n", nprocx*nprocy*nprocz);
		printf("Number of MPI ranks used: %i \n", nprocs);
		printf("Full domain size: %i x %i x %i  \n",nx*nprocx,ny*nprocy,nz*nprocz);
	}
	if ( nprocs < nprocx*nprocy*nprocz ){
		ERROR("Insufficient number of processors");
	}

	char LocalRankFilename[40];

	int rnx,rny,rnz;
	rnx=2*(nx-1)+1;
	rny=2*(ny-1)+1;
	rnz=2*(nz-1)+1;
	
	rnx=2*nx;
	rny=2*ny;
	rnz=2*nz;
	
	if (rank==0) printf("Refining mesh to %i x %i x %i \n",rnx,rny,rnz);

	int BoundaryCondition=0;
	Domain Dm(rnx,rny,rnz,rank,nprocx,nprocy,nprocz,Lx,Ly,Lz,BoundaryCondition);

	// Communication the halos
	const RankInfoStruct rank_info(rank,nprocx,nprocy,nprocz);
	fillHalo<double> fillData(comm,rank_info,rnx,rny,rnz,1,1,1,0,1);

	nx+=2; ny+=2; nz+=2;
	rnx+=2; rny+=2; rnz+=2;
	int N = nx*ny*nz;

	// Define communication sub-domain -- everywhere
	for (int k=0; k<rnz; k++){
		for (int j=0; j<rny; j++){
			for (int i=0; i<rnx; i++){
				n = k*rnx*rny+j*rnx+i;
				Dm.id[n] = 1;
			}
		}
	}
	Dm.CommInit(comm);
	
	DoubleArray SignDist(nx,ny,nz);
	// Read the signed distance from file
	sprintf(LocalRankFilename,"SignDist.%05i",rank);
	FILE *DIST = fopen(LocalRankFilename,"rb");
	size_t ReadSignDist;
	ReadSignDist=fread(SignDist.data(),8,N,DIST);
	if (ReadSignDist != size_t(N)) printf("lbpm_refine_pp: Error reading signed distance function (rank=%i)\n",rank);
	fclose(DIST);


	if ( rank==0 )   printf("Set up Domain, read input distance \n");

	DoubleArray RefinedSignDist(rnx,rny,rnz);	
	TriLinPoly LocalApprox;
	Point pt;

	int ri,rj,rk,rn; //refined mesh indices
	for (rk=1; rk<rnz-2; rk++){
	     for (rj=1; rj<rny-2; rj++){
		  for (ri=1; ri<rnx-2; ri++){
				n = rk*rnx*rny+rj*rnx+ri;
				// starting node for each processor matches exactly
				i = (ri-1)/2+1;
				j = (rj-1)/2+1;
				k = (rk-1)/2+1;

				//printf("(%i,%i,%i -> %i,%i,%i) \n",ri,rj,rk,i,j,k);
				// Assign local tri-linear polynomial
				LocalApprox.assign(SignDist,i,j,k); 
				pt.x=0.5*(ri-1)+1.f; 
				pt.y=0.5*(rj-1)+1.f; 
				pt.z=0.5*(rk-1)+1.f;
				RefinedSignDist(ri,rj,rk) = LocalApprox.eval(pt);
			}
		}
	}
	fillData.fill(RefinedSignDist);
	//	sprintf(LocalRankFilename,"ID.%05i",rank);
	//FILE *ID = fopen(LocalRankFilename,"wb");
	//fwrite(id,1,N,ID);
	//fclose(ID);

	sprintf(LocalRankFilename,"RefineDist.%05i",rank);
	FILE *REFINEDIST = fopen(LocalRankFilename,"wb");
	fwrite(RefinedSignDist.data(),8,rnx*rny*rnz,REFINEDIST);
	fclose(REFINEDIST);

        }
	MPI_Barrier(comm);
	MPI_Finalize();
	return 0;
}
