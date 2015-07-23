#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <fstream>

#include "ScaLBL.h"
#include "Communication.h"
#include "TwoPhase.h"
#include "common/MPI_Helpers.h"

//#define WRITE_SURFACES

/*
 * Simulator for two-phase flow in porous media
 * James E. McClure 2013-2014
 */

using namespace std;

//*************************************************************************
// Implementation of Two-Phase Immiscible LBM using CUDA
//*************************************************************************
inline void PackID(int *list, int count, char *sendbuf, char *ID){
	// Fill in the phase ID values from neighboring processors
	// This packs up the values that need to be sent from one processor to another
	int idx,n;

	for (idx=0; idx<count; idx++){
		n = list[idx];
		sendbuf[idx] = ID[n];
	}
}
//***************************************************************************************

inline void UnpackID(int *list, int count, char *recvbuf, char *ID){
	// Fill in the phase ID values from neighboring processors
	// This unpacks the values once they have been recieved from neighbors
	int idx,n;

	for (idx=0; idx<count; idx++){
		n = list[idx];
		ID[n] = recvbuf[idx];
	}
}

//***************************************************************************************

inline void ZeroHalo(double *Data, int Nx, int Ny, int Nz)
{
	int i,j,k,n;
	for (k=0;k<Nz;k++){
		for (j=0;j<Ny;j++){
			i=0;
			n = k*Nx*Ny+j*Nx+i;
			Data[2*n] = 0.0;
			Data[2*n+1] = 0.0;
			i=Nx-1;
			n = k*Nx*Ny+j*Nx+i;
			Data[2*n] = 0.0;
			Data[2*n+1] = 0.0;
		}
	}

	for (k=0;k<Nz;k++){
		for (i=0;i<Nx;i++){
			j=0;
			n = k*Nx*Ny+j*Nx+i;
			Data[2*n] = 0.0;
			Data[2*n+1] = 0.0;
			j=Ny-1;
			n = k*Nx*Ny+j*Nx+i;
			Data[2*n] = 0.0;
			Data[2*n+1] = 0.0;
		}
	}

	for (j=0;j<Ny;j++){
		for (i=0;i<Nx;i++){
			k=0;
			n = k*Nx*Ny+j*Nx+i;
			Data[2*n] = 0.0;
			Data[2*n+1] = 0.0;
			k=Nz-1;
			n = k*Nx*Ny+j*Nx+i;
			Data[2*n] = 0.0;
			Data[2*n+1] = 0.0;
		}
	}
}
//***************************************************************************************


