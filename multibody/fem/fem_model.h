#pragma once

#include <array>
#include <memory>
#include <string>
#include <utility>

#include <Eigen/Sparse>

#include "drake/common/default_scalars.h"
#include "drake/common/eigen_types.h"
#include "drake/multibody/fem/fem_state.h"
#include "drake/multibody/fem/petsc_symmetric_block_sparse_matrix.h"

namespace drake {
namespace multibody {
namespace fem {

/** %FemModel calculates the components of the spatially discretized FEM
 equations for dynamic elasticity problems. Typically, in dynamic elasticity
 problems, we are interested in the mapping that describes the motion of a
 material

     ϕ(⋅,t) : Ω⁰ → Ωᵗ,

 where Ω⁰ and Ωᵗ are subsets of R³, along with its first and second derivatives
 (velocity and acceleration respectively):

     V(⋅,t) = ∂ϕ(⋅,t)/∂t,
     A(⋅,t) = ∂²ϕ(⋅,t)/∂t².

 We call Ω⁰ the reference domain and Ωᵗ the current domain. We use upper case
 letters to denote states (positions, velocities, and accelerations) in
 reference domain (X, V, A) and lower case letters to denote their current
 domain counterparts (x, v, a). In particular, x(X,t) = ϕ(X,t). The deformation
 gradient F(X,t) is given by ∂ϕ(X,t)/∂X.

 The governing equations of interest are conservation of mass and conservation
 of momentum:

     R(X,t)J(X,t) = R(X,0),
     R(X,0)A(X,t) = fᵢₙₜ(X,t) + fₑₓₜ(X,t),

 where R is mass density, fᵢₙₜ and fₑₓₜ are internal and external force
 densities respectively, and J is the determinant of the deformation gradient.
 Using finite element method to discretize in space, one gets

     ϕ(X,t) = ∑ᵢ xᵢ(t)Nᵢ(X)
     V(X,t) = ∑ᵢ vᵢ(t)Nᵢ(X)
     A(X,t) = ∑ᵢ aᵢ(t)Nᵢ(X)

 where xᵢ, vᵢ, aᵢ ∈ R³ are nodal values of the spatially discretized position,
 velocity and acceleration, and Nᵢ(X):Ω⁰ → R are the the basis functions. With
 this spatial discretization, the PDE is turned into an ODE of the form

     G(x, v, a) = 0,            (1)

 where x, v, a are the stacked xᵢ, vᵢ, aᵢ. %FemModel provides methods to
 query various information about equation (1) and its derivatives given an FEM
 state (x, v, a).

 We implement %FemModel in FemModelImpl that templatizes on the type of
 FemElement. Many functionalities provided by %FemModel (e.g.
 CalcTangentMatrix()) involve evaluating computationally intensive loops
 over FEM elements, and the overhead caused by virtual methods may be
 significant. We implement those functions in FemModelImpl templated on the
 FemElement to avoid the overhead of virtual methods. The type information at
 compile time also helps eliminate heap allocations.

 Sifakis, Eftychios, and Jernej Barbič. "Finite element method simulation of 3d
 deformable solids." Synthesis Lectures on Visual Computing: Computer Graphics,
 Animation, Computational Photography, and Imaging 1.1 (2015): 1-69.

 @tparam_double_only */
template <typename T>
class FemModel {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(FemModel);

  /** %Builder that builds the FemModel. Each concrete %FemModel must define its
   own builder, subclassing this class, to add new elements to the model. */
  class Builder {
   public:
    DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(Builder);

    virtual ~Builder() = default;

    /** Adds the FEM elements described by calls to this builder to this
     associated FemModel. The builder is left in an invalid state after Build()
     is invoked, and should thus be discarded and not reused again. */
    void Build();

   protected:
    /** Throws an exception if Build() has been called on this %Builder. */
    void ThrowIfBuilt() const;

    /** Constructs a new builder that builds into the given `model`.
     @pre model != nullptr.
     @note The `model` pointer is persisted and the pointed to FemModel must
     outlive `this` *Builder. */
    explicit Builder(FemModel<T>* model) : model_{model} {
      DRAKE_DEMAND(model_ != nullptr);
    }

    /** Derived builders must provide implementations for this function to add
     the FEM elements described by calls to the builder to the associated
     FemModel. */
    virtual void DoBuild() = 0;

   private:
    /* The model that `this` builder builds into. */
    FemModel<T>* model_{nullptr};
    /* Flag to keep track of whether Build() has been called on this builder. */
    bool built_{false};
  };

  virtual ~FemModel() = default;

  /* The `num_dofs()` is always a multiple of 3. It is enforced by
   FemStateSystem. */
  /** The number of nodes that are associated with this model. */
  int num_nodes() const { return num_dofs() / 3; }

