// Author: David Kriebel <dotnotlock@gmail.com>
// Copyright (C) 2018-2021 Yixuan Qiu <yixuan.qiu@cos.name>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef SPECTRA_KRYLOVSCHURGEIGSBASE_H
#define SPECTRA_KRYLOVSCHURGEIGSBASE_H

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <Eigen/Jacobi>

#include <vector>     // std::vector
#include <cmath>      // std::abs, std::pow
#include <algorithm>  // std::min
#include <stdexcept>  // std::invalid_argument
#include <utility>    // std::move
#include <complex>    // std::complex

#include "Util/Version.h"
#include "Util/TypeTraits.h"
#include "Util/SelectionRule.h"
#include "Util/CompInfo.h"
#include "Util/SimpleRandom.h"
#include "MatOp/internal/ArnoldiOp.h"
#include "LinAlg/UpperHessenbergQR.h"
#include "LinAlg/TridiagEigen.h"
#include "LinAlg/KrylovSchur.h"

namespace Spectra {

    ///
    /// \defgroup KrylovSchur Eigen Solvers
    ///
    /// Eigen solvers for different types of problems.
    ///

    ///
    /// \ingroup KrylovSchur
    ///
    /// This is the base class for symmetric eigen solvers, mainly for internal use.
    /// It is kept here to provide the documentation for member functions of concrete eigen solvers
    /// such as SymEigsSolver and SymEigsShiftSolver.
	/// 
	/// This code is based on MATLAB's "eigs" function which provides a very stable implementation of the Krylov-Schur eigenvalue extraction
	/// References:
	/// [1] Stewart, G.W. "A Krylov-Schur Algorithm for Large Eigenproblems." SIAM Journal of Matrix Analysis and Applications. Vol. 23, Issue 3, 2001, pp. 601–614.
	/// [2] Lehoucq, R.B., D.C. Sorenson, and C. Yang. ARPACK Users' Guide. Philadelphia, PA: SIAM, 1998.
	/// [3] https://de.mathworks.com/help/matlab/ref/eigs.html
	/// 
	/// Variable names were merged to fit in Spectra formatting.
	/// The Arnoldi class was modified a little bit according to MATLAB's code and implemented in the KrylovSchur class.
    ///
    template <typename OpType, typename BOpType>
    class KrylovSchurGEigsBase
    {
    private:
        using Scalar = typename OpType::Scalar;
        using Index = Eigen::Index;
        using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
        using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;

        using Complex = std::complex<Scalar>;
        using ComplexMatrix = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;
        using ComplexVector = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;

        using Array = Eigen::Array<Scalar, Eigen::Dynamic, 1>;
        using BoolArray = Eigen::Array<bool, Eigen::Dynamic, 1>;
        using MapMat = Eigen::Map<Matrix>;
        using MapVec = Eigen::Map<Vector>;
        using MapConstVec = Eigen::Map<const Vector>;

        using ArnoldiOpType = ArnoldiOp<Scalar, OpType, BOpType>;
        using KrylovFac = KrylovSchur<Scalar, ArnoldiOpType>;

    protected:
        // clang-format off
		
        std::vector<OpType> m_op_container;
        const OpType& m_op;         // matrix operator for A
        const Index   m_n;          // dimension of matrix A
        const Index   m_nev;        // number of eigenvalues requested
        const Index   m_ncv;        // dimension of Krylov subspace in the Lanczos method
        Index         m_nmatop;     // number of matrix operations called
        Index         m_niter;      // number of restarting iterations

        KrylovFac     m_fac;        // Lanczos factorization
        Vector        m_evals;      // Eigen values

    private:
        Matrix        m_evecs;       // Eigen vectors
        BoolArray     m_evals_conv;  // indicator of the convergence of Ritz values
        CompInfo      m_info;       // status of the computation
        // clang-format on

