#include "Viscosity_Bender2017.h"
#include "SPlisHSPlasH/TimeManager.h"
#include "Utilities/Counting.h"
#include "../Simulation.h"

using namespace SPH;
using namespace GenParam;

int Viscosity_Bender2017::ITERATIONS = -1;
int Viscosity_Bender2017::MAX_ITERATIONS = -1;
int Viscosity_Bender2017::MAX_ERROR = -1;


Viscosity_Bender2017::Viscosity_Bender2017(FluidModel *model) :
	ViscosityBase(model)
{
	m_targetStrainRate.resize(model->numParticles(), Vector6r::Zero());
	m_viscosityFactor.resize(model->numParticles(), Matrix6r::Zero());
	m_viscosityLambda.resize(model->numParticles(), Vector6r::Zero());

	m_iterations = 0;
	m_maxIter = 50;
	m_maxError = 0.01;
}

Viscosity_Bender2017::~Viscosity_Bender2017(void)
{
	m_targetStrainRate.clear();
	m_viscosityFactor.clear();
	m_viscosityLambda.clear();
}

void Viscosity_Bender2017::initParameters()
{
	ViscosityBase::initParameters();

	ITERATIONS = createNumericParameter("viscoIterations", "Iterations", &m_iterations);
	setGroup(ITERATIONS, "Viscosity");
	setDescription(ITERATIONS, "Iterations required by the viscosity solver.");
	getParameter(ITERATIONS)->setReadOnly(true);

	MAX_ITERATIONS = createNumericParameter("viscoMaxIter", "Max. iterations (visco)", &m_maxIter);
	setGroup(MAX_ITERATIONS, "Viscosity");
	setDescription(MAX_ITERATIONS, "Coefficient for the viscosity force computation");
	static_cast<NumericParameter<unsigned int>*>(getParameter(MAX_ITERATIONS))->setMinValue(1);

	MAX_ERROR = createNumericParameter("viscoMaxError", "Max. visco error", &m_maxError);
	setGroup(MAX_ERROR, "Viscosity");
	setDescription(MAX_ERROR, "Coefficient for the viscosity force computation");
	RealParameter* rparam = static_cast<RealParameter*>(getParameter(MAX_ERROR));
	rparam->setMinValue(1e-6);
}

