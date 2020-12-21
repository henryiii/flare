import numpy as np
from _C_flare import SparseGP, Structure
from scipy.optimize import minimize
from typing import List


class SGP_Wrapper:
    """Wrapper class used to make the C++ sparse GP object compatible with
    OTF. Methods and properties are designed to mirror the GP class."""

    def __init__(
        self,
        kernels: List,
        descriptor_calculators: List,
        cutoff: float,
        sigma_e: float,
        sigma_f: float,
        sigma_s: float,
        species_map: dict,
        variance_type: str = "SOR",
        single_atom_energies: dict = None,
        energy_training=True,
        force_training=True,
        stress_training=True,
        max_iterations=10,
        opt_type="all"
    ):

        self.sparse_gp = SparseGP(kernels, sigma_e, sigma_f, sigma_s)
        self.descriptor_calculators = descriptor_calculators
        self.cutoff = cutoff
        self.hyps_mask = None
        self.species_map = species_map
        self.variance_type = variance_type
        self.single_atom_energies = single_atom_energies
        self.energy_training = energy_training
        self.force_training = force_training
        self.stress_training = stress_training
        self.max_iterations = max_iterations
        self.opt_type = opt_type

        # Make placeholder hyperparameter labels.
        self.hyp_labels = []
        for n in range(len(self.hyps)):
            self.hyp_labels.append("Hyp" + str(n))

    @property
    def training_data(self):
        return self.sparse_gp.training_structures

    @property
    def hyps(self):
        return self.sparse_gp.hyperparameters

    @property
    def hyps_and_labels(self):
        return self.hyps, self.hyp_labels

    @property
    def likelihood(self):
        return self.sparse_gp.log_marginal_likelihood

    @property
    def likelihood_gradient(self):
        return self.sparse_gp.likelihood_gradient

    @property
    def force_noise(self):
        return self.sparse_gp.force_noise

    def __str__(self):
        return "Sparse GP model"

    def check_L_alpha(self):
        pass

    def write_model(self, name: str):
        pass

    def update_db(
        self,
        structure,
        forces,
        custom_range=(),
        energy: float = None,
        stress: "ndarray" = None,
    ):

        # Convert coded species to 0, 1, 2, etc.
        coded_species = []
        for spec in structure.coded_species:
            coded_species.append(self.species_map[spec])

        # Convert flare structure to structure descriptor.
        structure_descriptor = Structure(
            structure.cell,
            coded_species,
            structure.positions,
            self.cutoff,
            self.descriptor_calculators,
        )

        # Add labels to structure descriptor.
        if (energy is not None) and (self.energy_training):
            # Sum up single atom energies.
            single_atom_sum = 0
            if self.single_atom_energies is not None:
                for spec in coded_species:
                    single_atom_sum += self.single_atom_energies[spec]

            # Correct the energy label and assign to structure.
            corrected_energy = energy - single_atom_sum
            structure_descriptor.energy = np.array([[corrected_energy]])

        if (forces is not None) and (self.force_training):
            structure_descriptor.forces = forces.reshape(-1)

        if (stress is not None) and (self.stress_training):
            structure_descriptor.stresses = stress

        # Update the sparse GP.
        self.sparse_gp.add_training_structure(structure_descriptor)
        self.sparse_gp.add_all_environments(structure_descriptor)
        self.sparse_gp.update_matrices_QR()

    def set_L_alpha(self):
        # Taken care of in the update_db method.
        pass

    def train(self, logger_name=None):
        if self.opt_type == "all":
            optimize_hyperparameters(self.sparse_gp,
                                     max_iterations=self.max_iterations)
        elif self.opt_type == "freeze_noise":
            optimize_kernel_hyperparameters(
                self.sparse_gp, max_iterations=self.max_iterations)


def compute_negative_likelihood(hyperparameters, sparse_gp):
    """Compute the negative log likelihood and gradient with respect to the
    hyperparameters."""

    assert len(hyperparameters) == len(sparse_gp.hyperparameters)

    sparse_gp.set_hyperparameters(hyperparameters)
    sparse_gp.compute_likelihood()
    negative_likelihood = -sparse_gp.log_marginal_likelihood

    print_hyps(hyperparameters, negative_likelihood)

    return negative_likelihood