        // Move rvalue object to the container
        static std::vector<OpType> create_op_container(OpType&& rval)
        {
            std::vector<OpType> container;
            container.emplace_back(std::move(rval));
            return container;
        }

        // Calculates the number of converged eigenvalues
        Index num_converged(const Scalar& tol, ComplexVector& evals, Vector& res)
        {
            using std::pow;

            // The machine precision, ~= 1e-16 for the "double" type
            constexpr Scalar eps = TypeTraits<Scalar>::epsilon();
            // std::pow() is not constexpr, so we do not declare eps23 to be constexpr
            // But most compilers should be able to compute eps23 at compile time
            const Scalar eps23 = pow(eps, Scalar(2) / 3);

            // thresh = tol * max(eps23, abs(theta)), theta for Ritz value
            Array thresh = tol * evals.head(m_nev).array().abs().max(eps23);
            // Converged "wanted" Ritz values
            m_evals_conv = (res.head(m_nev).array() < thresh);

            return m_evals_conv.count();
        }

        // Returns the adjusted nev for restarting
        Index nev_adjusted(Index nconv, Index nconvold)
        {
            // Adjust nev to prevent stagnating (see reference 2)
            Index nev_new = m_nev + (std::min) (nconv, (m_ncv - m_nev) / 2); // k = k0 + min(nconv, floor((p - k0) / 2));
            if (nev_new == 1 && m_ncv > 3)
                nev_new = m_ncv / 2;

            // Lola's heuristic
            if (nev_new + 1 < m_ncv && nconvold > nconv)
                nev_new += 1;

            return nev_new;
        }

        // Permute/Reorder Schur decomposition in U and T according to permutation --> see Reference https://github.com/libigl/eigen/blob/master/unsupported/Eigen/src/MatrixFunctions/MatrixFunction.h
        void ordschur(Matrix& U, Matrix& T, BoolArray& select)
        {
			using std::swap;
			
            // build permutation vector
            Vector permutation(select.size());
            Index ind = 0;
            for (Index j = 0; j < select.size(); j++)
            {
                if (select(j))
                {
                    permutation(j) = ind;
                    ind++;
                }
            }
            for (Index j = 0; j < select.size(); j++)
            {
                if (!select(j))
                {
                    permutation(j) = ind;
                    ind++;
                }
            }

            for (Index i = 0; i < permutation.size() - 1; i++)
            {
                Index j;
                for (j = i; j < permutation.size(); j++)
                {
                    if (permutation(j) == i)
                        break;
                }
                eigen_assert(permutation(j) == i);
                for (Index k = j - 1; k >= i; k--)
                {
                    Eigen::JacobiRotation<Matrix::Scalar> rotation;
                    rotation.makeGivens(T(k, k + 1), T(k + 1, k + 1) - T(k, k));
                    T.applyOnTheLeft(k, k + 1, rotation.adjoint());
                    T.applyOnTheRight(k, k + 1, rotation);
                    U.applyOnTheRight(k, k + 1, rotation);
                    swap(permutation.coeffRef(k), permutation.coeffRef(k + 1));
                }
            }
        }

    protected:
        // Sort eigenvalues and returns a sorting index vector
        virtual std::vector<Index> which_eigenvalues(ComplexVector& evals, SortRule sort_rule)
        {
            // Sort Ritz values and put the wanted ones at the beginning
            std::vector<Index> ind;
            switch (sort_rule)
            {
                case SortRule::LargestMagn:
                {
                    SortEigenvalue<Complex, SortRule::LargestMagn> sorting(evals.data(), m_ncv);
                    sorting.swap(ind);
                    break;
                }
                case SortRule::LargestReal:
                {
                    SortEigenvalue<Complex, SortRule::LargestReal> sorting(evals.data(), m_ncv);
                    sorting.swap(ind);
                    break;
                }
                case SortRule::LargestImag:
                {
                    SortEigenvalue<Complex, SortRule::LargestImag> sorting(evals.data(), m_ncv);
                    sorting.swap(ind);
                    break;
                }
                case SortRule::SmallestMagn:
                {
                    SortEigenvalue<Complex, SortRule::SmallestMagn> sorting(evals.data(), m_ncv);
                    sorting.swap(ind);
                    break;
                }
                case SortRule::SmallestReal:
                {
                    SortEigenvalue<Complex, SortRule::SmallestReal> sorting(evals.data(), m_ncv);
                    sorting.swap(ind);
                    break;
                }
                case SortRule::SmallestImag:
                {
                    SortEigenvalue<Complex, SortRule::SmallestImag> sorting(evals.data(), m_ncv);
                    sorting.swap(ind);
                    break;
                }
                default:
                    throw std::invalid_argument("unsupported selection rule");
            }
            return ind;
        }