void Viscosity_Bender2017::step()
{
	Simulation *sim = Simulation::getCurrent();
	const int numParticles = (int) m_model->numActiveParticles();
	const unsigned int nFluids = sim->numberOfFluidModels();
	FluidModel *model = m_model;
	const unsigned int fluidModelIndex = model->getPointSetIndex();
	const unsigned int maxIter = m_maxIter;
	const Real maxError = m_maxError;	
	const Real maxError2 = maxError*maxError;

	const Real h = TimeManager::getCurrent()->getTimeStepSize();

	// Compute factors
	computeViscosityFactor();
	computeTargetStrainRate();

	m_iterations = 0;
	while (m_iterations < maxIter)
	{
		// Compute viscosity constraint value
		#pragma omp parallel default(shared)
		{
			#pragma omp for schedule(static) nowait 
			for (int i = 0; i < numParticles; i++)
			{
				const Vector3r &xi = m_model->getPosition(i);
				const Vector3r &vi = m_model->getVelocity(i);
				const Real density_i = m_model->getDensity(i);

				Vector6r viscosityC;
				viscosityC.setZero();

				//////////////////////////////////////////////////////////////////////////
				// Fluid
				//////////////////////////////////////////////////////////////////////////
				forall_fluid_neighbors_in_same_phase(
					const Vector3r &vj = m_model->getVelocity(neighborIndex);
					const Vector3r gradW = sim->gradW(xi - xj);
					const Vector3r vji = vj - vi;

					const Real m = m_model->getMass(neighborIndex);
					const Real m2 = m * 2.0;
					viscosityC[0] += m2 * vji[0] * gradW[0];
					viscosityC[1] += m2 * vji[1] * gradW[1];
					viscosityC[2] += m2 * vji[2] * gradW[2];
					viscosityC[3] += m * (vji[0] * gradW[1] + vji[1] * gradW[0]);
					viscosityC[4] += m * (vji[0] * gradW[2] + vji[2] * gradW[0]);
					viscosityC[5] += m * (vji[1] * gradW[2] + vji[2] * gradW[1]);
				)

				viscosityC = (0.5 / density_i) * viscosityC - getTargetStrainRate(i);
 
				getViscosityLambda(i) = viscosityC;
			}
		}
 
		Real avgStrainRateError = 0.0;
		for (int i = 0; i < (int)numParticles; i++)
			for (unsigned int j = 0; j < 6; j++)
				avgStrainRateError += fabs(getViscosityLambda(i)[j]);
		avgStrainRateError = (1.0 / (6.0 *(Real)numParticles)) * avgStrainRateError;

		// Compute viscosity constraint value
		#pragma omp parallel default(shared)
		{
			#pragma omp for schedule(static) nowait 
			for (int i = 0; i < (int)numParticles; i++)
			{
				getViscosityLambda(i) = -getViscosityFactor(i) * getViscosityLambda(i);
			}
		}

		
		// Apply impulses
		#pragma omp parallel default(shared)
		{
			#pragma omp for schedule(static) nowait 
			for (int i = 0; i < numParticles; i++)
			{
				const Vector3r &xi = m_model->getPosition(i);
				Vector3r &vi = m_model->getVelocity(i);
				const Real density_i = m_model->getDensity(i);


				//////////////////////////////////////////////////////////////////////////
				// Fluid
				//////////////////////////////////////////////////////////////////////////
				Eigen::Matrix<Real, 3, 6> gradT;
				forall_fluid_neighbors_in_same_phase(
					const Vector3r gradW = sim->gradW(xi - xj);
					const Real density_j = m_model->getDensity(neighborIndex);

 					gradT.setZero();
 					gradT(0,0) = 2.0 * gradW[0];
 					gradT(0,3) = gradW[1];
 					gradT(0,4) = gradW[2];
 
 					gradT(1,1) = 2.0 * gradW[1];
 					gradT(1,3) = gradW[0];
 					gradT(1,5) = gradW[2];
 
 					gradT(2,2) = 2.0 * gradW[2];
 					gradT(2,4) = gradW[0];
 					gradT(2,5) = gradW[1];
 
 					vi -= m_model->getMass(neighborIndex)* 0.5 * gradT * ((m_model->getMass(neighborIndex) / (density_i*density_i)) * getViscosityLambda(i) +
 						(m_model->getMass(neighborIndex) / (density_j*density_j)) * getViscosityLambda(neighborIndex));
				)
			}
		}
		m_iterations++;

		if (avgStrainRateError < maxError)
			break;
		
	}
	INCREASE_COUNTER("Visco iterations", m_iterations);

	// Compute viscosity forces (XSPH) with boundary to simulate simple friction
	const Real invH = (1.0 / h);
	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static)  
		for (int i = 0; i < (int)numParticles; i++)
		{
			const Vector3r &xi = m_model->getPosition(i);
			const Vector3r &vi = m_model->getVelocity(i);
			Vector3r &ai = m_model->getAcceleration(i);
			const Real density_i = m_model->getDensity(i);

			//////////////////////////////////////////////////////////////////////////
			// Boundary
			//////////////////////////////////////////////////////////////////////////
			forall_boundary_neighbors(
				const Vector3r &vj = bm_neighbor->getVelocity(neighborIndex);
				ai -= invH * 0.1 * m_viscosity * (bm_neighbor->getBoundaryPsi(neighborIndex) / density_i) * (vi - vj)* sim->W(xi - xj);
			)
		}
	}
}


void Viscosity_Bender2017::reset()
{
}


