#include "compute_flare_std_atom.h"
#include "atom.h"
#include "comm.h"
#include "error.h"
#include "force.h"
#include "memory.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "neighbor.h"
#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

// flare++ modules
#include "cutoffs.h"
#include "lammps_descriptor.h"
#include "radial.h"
#include "y_grad.h"

using namespace LAMMPS_NS;

#define MAXLINE 1024

/* ---------------------------------------------------------------------- */

ComputeFlareStdAtom::ComputeFlareStdAtom(LAMMPS *lmp, int narg, char **arg) : 
  Compute(lmp, narg, arg),
  stds(NULL)
{
  peratom_flag = 1;
  size_peratom_cols = 0;
  timeflag = 1;
  comm_reverse = 1;

  // restartinfo = 0;
  // manybody_flag = 1;

  setflag = 0;
  cutsq = NULL;

  beta = NULL;
  settings(narg, arg);
  coeff(narg, arg);
}

/* ----------------------------------------------------------------------
   check if allocated, since class can be destructed when incomplete
------------------------------------------------------------------------- */

ComputeFlareStdAtom::~ComputeFlareStdAtom() {
  if (copymode)
    return;

  memory->destroy(beta);

  if (allocated) {
    memory->destroy(setflag);
    memory->destroy(cutsq);
  }

  memory->destroy(stds);
  memory->destroy(desc_derv);
}

/* ---------------------------------------------------------------------- */

void ComputeFlareStdAtom::compute_peratom() {
  if (atom->nmax > nmax) {
    nmax = atom->nmax;
    memory->destroy(stds);
    memory->create(stds,nmax,3,"flarestd/atom:stds");
    array_atom = stds;

    memory->destroy(desc_derv); // need to monitor the memory since the descriptors can be high dim
    memory->create(desc_derv,nmax * 3, n_descriptors,"flarestd/atom:desc_derv");
  }

  int i, j, ii, jj, inum, jnum, itype, jtype, n_inner, n_count;
  double evdwl, delx, dely, delz, xtmp, ytmp, ztmp, rsq;
  int *ilist, *jlist, *numneigh, **firstneigh;

  double **x = atom->x;
  double **f = atom->f;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  int nall = nlocal + atom->nghost;
  int newton_pair = force->newton_pair;
  int ntotal = nlocal;
  if (force->newton) ntotal += atom->nghost;

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  int beta_init, beta_counter;
  double B2_norm_squared, B2_val_1, B2_val_2;
  Eigen::VectorXd single_bond_vals, B2_vals, B2_env_dot, beta_p, partial_forces;
  Eigen::MatrixXd single_bond_env_dervs, B2_env_dervs;

  for (int ii = 0; ii < ntotal; ii++) {
    i = ilist[ii];
    for (int comp = 0; comp < 3; comp++) {
      stds[i][comp] = 0.0;
      for (int nl = 0; nl < n_descriptors; nl++) {
        desc_derv[i * 3 + comp][nl] = 0.0;
      }
    }
  }

  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    itype = type[i];
    jnum = numneigh[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    jlist = firstneigh[i];

    // Count the atoms inside the cutoff.
    n_inner = 0;
    for (int jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      delx = x[j][0] - xtmp;
      dely = x[j][1] - ytmp;
      delz = x[j][2] - ztmp;
      rsq = delx * delx + dely * dely + delz * delz;
      if (rsq < (cutoff * cutoff))
        n_inner++;
    }

    // Compute covariant descriptors.
    single_bond(x, type, jnum, n_inner, i, xtmp, ytmp, ztmp, jlist,
                basis_function, cutoff_function, cutoff, n_species, n_max,
                l_max, radial_hyps, cutoff_hyps, single_bond_vals,
                single_bond_env_dervs);

    // Compute invariant descriptors.
    B2_descriptor(B2_vals, B2_env_dervs, B2_norm_squared, B2_env_dot,
                  single_bond_vals, single_bond_env_dervs, n_species, n_max,
                  l_max);

    // Compute local energy and partial forces.
    //beta_p = beta_matrices[itype - 1] * B2_vals;
    //stds[i] += pow(B2_vals.dot(beta_p) / B2_norm_squared, 0.5) ;
    //partial_forces =
    //    2 * (-B2_env_dervs * beta_p + evdwl * B2_env_dot) / B2_norm_squared;

    // Update energy, force and stress arrays.
    n_count = 0;
    for (int jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      delx = xtmp - x[j][0];
      dely = ytmp - x[j][1];
      delz = ztmp - x[j][2];
      rsq = delx * delx + dely * dely + delz * delz;

      if (rsq < (cutoff * cutoff)) {
        for (int comp = 0; comp < 3; comp++) {
          for (int nl = 0; nl < n_descriptors; nl++) { // need to make sure the neighbor orders of B2_env_derv and jj are the same
            desc_derv[i * 3 + comp][nl] += B2_env_dervs(n_count * 3 + comp, nl);
            desc_derv[j * 3 + comp][nl] -= B2_env_dervs(n_count * 3 + comp, nl);
          }
        }
        n_count++;
      }
    }
  }
  // be careful the beta matrix has a factor of two, see read_file 
  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    itype = type[i];

    for (int nl1 = 0; nl1 < n_descriptors; nl1++) {
      for (int nl2 = nl1; nl2 < n_descriptors; nl2++) {
        for (int comp = 0; comp < 3; comp++) {
          if (nl1 == nl2) {
            stds[i][comp] += desc_derv[i * 3 + comp][nl1] * beta_matrices[itype - 1](nl1, nl2) * desc_derv[i * 3 + comp][nl2];
          } else {
            stds[i][comp] += desc_derv[i * 3 + comp][nl1] * beta_matrices[itype - 1](nl1, nl2) * desc_derv[i * 3 + comp][nl2] * 2;
          }
        }
      }
    }

  }


}

