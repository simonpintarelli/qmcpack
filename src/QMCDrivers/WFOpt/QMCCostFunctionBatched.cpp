//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
//
// File developed by: Ken Esler, kpesler@gmail.com, University of Illinois at Urbana-Champaign
//                    Miguel Morales, moralessilva2@llnl.gov, Lawrence Livermore National Laboratory
//                    Jeremy McMinnis, jmcminis@gmail.com, University of Illinois at Urbana-Champaign
//                    Jeongnim Kim, jeongnim.kim@gmail.com, University of Illinois at Urbana-Champaign
//                    Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//                    Raymond Clay III, j.k.rofling@gmail.com, Lawrence Livermore National Laboratory
//                    Mark A. Berrill, berrillma@ornl.gov, Oak Ridge National Laboratory
//
// File created by: Jeongnim Kim, jeongnim.kim@gmail.com, University of Illinois at Urbana-Champaign
//////////////////////////////////////////////////////////////////////////////////////


#include "QMCDrivers/WFOpt/QMCCostFunctionBatched.h"
#include "Particle/MCWalkerConfiguration.h"
#include "QMCWaveFunctions/TrialWaveFunction.h"
#include "Message/CommOperators.h"
#include "QMCDrivers/Optimizers/DescentEngine.h"
//#define QMCCOSTFUNCTION_DEBUG