  /** The number of degrees of freedom in this model. */
  int num_dofs() const { return fem_state_system_->num_dofs(); }

  /** The number of FEM elements in this model. */
  virtual int num_elements() const = 0;

  /** Creates a default FemState compatible with this model. */
  std::unique_ptr<FemState<T>> MakeFemState() const;

  /** Calculates the residual G(x, v, a) (see class doc) evaluated at the
   given FEM state.
   @pre residual != nullptr.
   @throws std::exception if the FEM state is incompatible with this model. */
  void CalcResidual(const FemState<T>& fem_state,
                    EigenPtr<VectorX<T>> residual) const;

  /** Calculates an approximated tangent matrix evaluated at the given FEM state
   (or an approximation thereof). The tangent matrix is given by a weighted sum
   of stiffness matrix (∂G/∂x), damping matrix (∂G/∂v), and mass matrix (∂G/∂a).
   @param[in] fem_state        The FemState used to evaluate the tangent
                               matrix.
   @param[in] weights          The weights used to combine stiffness, damping,
                               and mass matrices (in that order) into the
                               tangent matrix.
   @param[out] tangent_matrix  The output tangent_matrix.
   @pre tangent_matrix != nullptr.
   @pre The size of `tangent_matrix` is `num_dofs()` * `num_dofs()`.
   @pre All nonzero entries in the resulting tangent matrix have been
   allocated. See MakePetscSymmetricBlockSparseMatrix().
   @warning This function sometimes makes simplifying approximations to avoid
   taking overly complicated derivatives. As such, the resulting tangent
   matrix is usually an approximation of the actual value.
   @throws std::exception if the FEM state is incompatible with this model. */
  void CalcTangentMatrix(
      const FemState<T>& fem_state, const Vector3<T>& weights,
      internal::PetscSymmetricBlockSparseMatrix* tangent_matrix) const;

  /** Creates a PetscSymmetricBlockSparseMatrix that has the sparsity pattern
   of the tangent matrix of this FEM model. In particular, the size of the
   tangent matrix is `num_dofs()` by `num_dofs()`. All entries are initialized
   to zero. */
  std::unique_ptr<internal::PetscSymmetricBlockSparseMatrix>
  MakePetscSymmetricBlockSparseTangentMatrix() const;

 protected:
  /** Constructs an empty FEM model. */
  FemModel();

  /** (Internal use only) Throws std::exception to report a mismatch between
  the FEM model and state that were passed to API method `func`. */
  void ThrowIfModelStateIncompatible(const char* func,
                                     const FemState<T>& fem_state) const;

  /** Returns the reference positions of this model. */
  virtual VectorX<T> MakeReferencePositions() const = 0;

  /** FemModelImpl must override this method to provide an implementation
   for the NVI CalcResidual(). The input `fem_state` is guaranteed to be
   compatible with `this` FEM model, and the input `residual` is guaranteed to
   be non-null. */
  virtual void DoCalcResidual(const FemState<T>& fem_state,
                              EigenPtr<VectorX<T>> residual) const = 0;

  /** FemModelImpl must override this method to provide an implementation for
   the NVI CalcTangentMatrix(). The input `fem_state` is guaranteed to be
   compatible with `this` FEM model, and the input `tangent_matrix` is
   guaranteed to be non-null and properly sized. */
  virtual void DoCalcTangentMatrix(
      const FemState<T>& fem_state, const Vector3<T>& weights,
      internal::PetscSymmetricBlockSparseMatrix* tangent_matrix) const = 0;

  /** FemModelImpl must override this method to provide an implementation for
   the NVI MakePetscSymmetricBlockSparseTangentMatrix(). */
  virtual std::unique_ptr<internal::PetscSymmetricBlockSparseMatrix>
  DoMakePetscSymmetricBlockSparseTangentMatrix() const = 0;

  /** Updates the system that manages the states and the cache entries of this
   FEM model. Must be called before calling MakeFemState() after the FEM model
   changes (e.g. adding new elements). */
  void UpdateFemStateSystem();

  /** Derived classes should override this method to declare cache entries in
   the given `fem_state_system`. */
  virtual void DeclareCacheEntries(
      internal::FemStateSystem<T>* fem_state_system) = 0;

  /** Returns the FemStateSystem that manages the states and cache entries in
   this %FemModel. */
  const internal::FemStateSystem<T>& fem_state_system() const {
    return *fem_state_system_;
  }

 private:
  /* The system that manages the states and cache entries of this FEM model.
   */
  std::unique_ptr<internal::FemStateSystem<T>> fem_state_system_;
};

}  // namespace fem
}  // namespace multibody
}  // namespace drake