/* ---------------------------------------------------------------------- */

int ComputeFlareStdAtom::pack_reverse_comm(int n, int first, double *buf)
{
  int i,m,last;

  m = 0;
  last = first + n;
  for (i = first; i < last; i++) {
    for (int comp = 0; comp < 3; comp++) {
      buf[m++] = stds[i][comp];
    }
  }
  return m;
}

/* ---------------------------------------------------------------------- */

void ComputeFlareStdAtom::unpack_reverse_comm(int n, int *list, double *buf)
{
  int i,j,m;

  m = 0;
  for (i = 0; i < n; i++) {
    j = list[i];
    for (int comp = 0; comp < 3; comp++) {
      stds[j][comp] += buf[m++];
    }
  }
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based array
------------------------------------------------------------------------- */

double ComputeFlareStdAtom::memory_usage()
{
  double bytes = nmax * sizeof(double);
  return bytes;
}



/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void ComputeFlareStdAtom::allocate() {
  allocated = 1;
  int n = atom->ntypes;

  memory->create(setflag, n + 1, n + 1, "compute:setflag");

  // Set the diagonal of setflag to 1 (otherwise pair.cpp will throw an error)
  for (int i = 1; i <= n; i++)
    setflag[i][i] = 1;

  // Create cutsq array (used in pair.cpp)
  memory->create(cutsq, n + 1, n + 1, "compute:cutsq");
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void ComputeFlareStdAtom::settings(int narg, char ** /*arg*/) {
  // "flare" should be the only word after "compute" in the input file.
  if (narg > 0)
    error->all(FLERR, "Illegal compute command");
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
   read DYNAMO funcfl file
------------------------------------------------------------------------- */

void ComputeFlareStdAtom::coeff(int narg, char **arg) {
  if (!allocated)
    allocate();

  // Should be exactly 3 arguments following "pair_coeff" in the input file.
  if (narg != 3)
    error->all(FLERR, "Incorrect args for compute coefficients");

  // Ensure I,J args are "* *".
  if (strcmp(arg[0], "*") != 0 || strcmp(arg[1], "*") != 0)
    error->all(FLERR, "Incorrect args for compute coefficients");

  read_file(arg[2]);
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void ComputeFlareStdAtom::init() {
  // Require newton on.
  if (force->newton_pair == 0)
    error->all(FLERR, "Compute command requires newton pair on");

  // Request a full neighbor list.
  int irequest = neighbor->request(this, instance_me);
  neighbor->requests[irequest]->half = 0;
  neighbor->requests[irequest]->full = 1;
}

void ComputeFlareStdAtom::init_list(int /*id*/, NeighList *ptr)
{
  list = ptr;
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double ComputeFlareStdAtom::init_one(int i, int j) {
  // init_one is called for each i, j pair in pair.cpp after calling init_style.
  if (setflag[i][j] == 0)
    error->all(FLERR, "All pair coeffs are not set");
  return cutoff;
}

/* ----------------------------------------------------------------------
   read potential values from a DYNAMO single element funcfl file
------------------------------------------------------------------------- */

void ComputeFlareStdAtom::read_file(char *filename) {
  int me = comm->me;
  char line[MAXLINE], radial_string[MAXLINE], cutoff_string[MAXLINE];
  int radial_string_length, cutoff_string_length;
  FILE *fptr;

  // Check that the potential file can be opened.
  if (me == 0) {
    fptr = utils::open_potential(filename,lmp,nullptr);
    if (fptr == NULL) {
      char str[128];
      snprintf(str, 128, "Cannot open variance file %s", filename);
      error->one(FLERR, str);
    }
  }

  int tmp, nwords;
  if (me == 0) {
    fgets(line, MAXLINE, fptr);
    fgets(line, MAXLINE, fptr);
    sscanf(line, "%s", radial_string); // Radial basis set
    radial_string_length = strlen(radial_string);
    fgets(line, MAXLINE, fptr);
    sscanf(line, "%i %i %i %i", &n_species, &n_max, &l_max, &beta_size);
    fgets(line, MAXLINE, fptr);
    sscanf(line, "%s", cutoff_string); // Cutoff function
    cutoff_string_length = strlen(cutoff_string);
    fgets(line, MAXLINE, fptr);
    sscanf(line, "%lg", &cutoff); // Cutoff
  }

  MPI_Bcast(&n_species, 1, MPI_INT, 0, world);
  MPI_Bcast(&n_max, 1, MPI_INT, 0, world);
  MPI_Bcast(&l_max, 1, MPI_INT, 0, world);
  MPI_Bcast(&beta_size, 1, MPI_INT, 0, world);
  MPI_Bcast(&cutoff, 1, MPI_DOUBLE, 0, world);
  MPI_Bcast(&radial_string_length, 1, MPI_INT, 0, world);
  MPI_Bcast(&cutoff_string_length, 1, MPI_INT, 0, world);
  MPI_Bcast(radial_string, radial_string_length + 1, MPI_CHAR, 0, world);
  MPI_Bcast(cutoff_string, cutoff_string_length + 1, MPI_CHAR, 0, world);

  // Set number of descriptors.
  int n_radial = n_max * n_species;
  n_descriptors = (n_radial * (n_radial + 1) / 2) * (l_max + 1);

  // Check the relationship between the power spectrum and beta.
  int beta_check = n_descriptors * (n_descriptors + 1) / 2;
  if (beta_check != beta_size)
    error->all(FLERR, "Beta size doesn't match the number of descriptors.");

  // Set the radial basis.
  if (!strcmp(radial_string, "chebyshev")) {
    basis_function = chebyshev;
    radial_hyps = std::vector<double>{0, cutoff};
  }

  // Set the cutoff function.
  if (!strcmp(cutoff_string, "quadratic"))
    cutoff_function = quadratic_cutoff;
  else if (!strcmp(cutoff_string, "cosine"))
    cutoff_function = cos_cutoff;

  // Parse the beta vectors.
  memory->create(beta, beta_size * n_species, "compute:beta");
  if (me == 0)
    grab(fptr, beta_size * n_species, beta);
  MPI_Bcast(beta, beta_size * n_species, MPI_DOUBLE, 0, world);

  // Fill in the beta matrix.
  // TODO: Remove factor of 2 from beta.
  Eigen::MatrixXd beta_matrix;
  int beta_count = 0;
  double beta_val;
  for (int k = 0; k < n_species; k++) {
    beta_matrix = Eigen::MatrixXd::Zero(n_descriptors, n_descriptors);
    for (int i = 0; i < n_descriptors; i++) {
      for (int j = i; j < n_descriptors; j++) {
        if (i == j)
          beta_matrix(i, j) = beta[beta_count];
        else if (i != j) {
          beta_val = beta[beta_count] / 2;
          beta_matrix(i, j) = beta_val;
          beta_matrix(j, i) = beta_val;
        }
        beta_count++;
      }
    }
    beta_matrices.push_back(beta_matrix);
  }
}

/* ----------------------------------------------------------------------
   grab n values from file fp and put them in list
   values can be several to a line
   only called by proc 0
------------------------------------------------------------------------- */

void ComputeFlareStdAtom::grab(FILE *fptr, int n, double *list) {
  char *ptr;
  char line[MAXLINE];

  int i = 0;
  while (i < n) {
    fgets(line, MAXLINE, fptr);
    ptr = strtok(line, " \t\n\r\f");
    list[i++] = atof(ptr);
    while ((ptr = strtok(NULL, " \t\n\r\f")))
      list[i++] = atof(ptr);
  }
}