    public:
        /// \cond

        // If op is an lvalue
        KrylovSchurGEigsBase(OpType& op, const BOpType& Bop, Index nev, Index ncv) :
            m_op(op),
            m_n(op.rows()),
            m_nev(nev),
            m_ncv(ncv > m_n ? m_n : ncv),
            m_nmatop(0),
            m_niter(0),
            m_fac(ArnoldiOpType(op, Bop), m_ncv),
            m_info(CompInfo::NotComputed)
        {
            if (nev < 1 || nev > m_n - 1)
                throw std::invalid_argument("nev must satisfy 1 <= nev <= n - 1, n is the size of matrix");

            if (ncv <= nev || ncv > m_n)
                throw std::invalid_argument("ncv must satisfy nev < ncv <= n, n is the size of matrix");
        }

        // If op is an rvalue
        KrylovSchurGEigsBase(OpType&& op, const BOpType& Bop, Index nev, Index ncv) :
            m_op_container(create_op_container(std::move(op))),
            m_op(m_op_container.front()),
            m_n(m_op.rows()),
            m_nev(nev),
            m_ncv(ncv > m_n ? m_n : ncv),
            m_nmatop(0),
            m_niter(0),
            m_fac(ArnoldiOpType(m_op, Bop), m_ncv),
            m_info(CompInfo::NotComputed)
        {
            if (nev < 1 || nev > m_n - 1)
                throw std::invalid_argument("nev must satisfy 1 <= nev <= n - 1, n is the size of matrix");

            if (ncv <= nev || ncv > m_n)
                throw std::invalid_argument("ncv must satisfy nev < ncv <= n, n is the size of matrix");
        }

        ///
        /// Virtual destructor
        ///
        virtual ~KrylovSchurGEigsBase() {}

        /// \endcond

        ///
        /// Initializes the solver by providing an initial residual vector.
        ///
        /// \param init_resid Pointer to the initial residual vector.
        ///
        /// **Spectra** (and also **ARPACK**) uses an iterative algorithm
        /// to find eigenvalues. This function allows the user to provide the initial
        /// residual vector.
        ///
        void init(const Scalar* init_resid)
        {
            // Reset all matrices/vectors to zero
            m_evals.resize(m_ncv);
            m_evecs.resize(m_ncv, m_nev);
            m_evals_conv.resize(m_nev);

            m_evals.setZero();
            m_evecs.setZero();
            m_evals_conv.setZero();

            m_nmatop = 0;
            m_niter = 0;

            // Initialize the Lanczos factorization
            MapConstVec v0(init_resid, m_n);
            m_fac.init(v0, m_nmatop);
        }

        ///
        /// Initializes the solver by providing a random initial residual vector.
        ///
        /// This overloaded function generates a random initial residual vector
        /// (with a fixed random seed) for the algorithm. Elements in the vector
        /// follow independent Uniform(-0.5, 0.5) distribution.
        ///
        void init()
        {
            SimpleRandom<Scalar> rng(0);
            Vector init_resid = rng.random_vec(m_n);
            init(init_resid.data());
        }