void Viscosity_Bender2017::computeViscosityFactor()
{
	//////////////////////////////////////////////////////////////////////////
	// Init parameters
	//////////////////////////////////////////////////////////////////////////

	Simulation *sim = Simulation::getCurrent();
	const int numParticles = (int) m_model->numActiveParticles();
	const unsigned int nFluids = sim->numberOfFluidModels();
	const FluidModel *model = m_model;
	const unsigned int fluidModelIndex = model->getPointSetIndex();

	#pragma omp parallel default(shared)
	{
		//////////////////////////////////////////////////////////////////////////
		// Compute inverted viscosity matrix
		//////////////////////////////////////////////////////////////////////////

		#pragma omp for schedule(static)  
		for (int i = 0; i < numParticles; i++)
		{
			//////////////////////////////////////////////////////////////////////////
			// Compute viscosity matrix
			//////////////////////////////////////////////////////////////////////////
			const Vector3r &xi = m_model->getPosition(i);
			const Real density_i = m_model->getDensity(i);
			Matrix6r &Kinv = getViscosityFactor(i);
			Matrix6r K;
			K.setZero();
	
			Eigen::Matrix<Real, 6, 3> grad_i;
			grad_i.setZero();

			//////////////////////////////////////////////////////////////////////////
			// Fluid
			//////////////////////////////////////////////////////////////////////////
			Eigen::Matrix<Real, 6, 3> grad_j;
			forall_fluid_neighbors_in_same_phase(
				const Vector3r gradW = sim->gradW(xi - xj);

				grad_j.setZero();
				grad_j(0,0) = 2.0 * gradW[0];
				grad_j(3,0) = gradW[1];
				grad_j(4,0) = gradW[2];

				grad_j(1,1) = 2.0 * gradW[1];
				grad_j(3,1) = gradW[0];
				grad_j(5,1) = gradW[2];

				grad_j(2,2) = 2.0 * gradW[2];
				grad_j(4,2) = gradW[0];
				grad_j(5,2) = gradW[1];

				grad_j = ((0.5 / density_i) * m_model->getMass(neighborIndex)) * grad_j;
				grad_i -= grad_j;

				Matrix6r Klocal;
				viscoGradientMultTransposeRightOpt((1.0 / m_model->getDensity(i)) * grad_j, grad_j, Klocal);
				K += Klocal;
			)


			Matrix6r Klocal;
			viscoGradientMultTransposeRightOpt((1.0 / m_model->getDensity(i)) * grad_i, grad_i, Klocal);
			K += Klocal;
			
			Vector6r Kdiag_inv;
			for (unsigned l = 0; l < 6; l++)
			{

				if (fabs(K(l,l)) < 1.0e-6)
					Kdiag_inv[l] = 1.0;
				else
					Kdiag_inv[l] = 1.0 / K(l,l);
			}
			Matrix6r precondK;
			for (unsigned k = 0; k < 6; k++)
			{
				for (unsigned l = 0; l < 6; l++)
				{
					precondK(k,l) = Kdiag_inv[k] * K(k,l);
				}
			}

			if (fabs(precondK.determinant()) < 1.0e-6)
				Kinv.setZero();
			else
				Kinv = precondK.inverse();

			for (unsigned k = 0; k < 6; k++)
			{
				for (unsigned l = 0; l < 6; l++)
				{
					Kinv(k,l) = Kinv(k,l) * Kdiag_inv[l];
				}
			}
		}
	}
}

void Viscosity_Bender2017::computeTargetStrainRate()
{
	Simulation *sim = Simulation::getCurrent();
	const int numParticles = (int) m_model->numActiveParticles();
	const unsigned int fluidModelIndex = m_model->getPointSetIndex();
	const unsigned int nFluids = sim->numberOfFluidModels();
	FluidModel *model = m_model;
		
	// Compute target strain rate
	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) nowait 
		for (int i = 0; i < numParticles; i++)
		{
			const Vector3r &xi = m_model->getPosition(i);
			const Vector3r &vi = m_model->getVelocity(i);
			const Real density_i = m_model->getDensity(i);

			Vector6r &strainRate = getTargetStrainRate(i);
			strainRate.setZero();

			//////////////////////////////////////////////////////////////////////////
			// Fluid
			//////////////////////////////////////////////////////////////////////////
			forall_fluid_neighbors_in_same_phase(
				const Vector3r &vj = m_model->getVelocity(neighborIndex);

				const Vector3r gradW = sim->gradW(xi - xj);
				const Vector3r vji = vj - vi;
				const Real m = m_model->getMass(neighborIndex);
				const Real m2 = m * 2.0;
				strainRate[0] += (1.0-m_viscosity) * m2 * vji[0] * gradW[0];
				strainRate[1] += (1.0-m_viscosity) * m2 * vji[1] * gradW[1];
				strainRate[2] += (1.0-m_viscosity) * m2 * vji[2] * gradW[2];
				strainRate[3] += (1.0-m_viscosity) * m * (vji[0] * gradW[1] + vji[1] * gradW[0]);
				strainRate[4] += (1.0-m_viscosity) * m * (vji[0] * gradW[2] + vji[2] * gradW[0]);
				strainRate[5] += (1.0-m_viscosity) * m * (vji[1] * gradW[2] + vji[2] * gradW[1]);
			)
			strainRate = (0.5 / density_i) * strainRate;
		}
	}
}

void Viscosity_Bender2017::performNeighborhoodSearchSort()
{
	const unsigned int numPart = m_model->numActiveParticles();
	if (numPart == 0)
		return;

	Simulation *sim = Simulation::getCurrent();
	auto const& d = sim->getNeighborhoodSearch()->point_set(m_model->getPointSetIndex());
	d.sort_field(&m_targetStrainRate[0]);
	d.sort_field(&m_viscosityFactor[0]);
	d.sort_field(&m_viscosityLambda[0]);
}