def compute_neglike_fixed_noise(hyperparameters, sparse_gp, noise_hyps):
    """Compute the negative log likelihood and gradient with respect to the
    hyperparameters."""

    assert len(hyperparameters) == len(sparse_gp.hyperparameters) - 3
    all_hyps = np.concatenate((hyperparameters, noise_hyps))

    sparse_gp.set_hyperparameters(all_hyps)
    sparse_gp.compute_likelihood()
    negative_likelihood = -sparse_gp.log_marginal_likelihood

    print_hyps(hyperparameters, negative_likelihood)

    return negative_likelihood


def compute_negative_likelihood_grad(hyperparameters, sparse_gp):
    """Compute the negative log likelihood and gradient with respect to the
    hyperparameters."""

    assert len(hyperparameters) == len(sparse_gp.hyperparameters)

    negative_likelihood = -sparse_gp.compute_likelihood_gradient(hyperparameters)
    negative_likelihood_gradient = -sparse_gp.likelihood_gradient

    print_hyps_and_grad(hyperparameters, negative_likelihood_gradient,
                        negative_likelihood)

    return negative_likelihood, negative_likelihood_gradient


def compute_neglike_grad_fixed_noise(hyperparameters, sparse_gp, noise_hyps):
    assert len(hyperparameters) == len(sparse_gp.hyperparameters) - 3
    all_hyps = np.concatenate((hyperparameters, noise_hyps))

    negative_likelihood = -sparse_gp.compute_likelihood_gradient(all_hyps)
    negative_likelihood_gradient = -sparse_gp.likelihood_gradient[:-3]

    print_hyps_and_grad(hyperparameters, negative_likelihood_gradient,
                        negative_likelihood)

    return negative_likelihood, negative_likelihood_gradient


def print_hyps(hyperparameters, neglike):
    print("Hyperparameters:")
    print(hyperparameters)
    print("Likelihood:")
    print(-neglike)
    print("\n")


def print_hyps_and_grad(hyperparameters, neglike_grad, neglike):
    print("Hyperparameters:")
    print(hyperparameters)
    print("Likelihood gradient:")
    print(-neglike_grad)
    print("Likelihood:")
    print(-neglike)
    print("\n")


def optimize_kernel_hyperparameters(
    sparse_gp, display_results=True, gradient_tolerance=1e-4,
    max_iterations=10, method="BFGS"
):
    # Optimize the hyperparameters with BFGS.
    initial_guess = sparse_gp.hyperparameters[:-3]
    noise_hyps = sparse_gp.hyperparameters[-3:]
    arguments = (sparse_gp, noise_hyps)

    if method == "BFGS":
        optimization_result = minimize(
            compute_neglike_grad_fixed_noise,
            initial_guess,
            arguments,
            method="BFGS",
            jac=True,
            options={
                "disp": display_results,
                "gtol": gradient_tolerance,
                "maxiter": max_iterations,
            },
        )

        # Assign likelihood gradient.
        sparse_gp.likelihood_gradient = -optimization_result.jac

    elif method == "nelder-mead":
        optimization_result = minimize(
            compute_neglike_fixed_noise,
            initial_guess,
            arguments,
            method="nelder-mead",
            options={
                "maxiter": max_iterations,
            }
        )

    # Set the hyperparameters to the optimal value.
    new_hyps = np.copy(sparse_gp.hyperparameters)
    new_hyps[:-3] = optimization_result.x
    sparse_gp.set_hyperparameters(new_hyps)
    sparse_gp.log_marginal_likelihood = -optimization_result.fun

    return optimization_result


def optimize_hyperparameters(
    sparse_gp, display_results=True, gradient_tolerance=1e-4,
    max_iterations=10, method="BFGS"
):
    """Optimize the hyperparameters of a sparse GP model."""

    initial_guess = sparse_gp.hyperparameters
    arguments = sparse_gp

    if method == "BFGS":
        optimization_result = minimize(
            compute_negative_likelihood_grad,
            initial_guess,
            arguments,
            method="BFGS",
            jac=True,
            options={
                "disp": display_results,
                "gtol": gradient_tolerance,
                "maxiter": max_iterations,
            },
        )

        # Assign likelihood gradient.
        sparse_gp.likelihood_gradient = -optimization_result.jac

    elif method == "nelder-mead":
        optimization_result = minimize(
            compute_negative_likelihood,
            initial_guess,
            arguments,
            method="nelder-mead",
            options={
                "maxiter": max_iterations,
            }
        )

    # Set the hyperparameters to the optimal value.
    sparse_gp.set_hyperparameters(optimization_result.x)
    sparse_gp.log_marginal_likelihood = -optimization_result.fun

    return optimization_result


if __name__ == "__main__":
    pass