        ///
        /// Conducts the major computation procedure.
        ///
        /// \param selection  An enumeration value indicating the selection rule of
        ///                   the requested eigenvalues, for example `SortRule::LargestMagn`
        ///                   to retrieve eigenvalues with the largest magnitude.
        ///                   The full list of enumeration values can be found in
        ///                   \ref Enumerations.
        /// \param maxit      Maximum number of iterations allowed in the algorithm.
        /// \param tol        Precision parameter for the calculated eigenvalues.
        /// \param sorting    Rule to sort the eigenvalues and eigenvectors.
        ///                   Supported values are
        ///                   `SortRule::LargestAlge`, `SortRule::LargestMagn`,
        ///                   `SortRule::SmallestAlge`, and `SortRule::SmallestMagn`.
        ///                   For example, `SortRule::LargestAlge` indicates that largest eigenvalues
        ///                   come first. Note that this argument is only used to
        ///                   **sort** the final result, and the **selection** rule
        ///                   (e.g. selecting the largest or smallest eigenvalues in the
        ///                   full spectrum) is specified by the parameter `selection`.
        ///
        /// \return Number of converged eigenvalues.
        ///
        Index compute(SortRule selection = SortRule::LargestMagn, Index maxit = 1000,
            Scalar tol = 1e-10, SortRule sorting = SortRule::LargestAlge)
        {
            bool stopAlgorithm = false;
            Index nev_new = m_nev;  // (variable nev_new will be adaptively increased)
            Index sizeV = 0;
            std::vector<Index> ind(m_ncv);
            ComplexVector d(m_ncv);
            ComplexMatrix U(m_ncv, m_ncv);

            Index i, nconv = 0;
            for (i = 0; i < maxit; i++)
            {
                // The m-step Lanczos factorization
                stopAlgorithm = m_fac.factorize_from(sizeV, m_ncv, m_nmatop);

				if (stopAlgorithm)
				{
					m_evals.resize(0);
					m_evecs.resize(0, 0);
					return 0;
				}
				
                //// Should we expect conjugate pairs ?
                Matrix H(m_fac.matrix_H());
                bool isrealprob = true;
                //isrealprob = H.real(); // check if H is a real matrix

                // Schur Decomposition
                // Returns 2x2 block form if H is real
                Eigen::RealSchur<Matrix> schur(H.topLeftCorner(m_ncv, m_ncv));
                Matrix X(schur.matrixU());
                Matrix T(schur.matrixT());

                // Compute eigenvalues
                Eigen::EigenSolver<Matrix> eig(T);
                d.noalias() = eig.eigenvalues();
                U.noalias() = X * eig.eigenvectors();

                // Implicitly calculate residuals
                Vector res((H.bottomRows(1) * U).transpose().cwiseAbs());

                // Sort eigenvalues and residuals
                ind = which_eigenvalues(d, selection);  // ind = whichEigenvalues(d, method);
                Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic> reorder(ind.size());
                reorder.indices() = Eigen::Map<Eigen::Vector<Index, Eigen::Dynamic>>(ind.data(), ind.size()).cast<int>();
                d.noalias() = reorder.inverse() * d; // d = d(ind);
                res.noalias() = reorder.inverse() * res;  // res = res(ind);

                // Number of converged eigenpairs : 
                Index nconvold = nconv;
                nconv = num_converged(tol, d, res);
                
                if (nconv >= m_nev || i == maxit)
                {
                    // Stop the algorithm now
                    break;
                }
                else
                {
                    // Adjust k to prevent stagnating (see reference 2)
                    nev_new = nev_adjusted(nconv, nconvold);
                }
                
                // Get original ordering of eigenvalues back
                d.noalias() = T.diagonal();
                
                // Choose desired eigenvalues in d to create a Boolean select vector
                ind = which_eigenvalues(d, selection);
                Eigen::Map< Eigen::Vector<Index, Eigen::Dynamic> > ind_sel(ind.data(), ind.size());
                BoolArray select(m_ncv);
                select.setConstant(false);
                select( ind_sel.head(nev_new) ).setConstant(true);

                // Make sure both parts of a conjugate pair are present
                if (isrealprob)
                {
                    for(auto it=ind_sel.begin(); it!=ind_sel.end(); it++)
                    {
                        int i = *it;
                        if (i+1 < m_ncv && T(i + 1, i) != 0 && !select(i + 1))
                        {
                            select(i + 1) = true;
                            nev_new = nev_new + 1;
                        }
                        if (i > 0 && T(i, i - 1) != 0 && !select(i - 1))
                        {
                            select(i - 1) = true;
                            nev_new = nev_new + 1;
                        }
                    }
                }
                
                // Reorder X and T based on select
                ordschur(X, T, select);  // [X, T] = ordschur(X, T, select);

                // Store variables for next iteration
                MapMat Xk(X.data(), m_ncv, nev_new);  // X(:, 1:k)

                // H = [T(1:k, 1:k); H(end, :) * Xk];
                H.topLeftCorner(nev_new, nev_new).noalias() = T.topLeftCorner(nev_new, nev_new); 
                H.block(nev_new, 0, 1, nev_new).noalias() = H.bottomRows(1) * Xk;

                Matrix V(m_fac.matrix_V());
                V.leftCols(nev_new) = V * Xk;  // V(:,1:k) = V * Xk;

                m_fac.swap_H(H);
                m_fac.swap_V(V);

                sizeV = nev_new; // sizeV = k + 1;
            }
            
            // export eigenvalues and eigenvectors
            m_evals.resize(m_nev);
            m_evals.noalias() = d.topRows(m_nev).real();

            m_evecs.resize(m_n, m_nev);
            Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic> reorder(ind.size());
            reorder.indices() = Eigen::Map<Eigen::Vector<Index, Eigen::Dynamic>>(ind.data(), ind.size()).cast<int>();
            U.noalias() = U * reorder;  // d = d(ind);
            m_evecs.noalias() = (m_fac.matrix_V() * U.leftCols(m_nev)).real();

            m_niter += i + 1;
            m_info = (nconv >= m_nev) ? CompInfo::Successful : CompInfo::NotConverging;

            return (std::min)(m_nev, nconv);
        }