namespace qmcplusplus
{
QMCCostFunctionBatched::QMCCostFunctionBatched(MCWalkerConfiguration& w,
                                               TrialWaveFunction& psi,
                                               QMCHamiltonian& h,
                                               SampleStack& samples,
                                               Communicate* comm)
    : QMCCostFunctionBase(w, psi, h, comm), samples_(samples)
{
  CSWeight = 1.0;
  app_log() << " Using QMCCostFunctionBatched::QMCCostFunctionBatched" << std::endl;
}


/** Clean up the vector */
QMCCostFunctionBatched::~QMCCostFunctionBatched()
{
  delete_iter(RngSaved.begin(), RngSaved.end());

  delete_iter(wf_ptr_list_.begin(), wf_ptr_list_.end());
  delete_iter(p_ptr_list_.begin(), p_ptr_list_.end());
  delete_iter(h_ptr_list_.begin(), h_ptr_list_.end());
  delete_iter(h0_ptr_list_.begin(), h0_ptr_list_.end());
}

void QMCCostFunctionBatched::GradCost(std::vector<Return_rt>& PGradient,
                                      const std::vector<Return_rt>& PM,
                                      Return_rt FiniteDiff)
{
  if (FiniteDiff > 0)
  {
    QMCTraits::RealType dh = 1.0 / (2.0 * FiniteDiff);
    for (int i = 0; i < NumOptimizables; i++)
    {
      for (int j = 0; j < NumOptimizables; j++)
        OptVariables[j] = PM[j];
      OptVariables[i]               = PM[i] + FiniteDiff;
      QMCTraits::RealType CostPlus  = this->Cost();
      OptVariables[i]               = PM[i] - FiniteDiff;
      QMCTraits::RealType CostMinus = this->Cost();
      PGradient[i]                  = (CostPlus - CostMinus) * dh;
    }
  }
  else
  {
    for (int j = 0; j < NumOptimizables; j++)
      OptVariables[j] = PM[j];
    resetPsi();
    //evaluate new local energies and derivatives
    NumWalkersEff = correlatedSampling(true);
    //Estimators::accumulate has been called by correlatedSampling
    curAvg_w = SumValue[SUM_E_WGT] / SumValue[SUM_WGT];
    //    Return_t curAvg2_w = curAvg_w*curAvg_w;
    curVar_w = SumValue[SUM_ESQ_WGT] / SumValue[SUM_WGT] - curAvg_w * curAvg_w;
    std::vector<Return_rt> EDtotals(NumOptimizables, 0.0);
    std::vector<Return_rt> EDtotals_w(NumOptimizables, 0.0);
    std::vector<Return_rt> E2Dtotals_w(NumOptimizables, 0.0);
    std::vector<Return_rt> URV(NumOptimizables, 0.0);
    std::vector<Return_rt> HD_avg(NumOptimizables, 0.0);
    Return_rt wgtinv   = 1.0 / SumValue[SUM_WGT];
    Return_rt delE_bar = 0;
    int numSamples = samples_.getNumSamples();
    {
      for (int iw = 0; iw < numSamples; iw++)
      {
        const Return_rt* restrict saved = RecordsOnNode[iw];
        Return_rt weight                = saved[REWEIGHT] * wgtinv;
        Return_rt eloc_new              = saved[ENERGY_NEW];
        delE_bar += weight * std::pow(std::abs(eloc_new - EtargetEff), PowerE);
        const Return_rt* HDsaved = HDerivRecords[iw];
        for (int pm = 0; pm < NumOptimizables; pm++)
          HD_avg[pm] += HDsaved[pm];
      }
    }
    myComm->allreduce(HD_avg);
    myComm->allreduce(delE_bar);
    for (int pm = 0; pm < NumOptimizables; pm++)
      HD_avg[pm] *= 1.0 / static_cast<Return_rt>(NumSamples);
    {
      int ip = 0;
      for (int iw = 0; iw < numSamples; iw++)
      {
        const Return_rt* restrict saved = RecordsOnNode[iw];
        Return_rt weight                = saved[REWEIGHT] * wgtinv;
        Return_rt eloc_new              = saved[ENERGY_NEW];
        Return_rt delta_l               = (eloc_new - curAvg_w);
        bool ltz(true);
        if (eloc_new - EtargetEff < 0)
          ltz = false;
        Return_rt delE           = std::pow(std::abs(eloc_new - EtargetEff), PowerE);
        Return_rt ddelE          = PowerE * std::pow(std::abs(eloc_new - EtargetEff), PowerE - 1);
        const Return_rt* Dsaved  = DerivRecords[iw];
        const Return_rt* HDsaved = HDerivRecords[iw];
        for (int pm = 0; pm < NumOptimizables; pm++)
        {
          EDtotals_w[pm] += weight * (HDsaved[pm] + 2.0 * Dsaved[pm] * delta_l);
          URV[pm]        += 2.0 * (eloc_new * HDsaved[pm] - curAvg * HD_avg[pm]);
          if (ltz)
            EDtotals[pm] += weight * (2.0 * Dsaved[pm] * (delE - delE_bar) + ddelE * HDsaved[pm]);
          else
            EDtotals[pm] += weight * (2.0 * Dsaved[pm] * (delE - delE_bar) - ddelE * HDsaved[pm]);
        }
      }
    }
    myComm->allreduce(EDtotals);
    myComm->allreduce(EDtotals_w);
    myComm->allreduce(URV);
    Return_rt smpinv = 1.0 / static_cast<Return_rt>(NumSamples);
    {
      int ip = 0;
      for (int iw = 0; iw < numSamples; iw++)
      {
        const Return_rt* restrict saved = RecordsOnNode[iw];
        Return_rt weight                = saved[REWEIGHT] * wgtinv;
        Return_rt eloc_new              = saved[ENERGY_NEW];
        Return_rt delta_l               = (eloc_new - curAvg_w);
        Return_rt sigma_l               = delta_l * delta_l;
        const Return_rt* Dsaved         = DerivRecords[iw];
        const Return_rt* HDsaved        = HDerivRecords[iw];
        for (int pm = 0; pm < NumOptimizables; pm++)
        {
          E2Dtotals_w[pm] +=
              weight * 2.0 * (Dsaved[pm] * (sigma_l - curVar_w) + delta_l * (HDsaved[pm] - EDtotals_w[pm]));
        }
      }
    }
    myComm->allreduce(E2Dtotals_w);
    for (int pm = 0; pm < NumOptimizables; pm++)
      URV[pm] *= smpinv;
    for (int j = 0; j < NumOptimizables; j++)
    {
      PGradient[j] = 0.0;
      if (std::abs(w_var) > 1.0e-10)
        PGradient[j] += w_var * E2Dtotals_w[j];
      if (std::abs(w_en) > 1.0e-10)
        PGradient[j] += w_en * EDtotals_w[j];
      if (std::abs(w_w) > 1.0e-10)
        PGradient[j] += w_w * URV[j];
      if (std::abs(w_abs) > 1.0e-10)
        PGradient[j] += w_abs * EDtotals[j];
    }
    IsValid = true;
    //         if ((CSWeight/wgtinv) < MinNumWalkers)
    if (NumWalkersEff < MinNumWalkers * NumSamples)
    {
      ERRORMSG("CostFunction-> Number of Effective Walkers is too small " << NumWalkersEff << "Minimum required"
                                                                          << MinNumWalkers * NumSamples)
      //ERRORMSG("Going to stop now.")
      IsValid = false;
    }
  }
}


void QMCCostFunctionBatched::getConfigurations(const std::string& aroot)
{
  //makeClones(W,Psi,H);
  if (H_KE_Node == nullptr)
  {
    app_log() << "  QMCCostFunctionBatched is created with " << NumThreads << " threads." << std::endl;
    //make H_KE_Node
  }

  app_log() << "  Using Nonlocal PP in Opt: " << includeNonlocalH << std::endl;
  outputManager.pause();
  {
    int ip = 0;
    if (H_KE_Node == nullptr)
    {
      H_KE_Node = std::make_unique<QMCHamiltonian>();
      H_KE_Node->addOperator(hClones[ip]->getHamiltonian("Kinetic"), "Kinetic");
      if (includeNonlocalH != "no")
      {
        OperatorBase* a = hClones[ip]->getHamiltonian(includeNonlocalH);
        if (a)
        {
          app_log() << " Found non-local Hamiltonian element named " << includeNonlocalH << std::endl;
          H_KE_Node->addOperator(a, includeNonlocalH);
        }
        else
          app_log() << " Did not find non-local Hamiltonian element named " << includeNonlocalH << std::endl;
      }
    }
  }

  //load samples from SampleStack
  outputManager.resume();
  int numSamples = samples_.getNumSamples();

  if (dLogPsi.size() != numSamples)
  {
    delete_iter(dLogPsi.begin(), dLogPsi.end());
    delete_iter(d2LogPsi.begin(), d2LogPsi.end());
    int nptcl = W.getTotalNum();
    dLogPsi.resize(numSamples);
    d2LogPsi.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
      dLogPsi[i] = new ParticleGradient_t(nptcl);
    for (int i = 0; i < numSamples; ++i)
      d2LogPsi[i] = new ParticleLaplacian_t(nptcl);
  }
}

/** evaluate everything before optimization */
void QMCCostFunctionBatched::checkConfigurations()
{
  RealType et_tot = 0.0;
  RealType e2_tot = 0.0;
  int numSamples = samples_.getNumSamples();
  {
    int ip         = 0;
    if (log_psi_fixed_.size() != numSamples)
    {
      log_psi_fixed_.resize(numSamples);
      log_psi_opt_.resize(numSamples);
    }
    if (wf_ptr_list_.size() != numSamples)
    {
      wf_ptr_list_.resize(numSamples);
      p_ptr_list_.resize(numSamples);
      h_ptr_list_.resize(numSamples);
      h0_ptr_list_.resize(numSamples);
    }

    if (RecordsOnNode.size1() == 0)
    {
      RecordsOnNode.resize(numSamples, SUM_INDEX_SIZE);
      if (needGrads)
      {
        DerivRecords.resize(numSamples, NumOptimizables);
        HDerivRecords.resize(numSamples, NumOptimizables);
      }
    }
    else if (RecordsOnNode.size1() != numSamples)
    {
      RecordsOnNode.resize(numSamples, SUM_INDEX_SIZE);
      if (needGrads)
      {
        DerivRecords.resize(numSamples, NumOptimizables);
        HDerivRecords.resize(numSamples, NumOptimizables);
      }
    }
    OperatorBase* nlpp = (includeNonlocalH == "no") ? 0 : hClones[ip]->getHamiltonian(includeNonlocalH);
    bool compute_nlpp  = useNLPPDeriv && nlpp;
    //set the optimization mode for the trial wavefunction
    psiClones[ip]->startOptimization();
    //    synchronize the random number generator with the node
    (*MoverRng[ip]) = (*RngSaved[ip]);
    hClones[ip]->setRandomGenerator(MoverRng[ip]);
    //int nat = wRef.getTotalNum();
    Return_rt e0 = 0.0;
    //       Return_t ef=0.0;
    Return_rt e2 = 0.0;


    auto ref_dLogPsi  = convertPtrToRefVector(dLogPsi);
    auto ref_d2LogPsi = convertPtrToRefVector(d2LogPsi);


    outputManager.pause();
    for (int iw = 0; iw < numSamples; iw++)
    {
      ParticleSet* wRef = new ParticleSet(*wClones[0]);
      samples_.loadSample(wRef->R, iw);
      wRef->update();
      p_ptr_list_[iw]  = wRef;
      wf_ptr_list_[iw] = psiClones[0]->makeClone(*wRef);
      h_ptr_list_[iw]  = hClones[0]->makeClone(*wRef, *wf_ptr_list_[iw]);
      h0_ptr_list_[iw] = H_KE_Node->makeClone(*wRef, *wf_ptr_list_[iw]);
    }
    outputManager.resume();

    RefVector<TrialWaveFunction> wf_list = convertPtrToRefVector(wf_ptr_list_);
    RefVector<ParticleSet> p_list        = convertPtrToRefVector(p_ptr_list_);
    RefVector<QMCHamiltonian> h_list     = convertPtrToRefVector(h_ptr_list_);

    TrialWaveFunction::flex_evaluateDeltaLogSetup(wf_list, p_list, log_psi_fixed_, log_psi_opt_, ref_dLogPsi,
                                                  ref_d2LogPsi);

    int nparam = getNumParams();
    RecordArray<Return_t> dlogpsi_array(nparam, numSamples);
    RecordArray<Return_t> dhpsioverpsi_array(nparam, numSamples);
    TrialWaveFunction::flex_evaluateParameterDerivatives(wf_list, p_list, OptVariablesForPsi, dlogpsi_array,
                                                         dhpsioverpsi_array);

    for (int iw = 0; iw < numSamples; iw++)
    {
      for (int j = 0; j < nparam; j++)
      {
        DerivRecords[iw][j]  = std::real(dlogpsi_array.getValue(j, iw));
        HDerivRecords[iw][j] = std::real(dhpsioverpsi_array.getValue(j, iw));
      }
      RecordsOnNode[iw][LOGPSI_FIXED] = log_psi_fixed_[iw];
      RecordsOnNode[iw][LOGPSI_FREE]  = log_psi_opt_[iw];
    }

    auto energy_list = QMCHamiltonian::flex_evaluate(h_list, p_list);

    for (int iw = 0; iw < numSamples; iw++)
    {
      auto etmp = energy_list[iw];
      e0 += etmp;
      e2 += etmp * etmp;

      RecordsOnNode[iw][ENERGY_NEW]   = etmp;
      RecordsOnNode[iw][ENERGY_TOT]   = etmp;
      RecordsOnNode[iw][ENERGY_FIXED] = h_list[iw].get().getLocalPotential();
      RecordsOnNode[iw][REWEIGHT]     = 1.0;
    }

    //add them all using reduction
    et_tot += e0;
    e2_tot += e2;
  }
  OptVariablesForPsi.setComputed();
  //     app_log() << "  VMC Efavg = " << eft_tot/static_cast<Return_t>(wPerNode[NumThreads]) << std::endl;
  //Need to sum over the processors
  std::vector<Return_rt> etemp(3);
  etemp[0] = et_tot;
  etemp[1] = static_cast<Return_rt>(numSamples);
  etemp[2] = e2_tot;
  myComm->allreduce(etemp);
  Etarget    = static_cast<Return_rt>(etemp[0] / etemp[1]);
  NumSamples = static_cast<int>(etemp[1]);
  app_log() << "  VMC Eavg = " << Etarget << std::endl;
  app_log() << "  VMC Evar = " << etemp[2] / etemp[1] - Etarget * Etarget << std::endl;
  app_log() << "  Total weights = " << etemp[1] << std::endl;
  app_log().flush();
  setTargetEnergy(Etarget);
  ReportCounter = 0;

  //collect SumValue for computedCost
  NumWalkersEff           = etemp[1];
  SumValue[SUM_WGT]       = etemp[1];
  SumValue[SUM_WGTSQ]     = etemp[1];
  SumValue[SUM_E_WGT]     = etemp[0];
  SumValue[SUM_ESQ_WGT]   = etemp[2];
  SumValue[SUM_E_BARE]    = etemp[0];
  SumValue[SUM_ESQ_BARE]  = etemp[2];
  SumValue[SUM_ABSE_BARE] = 0.0;
}

#ifdef HAVE_LMY_ENGINE
void QMCCostFunctionBatched::engine_checkConfigurations(cqmc::engine::LMYEngine<Return_t>* EngineObj,
                                                        DescentEngine& descentEngineObj,
                                                        const std::string& MinMethod)
{
  APP_ABORT("LMYEngine not implemented with batch optimization");
}
#endif


void QMCCostFunctionBatched::resetPsi(bool final_reset)
{
  if (OptVariables.size() < OptVariablesForPsi.size())
    for (int i = 0; i < equalVarMap.size(); ++i)
      OptVariablesForPsi[equalVarMap[i][0]] = OptVariables[equalVarMap[i][1]];
  else
    for (int i = 0; i < OptVariables.size(); ++i)
      OptVariablesForPsi[i] = OptVariables[i];
  if (final_reset)
  {
    for (int i = 0; i < psiClones.size(); ++i)
      psiClones[i]->stopOptimization();
  }
  //cout << "######### QMCCostFunctionBatched::resetPsi " << std::endl;
  //OptVariablesForPsi.print(std::cout);
  //cout << "-------------------------------------- " << std::endl;
  Psi.resetParameters(OptVariablesForPsi);
  for (int i = 0; i < psiClones.size(); ++i)
    psiClones[i]->resetParameters(OptVariablesForPsi);
}

QMCCostFunctionBatched::Return_rt QMCCostFunctionBatched::correlatedSampling(bool needGrad)
{
  {
    int ip = 0;
    //    synchronize the random number generator with the node
    (*MoverRng[ip]) = (*RngSaved[ip]);
    hClones[ip]->setRandomGenerator(MoverRng[ip]);
  }

  //Return_rt wgt_node = 0.0, wgt_node2 = 0.0;
  const bool nlpp         = (includeNonlocalH != "no");
  Return_rt wgt_tot       = 0.0;
  Return_rt wgt_tot2      = 0.0;
  int numSamples = samples_.getNumSamples();
  Return_rt inv_n_samples = 1.0 / numSamples;
  {
    int ip                        = 0;
    bool compute_nlpp             = useNLPPDeriv && (includeNonlocalH != "no");
    bool compute_all_from_scratch = (includeNonlocalH != "no"); //true if we have nlpp
    if (compute_all_from_scratch)
      APP_ABORT("Batched optimizer does not have support for NLPP yet");

    auto ref_dLogPsi  = convertPtrToRefVector(dLogPsi);
    auto ref_d2LogPsi = convertPtrToRefVector(d2LogPsi);

    outputManager.pause();
    delete_iter(wf_ptr_list_.begin(), wf_ptr_list_.end());
    for (int iw = 0; iw < numSamples; iw++)
    {
      ParticleSet* wRef = p_ptr_list_[iw];
      samples_.loadSample(wRef->R, iw);
      wRef->update(true);
      // TrialWaveFunction needs to be re-created.  Not sure why.
      wf_ptr_list_[iw] = psiClones[0]->makeClone(*wRef);
    }
    outputManager.resume();

    RefVector<TrialWaveFunction> wf_list = convertPtrToRefVector(wf_ptr_list_);
    RefVector<ParticleSet> p_list        = convertPtrToRefVector(p_ptr_list_);
    RefVector<QMCHamiltonian> h0_list    = convertPtrToRefVector(h0_ptr_list_);


    RefVector<ParticleSet::ParticleGradient_t> dummyG_list;
    RefVector<ParticleSet::ParticleLaplacian_t> dummyL_list;
    if (compute_all_from_scratch)
    {
      // need to have dummyG_list and dummyL_list set up
    }
    std::fill(log_psi_opt_.begin(), log_psi_opt_.end(), 0.0);

    TrialWaveFunction::flex_evaluateDeltaLog(wf_list, p_list, log_psi_opt_, dummyG_list, dummyL_list,
                                             compute_all_from_scratch);

    for (int iw = 0; iw < numSamples; iw++)
    {
      wf_list[iw].get().G += *dLogPsi[iw];
      wf_list[iw].get().L += *d2LogPsi[iw];
      // This is needed to get the KE correct in QMCHamiltonian::flex_evaluate below
      p_list[iw].get().G += *dLogPsi[iw];
      p_list[iw].get().L += *d2LogPsi[iw];
      Return_rt weight                  = vmc_or_dmc * (log_psi_opt_[iw] - RecordsOnNode[iw][LOGPSI_FREE]);
      RecordsOnNode[iw][REWEIGHT] = weight;
      wgt_tot  += inv_n_samples * weight;
      wgt_tot2 += inv_n_samples * weight * weight;
    }

    if (needGrad)
    {
      int nparam = getNumParams();
      RecordArray<Return_t> dlogpsi_array(nparam, numSamples);
      RecordArray<Return_t> dhpsioverpsi_array(nparam, numSamples);

      TrialWaveFunction::flex_evaluateParameterDerivatives(wf_list, p_list, OptVariablesForPsi, dlogpsi_array,
                                                           dhpsioverpsi_array);

      for (int iw = 0; iw < numSamples; iw++)
      {
        for (int j = 0; j < nparam; j++)
        {
          if (OptVariablesForPsi.recompute(j))
          {
            DerivRecords[iw][j]  = std::real(dlogpsi_array.getValue(j, iw));
            HDerivRecords[iw][j] = std::real(dhpsioverpsi_array.getValue(j, iw));
          }
        }
      }
      auto energy_list = QMCHamiltonian::flex_evaluate(h0_list, p_list);
      for (int iw = 0; iw < numSamples; iw++)
      {
        auto etmp                           = energy_list[iw];
        RecordsOnNode[iw][ENERGY_NEW] = etmp + RecordsOnNode[iw][ENERGY_FIXED];
      }
    }
    else
    {
      auto energy_list = QMCHamiltonian::flex_evaluate(h0_list, p_list);
      for (int iw = 0; iw < numSamples; iw++)
      {
        auto etmp                           = energy_list[iw];
        RecordsOnNode[iw][ENERGY_NEW] = etmp + RecordsOnNode[iw][ENERGY_FIXED];
      }
    }
  }

  //this is MPI barrier
  OHMMS::Controller->barrier();
  //collect the total weight for normalization and apply maximum weight
  myComm->allreduce(wgt_tot);
  myComm->allreduce(wgt_tot2);
  //    app_log()<<"Before Purge"<<wgt_tot<<" "<<wgt_tot2<< std::endl;
  Return_rt wgtnorm = (wgt_tot == 0) ? 0 : wgt_tot;
  wgt_tot           = 0.0;
  {
    int ip = 0;
    for (int iw = 0; iw < numSamples; iw++)
    {
      Return_rt* restrict saved = RecordsOnNode[iw];
      saved[REWEIGHT] =
          std::min(std::exp(saved[REWEIGHT] - wgtnorm), std::numeric_limits<Return_rt>::max() * (RealType)0.1);
      wgt_tot += inv_n_samples * saved[REWEIGHT];
    }
  }
  myComm->allreduce(wgt_tot);
  //    app_log()<<"During Purge"<<wgt_tot<<" "<< std::endl;
  wgtnorm = (wgt_tot == 0) ? 1 : 1.0 / wgt_tot;
  wgt_tot = 0.0;
  {
    int ip = 0;
    for (int iw = 0; iw < numSamples; iw++)
    {
      Return_rt* restrict saved = RecordsOnNode[iw];
      saved[REWEIGHT]           = std::min(saved[REWEIGHT] * wgtnorm, MaxWeight);
      wgt_tot += inv_n_samples * saved[REWEIGHT];
    }
  }
  myComm->allreduce(wgt_tot);
  //    app_log()<<"After Purge"<<wgt_tot<<" "<< std::endl;
  for (int i = 0; i < SumValue.size(); i++)
    SumValue[i] = 0.0;
  CSWeight = wgt_tot = (wgt_tot == 0) ? 1 : 1.0 / wgt_tot;
  {
    int ip = 0;
    for (int iw = 0; iw < numSamples; iw++)
    {
      const Return_rt* restrict saved = RecordsOnNode[iw];
      //      Return_t weight=saved[REWEIGHT]*wgt_tot;
      Return_rt eloc_new = saved[ENERGY_NEW];
      Return_rt delE     = std::pow(std::abs(eloc_new - EtargetEff), PowerE);
      SumValue[SUM_E_BARE]    += eloc_new;
      SumValue[SUM_ESQ_BARE]  += eloc_new * eloc_new;
      SumValue[SUM_ABSE_BARE] += delE;
      SumValue[SUM_E_WGT]     += eloc_new * saved[REWEIGHT];
      SumValue[SUM_ESQ_WGT]   += eloc_new * eloc_new * saved[REWEIGHT];
      SumValue[SUM_ABSE_WGT]  += delE * saved[REWEIGHT];
      SumValue[SUM_WGT]       += saved[REWEIGHT];
      SumValue[SUM_WGTSQ]     += saved[REWEIGHT] * saved[REWEIGHT];
    }
  }
  //collect everything
  myComm->allreduce(SumValue);
  //     for (int i=0; i<SumValue.size(); i++) std::cerr <<SumValue[i]<<"  ";
  //     std::cerr << std::endl;
  //     app_log()<<"After purge Energy Variance Weight "
  //      << SumValue[SUM_E_WGT]/SumValue[SUM_WGT] << " "
  //      << SumValue[SUM_ESQ_WGT]/SumValue[SUM_WGT] -(SumValue[SUM_E_WGT]/SumValue[SUM_WGT])*(SumValue[SUM_E_WGT]/SumValue[SUM_WGT]) << " "
  //      << SumValue[SUM_WGT]*SumValue[SUM_WGT]/SumValue[SUM_WGTSQ] << std::endl;
  return SumValue[SUM_WGT] * SumValue[SUM_WGT] / SumValue[SUM_WGTSQ];
}


QMCCostFunctionBatched::Return_rt QMCCostFunctionBatched::fillOverlapHamiltonianMatrices(Matrix<Return_rt>& Left,
                                                                                         Matrix<Return_rt>& Right)
{
  RealType b1, b2;
  if (GEVType == "H2")
  {
    b1 = w_beta;
    b2 = 0;
  }
  else
  {
    b2 = w_beta;
    b1 = 0;
  }

  Right = 0.0;
  Left  = 0.0;

  //     resetPsi();
  //     Return_t NWE = NumWalkersEff=correlatedSampling(true);
  curAvg_w            = SumValue[SUM_E_WGT] / SumValue[SUM_WGT];
  Return_rt curAvg2_w = SumValue[SUM_ESQ_WGT] / SumValue[SUM_WGT];
  //    RealType H2_avg = 1.0/curAvg2_w;
  RealType H2_avg = 1.0 / (curAvg_w * curAvg_w);
  //    RealType H2_avg = 1.0/std::sqrt(curAvg_w*curAvg_w*curAvg2_w);
  RealType V_avg = curAvg2_w - curAvg_w * curAvg_w;
  std::vector<Return_rt> D_avg(getNumParams(), 0.0);
  Return_rt wgtinv = 1.0 / SumValue[SUM_WGT];
  int numSamples = samples_.getNumSamples();
  {
    int ip = 0;
    for (int iw = 0; iw < numSamples; iw++)
    {
      const Return_rt* restrict saved = RecordsOnNode[iw];
      Return_rt weight                = saved[REWEIGHT] * wgtinv;
      const Return_rt* Dsaved         = DerivRecords[iw];
      for (int pm = 0; pm < getNumParams(); pm++)
      {
        D_avg[pm] += Dsaved[pm] * weight;
      }
    }
  }

  myComm->allreduce(D_avg);

  {
    int ip = 0;
    for (int iw = 0; iw < numSamples; iw++)
    {
      const Return_rt* restrict saved = RecordsOnNode[iw];
      Return_rt weight                = saved[REWEIGHT] * wgtinv;
      Return_rt eloc_new              = saved[ENERGY_NEW];
      const Return_rt* Dsaved         = DerivRecords[iw];
      const Return_rt* HDsaved        = HDerivRecords[iw];
      for (int pm = 0; pm < getNumParams(); pm++)
      {
        Return_rt wfe = (HDsaved[pm] + (Dsaved[pm] - D_avg[pm]) * eloc_new) * weight;
        Return_rt wfd = (Dsaved[pm] - D_avg[pm]) * weight;
        Return_rt vterm =
            HDsaved[pm] * (eloc_new - curAvg_w) + (Dsaved[pm] - D_avg[pm]) * eloc_new * (eloc_new - 2.0 * curAvg_w);
        //                Return_t vterm = (HDsaved[pm]+(Dsaved[pm]-D_avg[pm])*eloc_new -curAvg_w)*(eloc_new-curAvg_w);
        //                 H2
        Right(0, pm + 1) += b1 * H2_avg * vterm * weight;
        Right(pm + 1, 0) += b1 * H2_avg * vterm * weight;
        //                 Variance
        Left(0, pm + 1) += b2 * vterm * weight;
        Left(pm + 1, 0) += b2 * vterm * weight;
        //                 Hamiltonian
        Left(0, pm + 1) += (1 - b2) * wfe;
        Left(pm + 1, 0) += (1 - b2) * wfd * eloc_new;
        for (int pm2 = 0; pm2 < getNumParams(); pm2++)
        {
          //                Hamiltonian
          Left(pm + 1, pm2 + 1) += (1 - b2) * wfd * (HDsaved[pm2] + (Dsaved[pm2] - D_avg[pm2]) * eloc_new);
          //                Overlap
          RealType ovlij = wfd * (Dsaved[pm2] - D_avg[pm2]);
          Right(pm + 1, pm2 + 1) += ovlij;
          //                Variance
          RealType varij = weight * (HDsaved[pm] - 2.0 * (Dsaved[pm] - D_avg[pm]) * eloc_new) *
              (HDsaved[pm2] - 2.0 * (Dsaved[pm2] - D_avg[pm2]) * eloc_new);
          //                  RealType varij=weight*(HDsaved[pm] +(Dsaved[pm]-D_avg[pm])*eloc_new-curAvg_w)*
          //                                      (HDsaved[pm2] + (Dsaved[pm2]-D_avg[pm2])*eloc_new-curAvg_w);
          Left(pm + 1, pm2 + 1) += b2 * (varij + V_avg * ovlij);
          //                H2
          Right(pm + 1, pm2 + 1) += b1 * H2_avg * varij;
        }
      }
    }
  }
  myComm->allreduce(Right);
  myComm->allreduce(Left);
  Left(0, 0)  = (1 - b2) * curAvg_w + b2 * V_avg;
  Right(0, 0) = 1.0 + b1 * H2_avg * V_avg;
  if (GEVType == "H2")
    return H2_avg;

  return 1.0;
}
} // namespace qmcplusplus