int main(int argc, char **argv)
{
	//*****************************************
	// ***** MPI STUFF ****************
	//*****************************************
	// Initialize MPI
	int rank,nprocs;
	MPI_Init(&argc,&argv);
	MPI_Comm_rank(MPI_COMM_WORLD,&rank);
	MPI_Comm_size(MPI_COMM_WORLD,&nprocs);
	// parallel domain size (# of sub-domains)
	int nprocx,nprocy,nprocz;
	int iproc,jproc,kproc;
	int sendtag,recvtag;
	//*****************************************
	// MPI ranks for all 18 neighbors
	//**********************************
	int rank_x,rank_y,rank_z,rank_X,rank_Y,rank_Z;
	int rank_xy,rank_XY,rank_xY,rank_Xy;
	int rank_xz,rank_XZ,rank_xZ,rank_Xz;
	int rank_yz,rank_YZ,rank_yZ,rank_Yz;
	//**********************************
	MPI_Request req1[18],req2[18];
	MPI_Status stat1[18],stat2[18];

	if (rank == 0){
		printf("********************************************************\n");
		printf("Running Color LBM	\n");
		printf("********************************************************\n");
	}

	// Variables that specify the computational domain  
	string FILENAME;
	unsigned int nBlocks, nthreads;
	int Nx,Ny,Nz;		// local sub-domain size
	int nspheres;		// number of spheres in the packing
	double Lx,Ly,Lz;	// Domain length
	double D = 1.0;		// reference length for non-dimensionalization
	// Color Model parameters
	int timestepMax, interval;
	double tau,Fx,Fy,Fz,tol,err;
	double alpha, beta;
	double das, dbs, phi_s;
	double din,dout;
	double wp_saturation;
	int BoundaryCondition;
	int InitialCondition;
//	bool pBC,Restart;
	int i,j,k,n;

	// pmmc threshold values
	double fluid_isovalue,solid_isovalue;
	fluid_isovalue = 0.0;
	solid_isovalue = 0.0;
	
	int RESTART_INTERVAL=20000;
	
	if (rank==0){
		//.............................................................
		//		READ SIMULATION PARMAETERS FROM INPUT FILE
		//.............................................................
		ifstream input("Color.in");
		// Line 1: Name of the phase indicator file (s=0,w=1,n=2)
//		input >> FILENAME;
		// Line 2: domain size (Nx, Ny, Nz)
//		input >> Nz;				// number of nodes (x,y,z)
//		input >> nBlocks;
//		input >> nthreads;
		// Line 3: model parameters (tau, alpha, beta, das, dbs)
		input >> tau;			// Viscosity parameter
		input >> alpha;			// Surface Tension parameter
		input >> beta;			// Width of the interface
		input >> phi_s;			// value of phi at the solid surface
//		input >> das;
//		input >> dbs;
		// Line 4: wetting phase saturation to initialize
		input >> wp_saturation;
		// Line 5: External force components (Fx,Fy, Fz)
		input >> Fx;
		input >> Fy;
		input >> Fz;
		// Line 6: Pressure Boundary conditions
		input >> InitialCondition;
		input >> BoundaryCondition;
		input >> din;
		input >> dout;
		// Line 7: time-stepping criteria
		input >> timestepMax;		// max no. of timesteps
		input >> interval;			// restart interval
		input >> tol;				// error tolerance
		//.............................................................

		//.......................................................................
		// Reading the domain information file
		//.......................................................................
		ifstream domain("Domain.in");
		domain >> nprocx;
		domain >> nprocy;
		domain >> nprocz;
		domain >> Nx;
		domain >> Ny;
		domain >> Nz;
		domain >> nspheres;
		domain >> Lx;
		domain >> Ly;
		domain >> Lz;
		//.......................................................................
		
	}
	// **************************************************************
	// Broadcast simulation parameters from rank 0 to all other procs
	MPI_Barrier(MPI_COMM_WORLD);
	//.................................................
	MPI_Bcast(&tau,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&alpha,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&beta,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&das,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&dbs,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&phi_s,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&wp_saturation,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&BoundaryCondition,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&InitialCondition,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&din,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&dout,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&Fx,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&Fy,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&Fz,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&timestepMax,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&interval,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&tol,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	// Computational domain
	MPI_Bcast(&Nx,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&Ny,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&Nz,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&nprocx,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&nprocy,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&nprocz,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&nspheres,1,MPI_INT,0,MPI_COMM_WORLD);
	MPI_Bcast(&Lx,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&Ly,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	MPI_Bcast(&Lz,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
	//.................................................
	MPI_Barrier(MPI_COMM_WORLD);
	
	RESTART_INTERVAL=interval;
	// **************************************************************
	// **************************************************************
	double Ps = -(das-dbs)/(das+dbs);
	double rlxA = 1.f/tau;
	double rlxB = 8.f*(2.f-rlxA)/(8.f-rlxA);
	double xIntPos;
	xIntPos = log((1.0+phi_s)/(1.0-phi_s))/(2.0*beta); 	
	
	// Set the density values inside the solid based on the input value phi_s
 	das = (phi_s+1.0)*0.5;
	dbs = 1.0 - das;
	
	if (nprocs != nprocx*nprocy*nprocz){
		printf("nprocx =  %i \n",nprocx);
		printf("nprocy =  %i \n",nprocy);
		printf("nprocz =  %i \n",nprocz);
		INSIST(nprocs == nprocx*nprocy*nprocz,"Fatal error in processor count!");
	}

	if (rank==0){
		printf("********************************************************\n");
		printf("tau = %f \n", tau);
		printf("alpha = %f \n", alpha);		
		printf("beta = %f \n", beta);
		printf("das = %f \n", das);
		printf("dbs = %f \n", dbs);
		printf("gamma_{wn} = %f \n", 5.796*alpha);
		printf("Force(x) = %f \n", Fx);
		printf("Force(y) = %f \n", Fy);
		printf("Force(z) = %f \n", Fz);
		printf("Sub-domain size = %i x %i x %i\n",Nz,Nz,Nz);
		printf("Parallel domain size = %i x %i x %i\n",nprocx,nprocy,nprocz);
		if (BoundaryCondition==0) printf("Periodic boundary conditions will applied \n");
		if (BoundaryCondition==1) printf("Pressure boundary conditions will be applied \n");
		if (BoundaryCondition==2) printf("Velocity boundary conditions will be applied \n");
		if (InitialCondition==0) printf("Initial conditions assigned from phase ID file \n");
		if (InitialCondition==1) printf("Initial conditions asdsigned from restart file \n");
		printf("********************************************************\n");
	}

	// Initialized domain and averaging framework for Two-Phase Flow
	bool pBC,velBC;
	if (BoundaryCondition==1)	pBC=true;
	else						pBC=false;
	if (BoundaryCondition==2)	velBC=true;
	else						velBC=false;
	bool Restart;
	if (InitialCondition==1)    Restart=true;
	else 						Restart=false;

	Domain Dm(Nx,Ny,Nz,rank,nprocx,nprocy,nprocz,Lx,Ly,Lz,BoundaryCondition);
	TwoPhase Averages(Dm);

	InitializeRanks( rank, nprocx, nprocy, nprocz, iproc, jproc, kproc,
			 	 	 rank_x, rank_y, rank_z, rank_X, rank_Y, rank_Z,
			 	 	 rank_xy, rank_XY, rank_xY, rank_Xy, rank_xz, rank_XZ, rank_xZ, rank_Xz,
			 	 	 rank_yz, rank_YZ, rank_yZ, rank_Yz );
	 
	MPI_Barrier(MPI_COMM_WORLD);

	Nz += 2;
	Nx = Ny = Nz;	// Cubic domain

	int N = Nx*Ny*Nz;
	int dist_mem_size = N*sizeof(double);

	//.......................................................................
	if (rank == 0)	printf("Read input media... \n");
	//.......................................................................
	
	//.......................................................................
	// Filenames used
	char LocalRankString[8];
	char LocalRankFilename[40];
	char LocalRestartFile[40];
	char tmpstr[10];
	sprintf(LocalRankString,"%05d",rank);
	sprintf(LocalRankFilename,"%s%s","ID.",LocalRankString);
	sprintf(LocalRestartFile,"%s%s","Restart.",LocalRankString);
	
//	printf("Local File Name =  %s \n",LocalRankFilename);
	// .......... READ THE INPUT FILE .......................................
//	char value;
	char *id;
	id = new char[N];
	int sum = 0;
	double sum_local;
	double iVol_global = 1.0/(1.0*(Nx-2)*(Ny-2)*(Nz-2)*nprocs);
	if (BoundaryCondition > 0) iVol_global = 1.0/(1.0*(Nx-2)*nprocx*(Ny-2)*nprocy*((Nz-2)*nprocz-6));
	double porosity, pore_vol;
	//...........................................................................
	if (rank == 0) cout << "Reading in domain from signed distance function..." << endl;

	//.......................................................................
	sprintf(LocalRankString,"%05d",rank);
//	sprintf(LocalRankFilename,"%s%s","ID.",LocalRankString);
//	WriteLocalSolidID(LocalRankFilename, id, N);
	sprintf(LocalRankFilename,"%s%s","SignDist.",LocalRankString);
	ReadBinaryFile(LocalRankFilename, Averages.SDs.get(), N);
	MPI_Barrier(MPI_COMM_WORLD);
	if (rank == 0) cout << "Domain set." << endl;
	
	//.......................................................................
	// Assign the phase ID field based on the signed distance
	//.......................................................................
	for (k=0;k<Nz;k++){
		for (j=0;j<Ny;j++){
			for (i=0;i<Nx;i++){
				n = k*Nx*Ny+j*Nx+i;
				id[n] = 0;
			}
		}
	}
	sum=0;
	pore_vol = 0.0;
	for ( k=0;k<Nz;k++){
		for ( j=0;j<Ny;j++){
			for ( i=0;i<Nx;i++){
				n = k*Nx*Ny+j*Nx+i;
				if (Averages.SDs(n) > 0.0){
					id[n] = 2;	
				}
				// compute the porosity (actual interface location used)
				if (Averages.SDs(n) > 0.0){
					sum++;	
				}
			}
		}
	}

	if (rank==0) printf("Initialize from segmented data: solid=0, NWP=1, WP=2 \n");
	sprintf(LocalRankFilename,"ID.%05i",rank);
	FILE *IDFILE = fopen(LocalRankFilename,"rb");
	if (IDFILE==NULL) ERROR("Error opening file: ID.xxxxx");
	fread(id,1,N,IDFILE);
	fclose(IDFILE);

	for ( k=0;k<Nz;k++){
		for ( j=0;j<Ny;j++){
			for ( i=0;i<Nx;i++){
				n = k*Nx*Ny+j*Nx+i;
				// The following turns off communication if external BC are being set
				if (BoundaryCondition > 0){
					if (kproc==0 && k==0)			id[n]=0;
					if (kproc==0 && k==1)			id[n]=0;
					if (kproc==nprocz-1 && k==Nz-2)	id[n]=0;
					if (kproc==nprocz-1 && k==Nz-1)	id[n]=0;
				}
			}
		}
	}

	// Set up kstart, kfinish so that the reservoirs are excluded from averaging
	int kstart,kfinish;
	kstart = 1;
	kfinish = Nz-1;
	if (BoundaryCondition >  0 && kproc==0)		kstart = 4;
	if (BoundaryCondition >  0 && kproc==nprocz-1)	kfinish = Nz-4;

	// Compute the pore volume
	sum_local = 0.0;
	for ( k=kstart;k<kfinish;k++){
		for ( j=1;j<Ny-1;j++){
			for ( i=1;i<Nx-1;i++){
				n = k*Nx*Ny+j*Nx+i;
				if (id[n] > 0){
					sum_local += 1.0;
				}
			}
		}
	}
	MPI_Allreduce(&sum_local,&pore_vol,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
//	MPI_Allreduce(&sum_local,&porosity,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
	porosity = pore_vol*iVol_global;
	if (rank==0) printf("Media porosity = %f \n",porosity);
	//.........................................................
	// If external boundary conditions are applied remove solid
	if (BoundaryCondition >  0  && kproc == 0){
		for (k=0; k<3; k++){
			for (j=0;j<Ny;j++){
				for (i=0;i<Nx;i++){
					n = k*Nx*Ny+j*Nx+i;
					id[n] = 1;
					Averages.SDs(n) = max(Averages.SDs(n),1.0*(2.5-k));
				}					
			}
		}
	}
	if (BoundaryCondition >  0  && kproc == nprocz-1){
		for (k=Nz-3; k<Nz; k++){
			for (j=0;j<Ny;j++){
				for (i=0;i<Nx;i++){
					n = k*Nx*Ny+j*Nx+i;
					id[n] = 2;
					Averages.SDs(n) = max(Averages.SDs(n),1.0*(k-Nz+2.5));
				}					
			}
		}
	}
	//.........................................................
	// don't perform computations at the eight corners
	id[0] = id[Nx-1] = id[(Ny-1)*Nx] = id[(Ny-1)*Nx + Nx-1] = 0;
	id[(Nz-1)*Nx*Ny] = id[(Nz-1)*Nx*Ny+Nx-1] = id[(Nz-1)*Nx*Ny+(Ny-1)*Nx] = id[(Nz-1)*Nx*Ny+(Ny-1)*Nx + Nx-1] = 0;
	//.........................................................

	// Initialize communication structures in averaging domain
	for (i=0; i<Dm.Nx*Dm.Ny*Dm.Nz; i++) Dm.id[i] = id[i];
	Dm.CommInit(MPI_COMM_WORLD);

	// set reservoirs
	if (BoundaryCondition > 0){
		for ( k=0;k<Nz;k++){
			for ( j=0;j<Ny;j++){
				for ( i=0;i<Nx;i++){
					// The following turns off communication if external BC are being set
					if (kproc==0 && k==0)			id[n]=1;
					if (kproc==0 && k==1)			id[n]=1;
					if (kproc==nprocz-1 && k==Nz-2)	id[n]=2;
					if (kproc==nprocz-1 && k==Nz-1)	id[n]=2;
					Dm.id[n] = id[n];
				}
			}
		}
	}

	//...........................................................................
	if (rank==0)	printf ("Create ScaLBL_Communicator \n");
	// Create a communicator for the device
	ScaLBL_Communicator ScaLBL_Comm(Dm);
	
	//...........device phase ID.................................................
	if (rank==0)	printf ("Copy phase ID to device \n");
	char *ID;
	AllocateDeviceMemory((void **) &ID, N);						// Allocate device memory
	// Don't compute in the halo
	for (k=0;k<Nz;k++){
		for (j=0;j<Ny;j++){
			for (i=0;i<Nx;i++){
				n = k*Nx*Ny+j*Nx+i;
				if (i==0 || i==Nx-1 || j==0 || j==Ny-1 || k==0 || k==Nz-1)	id[n] = 0;
			}
		}
	}
	// Copy to the device
	CopyToDevice(ID, id, N);
	DeviceBarrier();
	//...........................................................................

	//...........................................................................
	//				MAIN  VARIABLES ALLOCATED HERE
	//...........................................................................
	// LBM variables
	if (rank==0)	printf ("Allocating distributions \n");
	//......................device distributions.................................
	double *f_even,*f_odd;
	double *A_even,*A_odd,*B_even,*B_odd;
	//...........................................................................
	AllocateDeviceMemory((void **) &f_even, 10*dist_mem_size);	// Allocate device memory
	AllocateDeviceMemory((void **) &f_odd, 9*dist_mem_size);	// Allocate device memory
	AllocateDeviceMemory((void **) &A_even, 4*dist_mem_size);	// Allocate device memory
	AllocateDeviceMemory((void **) &A_odd, 3*dist_mem_size);	// Allocate device memory
	AllocateDeviceMemory((void **) &B_even, 4*dist_mem_size);	// Allocate device memory
	AllocateDeviceMemory((void **) &B_odd, 3*dist_mem_size);	// Allocate device memory
	//...........................................................................
	double *Phi,*Den;
	double *ColorGrad, *Velocity, *Pressure, *dvcSignDist;
	//...........................................................................
	AllocateDeviceMemory((void **) &Phi, dist_mem_size);
	AllocateDeviceMemory((void **) &Pressure, dist_mem_size);
	AllocateDeviceMemory((void **) &dvcSignDist, dist_mem_size);
	AllocateDeviceMemory((void **) &Den, 2*dist_mem_size);
	AllocateDeviceMemory((void **) &Velocity, 3*dist_mem_size);
	AllocateDeviceMemory((void **) &ColorGrad, 3*dist_mem_size);
	//copies of data needed to perform checkpointing from cpu
	double *cDen, *cDistEven, *cDistOdd;
	cDen = new double[2*N];
	cDistEven = new double[10*N];
	cDistOdd = new double[9*N];
	//...........................................................................

	// Copy signed distance for device initialization
	CopyToDevice(dvcSignDist, Averages.SDs.get(), dist_mem_size);
	//...........................................................................

	int logcount = 0; // number of surface write-outs
	
	//...........................................................................
	//				MAIN  VARIABLES INITIALIZED HERE
	//...........................................................................
	//...........................................................................
	//...........................................................................
	if (rank==0)	printf("Setting the distributions, size = %i\n", N);
	//...........................................................................
	DeviceBarrier();
	InitD3Q19(ID, f_even, f_odd, Nx, Ny, Nz);
	InitDenColor(ID, Den, Phi, das, dbs, Nx, Ny, Nz);
	DeviceBarrier();
	//......................................................................

	if (Restart == true){
		if (rank==0) printf("Reading restart file! \n");
		// Read in the restart file to CPU buffers
		ReadCheckpoint(LocalRestartFile, cDen, cDistEven, cDistOdd, N);
		// Copy the restart data to the GPU
		CopyToDevice(f_even,cDistEven,10*N*sizeof(double));
		CopyToDevice(f_odd,cDistOdd,9*N*sizeof(double));
		CopyToDevice(Den,cDen,2*N*sizeof(double));
		DeviceBarrier();
		MPI_Barrier(MPI_COMM_WORLD);
	}

	//......................................................................
	InitD3Q7(ID, A_even, A_odd, &Den[0], Nx, Ny, Nz);
	InitD3Q7(ID, B_even, B_odd, &Den[N], Nx, Ny, Nz);
	DeviceBarrier();
	MPI_Barrier(MPI_COMM_WORLD);
	//.......................................................................
	// Once phase has been initialized, map solid to account for 'smeared' interface
	for (i=0; i<N; i++)	Averages.SDs(i) -= (1.0); //
	//.......................................................................
	// Finalize setup for averaging domain
	//Averages.SetupCubes(Dm);
	Averages.UpdateSolid();
	//.......................................................................
	
	//*************************************************************************
	// 		Compute the phase indicator field and reset Copy, Den
	//*************************************************************************
	ComputePhi(ID, Phi, Den, N);
	//*************************************************************************
	DeviceBarrier();
	ScaLBL_Comm.SendHalo(Phi);
	ScaLBL_Comm.RecvHalo(Phi);
	DeviceBarrier();
	MPI_Barrier(MPI_COMM_WORLD);
	//*************************************************************************

	if (rank==0 && BoundaryCondition==1){
		printf("Setting inlet pressure = %f \n", din);
		printf("Setting outlet pressure = %f \n", dout);
	}
	if (BoundaryCondition==1 && kproc == 0)	{
		PressureBC_inlet(f_even,f_odd,din,Nx,Ny,Nz);
		ColorBC_inlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
	}
		
	if (BoundaryCondition==1 && kproc == nprocz-1){
		PressureBC_outlet(f_even,f_odd,dout,Nx,Ny,Nz,Nx*Ny*(Nz-2));
		ColorBC_outlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
	}

	if (rank==0 && BoundaryCondition==2){
		printf("Setting inlet velocity = %f \n", din);
		printf("Setting outlet velocity = %f \n", dout);
	}
	if (BoundaryCondition==2 && kproc == 0)	{
		ScaLBL_D3Q19_Velocity_BC_z(f_even,f_odd,din,Nx,Ny,Nz);
		//ColorBC_inlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
		SetPhiSlice_z(Phi,1.0,Nx,Ny,Nz,0);
	}

	if (BoundaryCondition==2 && kproc == nprocz-1){
		ScaLBL_D3Q19_Velocity_BC_Z(f_even,f_odd,dout,Nx,Ny,Nz,Nx*Ny*(Nz-2));
		//ColorBC_outlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
		SetPhiSlice_z(Phi,-1.0,Nx,Ny,Nz,Nz-1);
	}

	ComputePressureD3Q19(ID,f_even,f_odd,Pressure,Nx,Ny,Nz);
	ComputeVelocityD3Q19(ID,f_even,f_odd,Velocity,Nx,Ny,Nz);

	//...........................................................................
	// Copy the phase indicator field for the earlier timestep
	DeviceBarrier();
	CopyToHost(Averages.Phase_tplus.get(),Phi,N*sizeof(double));
	//...........................................................................
	//...........................................................................
	// Copy the data for for the analysis timestep
	//...........................................................................
	// Copy the phase from the GPU -> CPU
	//...........................................................................
	DeviceBarrier();
	ComputePressureD3Q19(ID,f_even,f_odd,Pressure,Nx,Ny,Nz);
	CopyToHost(Averages.Phase.get(),Phi,N*sizeof(double));
	CopyToHost(Averages.Press.get(),Pressure,N*sizeof(double));
	CopyToHost(Averages.Vel_x.get(),&Velocity[0],N*sizeof(double));
	CopyToHost(Averages.Vel_y.get(),&Velocity[N],N*sizeof(double));
	CopyToHost(Averages.Vel_z.get(),&Velocity[2*N],N*sizeof(double));
	//...........................................................................
	
	int timestep = 0;
	if (rank==0) printf("********************************************************\n");
	if (rank==0)	printf("No. of timesteps: %i \n", timestepMax);

	//.......create and start timer............
	double starttime,stoptime,cputime;
	DeviceBarrier();
	MPI_Barrier(MPI_COMM_WORLD);
	starttime = MPI_Wtime();
	//.........................................
	
	sendtag = recvtag = 5;
			// Copy the data to the CPU
	err = 1.0; 	
	double sat_w_previous = 1.01; // slightly impossible value!
	if (rank==0) printf("Begin timesteps: error tolerance is %f \n", tol);
	//************ MAIN ITERATION LOOP ***************************************/
	while (timestep < timestepMax && err > tol ){

		//*************************************************************************
		// Fused Color Gradient and Collision 
		//*************************************************************************
		ColorCollideOpt( ID,f_even,f_odd,Phi,ColorGrad,
							 Velocity,Nx,Ny,Nz,rlxA,rlxB,alpha,beta,Fx,Fy,Fz);
		//*************************************************************************

		DeviceBarrier();
		//*************************************************************************
		// Pack and send the D3Q19 distributions
		ScaLBL_Comm.SendD3Q19(f_even, f_odd);
		//*************************************************************************

		//*************************************************************************
		// 		Carry out the density streaming step for mass transport
		//*************************************************************************
		MassColorCollideD3Q7(ID, A_even, A_odd, B_even, B_odd, Den, Phi,
								ColorGrad, Velocity, beta, N, pBC);
		//*************************************************************************

		DeviceBarrier();
		MPI_Barrier(MPI_COMM_WORLD);
		//*************************************************************************
		// 		Swap the distributions for momentum transport
		//*************************************************************************
		SwapD3Q19(ID, f_even, f_odd, Nx, Ny, Nz);
		//*************************************************************************

		DeviceBarrier();
		MPI_Barrier(MPI_COMM_WORLD);
		//*************************************************************************
		// Wait for communications to complete and unpack the distributions
		ScaLBL_Comm.RecvD3Q19(f_even, f_odd);
		//*************************************************************************

		DeviceBarrier();
		//*************************************************************************
		// Pack and send the D3Q7 distributions
		ScaLBL_Comm.BiSendD3Q7(A_even, A_odd, B_even, B_odd);
		//*************************************************************************

		DeviceBarrier();
		SwapD3Q7(ID, A_even, A_odd, Nx, Ny, Nz);
		SwapD3Q7(ID, B_even, B_odd, Nx, Ny, Nz);

		DeviceBarrier();
		MPI_Barrier(MPI_COMM_WORLD);

		//*************************************************************************
		// Wait for communication and unpack the D3Q7 distributions
		ScaLBL_Comm.BiRecvD3Q7(A_even, A_odd, B_even, B_odd);
		//*************************************************************************

		DeviceBarrier();
		//..................................................................................
		ComputeDensityD3Q7(ID, A_even, A_odd, &Den[0], Nx, Ny, Nz);
		ComputeDensityD3Q7(ID, B_even, B_odd, &Den[N], Nx, Ny, Nz);
		//*************************************************************************
		// 		Compute the phase indicator field 
		//*************************************************************************
		DeviceBarrier();
		MPI_Barrier(MPI_COMM_WORLD);

		ComputePhi(ID, Phi, Den, N);
		//*************************************************************************
		ScaLBL_Comm.SendHalo(Phi);
		DeviceBarrier();
		ScaLBL_Comm.RecvHalo(Phi);
		//*************************************************************************

		DeviceBarrier();
		
		// Pressure boundary conditions
		if (BoundaryCondition==1 && kproc == 0)	{
			PressureBC_inlet(f_even,f_odd,din,Nx,Ny,Nz);
			ColorBC_inlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
		}
		if (BoundaryCondition==1 && kproc == nprocz-1){
			PressureBC_outlet(f_even,f_odd,dout,Nx,Ny,Nz,Nx*Ny*(Nz-2));
			ColorBC_outlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
		}

		// Velocity boundary conditions
		if (BoundaryCondition==2 && kproc == 0)	{
			ScaLBL_D3Q19_Velocity_BC_z(f_even,f_odd,din,Nx,Ny,Nz);
			//ColorBC_inlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
			SetPhiSlice_z(Phi,1.0,Nx,Ny,Nz,0);
		}
		if (BoundaryCondition==2 && kproc == nprocz-1){
			ScaLBL_D3Q19_Velocity_BC_Z(f_even,f_odd,dout,Nx,Ny,Nz,Nx*Ny*(Nz-2));
			//ColorBC_outlet(Phi,Den,A_even,A_odd,B_even,B_odd,Nx,Ny,Nz);
			SetPhiSlice_z(Phi,-1.0,Nx,Ny,Nz,Nz-1);
		}
		//...................................................................................

		MPI_Barrier(MPI_COMM_WORLD);

		// Timestep completed!
		timestep++;
		//...................................................................
		if (timestep%1000 == 995){
			//...........................................................................
			// Copy the phase indicator field for the earlier timestep
			DeviceBarrier();
			CopyToHost(Averages.Phase_tplus.get(),Phi,N*sizeof(double));
	//		Averages.ColorToSignedDistance(beta,Averages.Phase,Averages.Phase_tplus);
			//...........................................................................
		}
		if (timestep%1000 == 0){
			//...........................................................................
			// Copy the data for for the analysis timestep
			//...........................................................................
			// Copy the phase from the GPU -> CPU
			//...........................................................................
			DeviceBarrier();
			ComputePressureD3Q19(ID,f_even,f_odd,Pressure,Nx,Ny,Nz);
			CopyToHost(Averages.Phase.get(),Phi,N*sizeof(double));
			CopyToHost(Averages.Press.get(),Pressure,N*sizeof(double));
			CopyToHost(Averages.Vel_x.get(),&Velocity[0],N*sizeof(double));
			CopyToHost(Averages.Vel_y.get(),&Velocity[N],N*sizeof(double));
			CopyToHost(Averages.Vel_z.get(),&Velocity[2*N],N*sizeof(double));
			MPI_Barrier(MPI_COMM_WORLD);
		}
		if (timestep%1000 == 5){
			//...........................................................................
			// Copy the phase indicator field for the later timestep
			DeviceBarrier();
			CopyToHost(Averages.Phase_tminus.get(),Phi,N*sizeof(double));
//			Averages.ColorToSignedDistance(beta,Averages.Phase_tminus,Averages.Phase_tminus);
			//....................................................................
			Averages.Initialize();
			Averages.ComputeDelPhi();
			Averages.ColorToSignedDistance(beta,Averages.Phase,Averages.SDn);
			Averages.UpdateMeshValues();
			Averages.ComputeLocal();
			Averages.Reduce();
			Averages.PrintAll(timestep);
/*			Averages.Initialize();
			Averages.ComponentAverages();
			Averages.SortBlobs();
			Averages.PrintComponents(timestep);
*/			//....................................................................
		}

		if (timestep%RESTART_INTERVAL == 0){
			if (pBC){
				//err = fabs(sat_w - sat_w_previous);
				//sat_w_previous = sat_w;
				if (rank==0) printf("Timestep %i: change in saturation since last checkpoint is %f \n", timestep, err);
			}
			else{
				// Not clear yet
			}
			// Copy the data to the CPU
			CopyToHost(cDistEven,f_even,10*N*sizeof(double));
			CopyToHost(cDistOdd,f_odd,9*N*sizeof(double));
			CopyToHost(cDen,Den,2*N*sizeof(double));
			// Read in the restart file to CPU buffers
			WriteCheckpoint(LocalRestartFile, cDen, cDistEven, cDistOdd, N);
		}
	}
	//************************************************************************/
	DeviceBarrier();
	MPI_Barrier(MPI_COMM_WORLD);
	stoptime = MPI_Wtime();
	if (rank==0) printf("-------------------------------------------------------------------\n");
	// Compute the walltime per timestep
	cputime = (stoptime - starttime)/timestep;
	// Performance obtained from each node
	double MLUPS = double(Nx*Ny*Nz)/cputime/1000000;
	
	if (rank==0) printf("********************************************************\n");
	if (rank==0) printf("CPU time = %f \n", cputime);
	if (rank==0) printf("Lattice update rate (per core)= %f MLUPS \n", MLUPS);
	MLUPS *= nprocs;
	if (rank==0) printf("Lattice update rate (total)= %f MLUPS \n", MLUPS);
	if (rank==0) printf("********************************************************\n");
	
	//************************************************************************/
/*	// Perform component averaging and write tcat averages
	Averages.Initialize();
	Averages.ComponentAverages();
	Averages.SortBlobs();
	Averages.PrintComponents(timestep);
	//************************************************************************/
/*
	int NumberComponents_NWP = ComputeGlobalPhaseComponent(Dm.Nx-2,Dm.Ny-2,Dm.Nz-2,Dm.rank_info,Averages.PhaseID,1,Averages.Label_NWP);
	printf("Number of non-wetting phase components: %i \n ",NumberComponents_NWP);
	DeviceBarrier();
	CopyToHost(Averages.Phase.get(),Phi,N*sizeof(double));
*/
    // Create the MeshDataStruct
    fillHalo<double> fillData(Dm.rank_info,Nx-2,Ny-2,Nz-2,1,1,1,0,1);
    std::vector<IO::MeshDataStruct> meshData(1);
    meshData[0].meshName = "domain";
    meshData[0].mesh = std::shared_ptr<IO::DomainMesh>( new IO::DomainMesh(Dm.rank_info,Nx-2,Ny-2,Nz-2,Lx,Ly,Lz) );
    std::shared_ptr<IO::Variable> PhaseVar( new IO::Variable() );
    std::shared_ptr<IO::Variable> SignDistVar( new IO::Variable() );
    std::shared_ptr<IO::Variable> BlobIDVar( new IO::Variable() );
    PhaseVar->name = "phase";
    PhaseVar->type = IO::VolumeVariable;
    PhaseVar->dim = 1;
    PhaseVar->data.resize(Nx-2,Ny-2,Nz-2);
    meshData[0].vars.push_back(PhaseVar);
    SignDistVar->name = "SignDist";
    SignDistVar->type = IO::VolumeVariable;
    SignDistVar->dim = 1;
    SignDistVar->data.resize(Nx-2,Ny-2,Nz-2);
    meshData[0].vars.push_back(SignDistVar);
    BlobIDVar->name = "BlobID";
    BlobIDVar->type = IO::VolumeVariable;
    BlobIDVar->dim = 1;
    BlobIDVar->data.resize(Nx-2,Ny-2,Nz-2);
    meshData[0].vars.push_back(BlobIDVar);
    
    fillData.copy(Averages.SDn,PhaseVar->data);
    fillData.copy(Averages.SDs,SignDistVar->data);
    fillData.copy(Averages.Label_NWP,BlobIDVar->data);
    IO::writeData( 0, meshData, 2 );
    
/*	Averages.WriteSurfaces(0);

	sprintf(LocalRankFilename,"%s%s","Phase.",LocalRankString);
	FILE *PHASE;
	PHASE = fopen(LocalRankFilename,"wb");
	fwrite(Averages.Phase.get(),8,N,PHASE);
	fclose(PHASE);

	/*	sprintf(LocalRankFilename,"%s%s","Pressure.",LocalRankString);
	FILE *PRESS;
	PRESS = fopen(LocalRankFilename,"wb");
	fwrite(Averages.Press.get(),8,N,PRESS);
	fclose(PRESS);

	CopyToHost(Averages.Phase.get(),Phi,N*sizeof(double));
	double * Grad;
	Grad = new double [3*N];
	CopyToHost(Grad,ColorGrad,3*N*sizeof(double));
	sprintf(LocalRankFilename,"%s%s","ColorGrad.",LocalRankString);
	FILE *GRAD;
	GRAD = fopen(LocalRankFilename,"wb");
	fwrite(Grad,8,3*N,GRAD);
	fclose(GRAD);
	*/
	// ****************************************************
	MPI_Barrier(MPI_COMM_WORLD);
	MPI_Finalize();
	// ****************************************************
}