        ///
        /// Returns the status of the computation.
        /// The full list of enumeration values can be found in \ref Enumerations.
        ///
        CompInfo info() const { return m_info; }

        ///
        /// Returns the number of iterations used in the computation.
        ///
        Index num_iterations() const { return m_niter; }

        ///
        /// Returns the number of matrix operations used in the computation.
        ///
        Index num_operations() const { return m_nmatop; }

        ///
        /// Returns the converged eigenvalues.
        ///
        /// \return A vector containing the eigenvalues.
        /// Returned vector type will be `Eigen::Vector<Scalar, ...>`, depending on
        /// the template parameter `Scalar` defined.
        ///
        Vector eigenvalues() const
        {
            return m_evals;
        }

        ///
        /// Returns the eigenvectors associated with the converged eigenvalues.
        ///
        /// \param nvec The number of eigenvectors to return.
        ///
        /// \return A matrix containing the eigenvectors.
        /// Returned matrix type will be `Eigen::Matrix<Scalar, ...>`,
        /// depending on the template parameter `Scalar` defined.
        ///
        virtual Matrix eigenvectors(Index nvec) const
        {
            const Index nconv = m_evals_conv.count();
            nvec = (std::min)(nvec, nconv);
            Matrix res(m_n, nvec);

            return m_fac.matrix_V().leftCols(nvec);
        }

        ///
        /// Returns all converged eigenvectors.
        ///
        virtual Matrix eigenvectors() const
        {
            return eigenvectors(m_nev);
        }
    };

}  // namespace Spectra

#endif  // SPECTRA_KRYLOVSCHURGEIGSBASE_H
