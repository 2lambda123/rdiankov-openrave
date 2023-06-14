// -*- coding: utf-8 -*-
// Copyright (C) 2016-2020 Puttichai Lertkultanon & Rosen DianKov
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License as published by the Free Software Foundation, either version 3
// of the License, or at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License along with this program.
// If not, see <http://www.gnu.org/licenses/>.
#include "openraveplugindefs.h"
#include <cfloat>
#include <fstream>
#include <openrave/planningutils.h>

#include "rampoptimizer/interpolator.h"
#include "rampoptimizer/parabolicchecker.h"
#include "rampoptimizer/feasibilitychecker.h"
#include "manipconstraints2.h"

// #define SMOOTHER2_TIMING_DEBUG // uncomment this to get more information on time spent for collision checking, manip constraint checking, etc.
// #define SMOOTHER2_PROGRESS_DEBUG // uncomment his to get more information on progress during each shortcut iteration

// #define SMOOTHER2_ENABLE_LAZYCOLLISIONCHECKING
// #define SMOOTHER2_DISABLE_VVISITEDDISCRETIZATION
#define SMOOTHER2_ENABLE_MERGING

namespace rplanners {

namespace RampOptimizer = RampOptimizerInternal;

class ParabolicSmoother2 : public PlannerBase, public RampOptimizer::FeasibilityCheckerBase, public RampOptimizer::RandomNumberGeneratorBase {

    class MyRampNDFeasibilityChecker : public RampOptimizer::RampNDFeasibilityChecker {
public:
        MyRampNDFeasibilityChecker(RampOptimizer::FeasibilityCheckerBase* feas_) : RampOptimizer::RampNDFeasibilityChecker(feas_) {
            _bHasParameters = false;
            _cacheRampNDVectIn.resize(1);
            _envid = 0;
        }

        void SetParameters(PlannerParametersConstPtr params)
        {
            _bHasParameters = true;
            _parameters.reset(new ConstraintTrajectoryTimingParameters());
            _parameters->copy(params);
        }

        void SetEnvID(int envid)
        {
            _envid = envid;
        }

        /// \brief A wrapper function for Check2.
        RampOptimizer::CheckReturn Check2(const RampOptimizer::RampND& rampndIn, int options, std::vector<RampOptimizer::RampND>& rampndVectOut)
        {
            _cacheRampNDVectIn[0] = rampndIn;
            return Check2(_cacheRampNDVectIn, options, rampndVectOut);
        }

        /// \brief Check all constraints on all rampnds in the given vector of rampnds. options is passed to the OpenRAVE check function.
        RampOptimizer::CheckReturn Check2(const std::vector<RampOptimizer::RampND>& rampndVect, int options, std::vector<RampOptimizer::RampND>& rampndVectOut)
        {
            std::vector<dReal> &q0=_q0, &q1=_q1, &dq0=_dq0, &dq1=_dq1;
            std::vector<uint8_t> &vsearchsegments=_vsearchsegments;

            // If all necessary constraints are checked (specified by options), then we set constraintChecked to true.
            if( (options & constraintmask) == constraintmask ) {
                FOREACH(itrampnd, rampndVect) {
                    itrampnd->constraintChecked = true;
                }
            }
            OPENRAVE_ASSERT_OP(tol.size(), ==, rampndVect[0].GetDOF());
            for (size_t idof = 0; idof < tol.size(); ++idof) {
                OPENRAVE_ASSERT_OP(tol[idof], >, 0);
            }

            bool bExpectedModifiedConfigurations = false;
            if( _bHasParameters ) {
                bExpectedModifiedConfigurations = (_parameters->fCosManipAngleThresh > -1 + g_fEpsilonLinear);
            }
            _vcacheintermediateconfigurations.resize(0);

            // Extract all switch points (including t = 0 and t = duration).
            if( _vswitchtimes.size() != rampndVect.size() + 1 ) {
                _vswitchtimes.resize(rampndVect.size() + 1);
            }
            dReal switchtime = 0;
            int index = 0;
            _vswitchtimes[index] = switchtime;
            FOREACHC(itrampnd, rampndVect) {
                index++;
                switchtime += itrampnd->GetDuration();
                _vswitchtimes[index] = switchtime;
            }

            // Check configurations at switch times first
            rampndVect[0].GetX0Vect(q0);
            rampndVect[0].GetV0Vect(dq0);
            RampOptimizer::CheckReturn ret0 = feas->ConfigFeasible2(q0, dq0, options);
            if( ret0.retcode != 0 ) {
                return ret0;
            }

            _vsearchsegments.resize(rampndVect.size(), 0);
            for (size_t isegment = 0; isegment < rampndVect.size(); ++isegment) {
                _vsearchsegments[isegment] = isegment;
            }
            // Just in case, check the middle config first.
            int midindex = _vsearchsegments.size() / 2; // midindex
            std::swap(_vsearchsegments[0], _vsearchsegments[midindex]);
            for (size_t j = 0; j < _vsearchsegments.size(); ++j) {
                rampndVect[_vsearchsegments[j]].GetX1Vect(q1);
                if( feas->NeedDerivativeForFeasibility() ) {
                    rampndVect[_vsearchsegments[j]].GetV1Vect(dq1);
                }
                RampOptimizer::CheckReturn ret1 = feas->ConfigFeasible2(q1, dq1, options);
                if( ret1.retcode != 0 ) {
                    return ret1;
                }
            }

            rampndVectOut.resize(0);

            // Now check each RampND
            rampndVect[0].GetX0Vect(q0);
            rampndVect[0].GetV0Vect(dq0);
            dReal elapsedTime, expectedElapsedTime, newElapsedTime, iElapsedTime, totalWeight;

            // Do lazy collision checking by postponing collision checking until absolutely necessary
#ifdef SMOOTHER2_ENABLE_LAZYCOLLISIONCHECKING
            bool doLazyCollisionChecking = true;
#else
            bool doLazyCollisionChecking = false;
#endif
            bool doCheckEnvCollisionsLater = doLazyCollisionChecking ? (options & CFO_CheckEnvCollisions) == CFO_CheckEnvCollisions : false;
            bool doCheckSelfCollisionsLater = doLazyCollisionChecking ? (options & CFO_CheckSelfCollisions) == CFO_CheckSelfCollisions : false;
            if( doLazyCollisionChecking ) {
                options = options & (~CFO_CheckEnvCollisions) & (~CFO_CheckSelfCollisions);
                options |= CFO_FillCheckedConfiguration; // always do this if we use lazy collision checking
            }
            for (size_t iswitch = 1; iswitch < _vswitchtimes.size(); ++iswitch) {
                rampndVect[iswitch - 1].GetX1Vect(q1); // configuration at _vswitchtimes[iswitch]
                elapsedTime = _vswitchtimes[iswitch] - _vswitchtimes[iswitch - 1]; // current elapsed time of this ramp

                if( feas->NeedDerivativeForFeasibility() ) {
                    rampndVect[iswitch - 1].GetV1Vect(dq1);

                    if( bExpectedModifiedConfigurations ) {
                        // Due to constraints, configurations along the segment may have been modified
                        // (via CheckPathAllConstraints called from SegmentFeasible2). This may cause
                        // dq1 not being consistent with q0, q1, dq0, and elapsedTime. So we check
                        // consistency here as well as modify dq1 and elapsedTime if necessary.

                        expectedElapsedTime = 0;
                        totalWeight = 0;
                        for (size_t idof = 0; idof < q0.size(); ++idof) {
                            dReal avgVel = 0.5*(dq0[idof] + dq1[idof]);
                            if( RaveFabs(avgVel) > g_fEpsilon ) {
                                dReal fWeight = RaveFabs(q1[idof] - q0[idof]);
                                expectedElapsedTime += fWeight*(q1[idof] - q0[idof])/avgVel;
                                totalWeight += fWeight;
                            }
                        }

                        if( totalWeight > g_fEpsilon ) {
                            // Recompute elapsed time
                            newElapsedTime = expectedElapsedTime/totalWeight;

                            // Check elapsed time consistency
                            if( RaveFabs(newElapsedTime) > RampOptimizer::g_fRampEpsilon ) {
                                elapsedTime = newElapsedTime;
                                if( elapsedTime > g_fEpsilon ) {
                                    iElapsedTime = 1/elapsedTime;
                                    for (size_t idof = 0; idof < q0.size(); ++idof) {
                                        dq1[idof] = 2*iElapsedTime*(q1[idof] - q0[idof]) - dq0[idof];
                                    }
                                }
                                else {
                                    dq1 = dq0;
                                }
                            }
                        }
                    }
                }

                RampOptimizer::CheckReturn retseg = feas->SegmentFeasible2(q0, q1, dq0, dq1, elapsedTime, options, _cacheRampNDVectOut, _vcacheintermediateconfigurations);
                if( retseg.retcode != 0 ) {
                    return retseg;
                }

                if( _cacheRampNDVectOut.size() > 0 ) {
                    if( IS_DEBUGLEVEL(Level_Verbose) ) {
                        for (size_t idof = 0; idof < q0.size(); ++idof) {
                            if( RaveFabs(q1[idof] - _cacheRampNDVectOut.back().GetX1At(idof)) > RampOptimizer::g_fRampEpsilon ) {
                                RAVELOG_VERBOSE_FORMAT("env=%d, rampndVect[%d] idof=%d: end point does not finish at the desired position, diff=%.15e", _envid%(iswitch - 1)%idof%RaveFabs(q1[idof] - _cacheRampNDVectOut.back().GetX1At(idof)));
                            }
                            if( RaveFabs(dq1[idof] - _cacheRampNDVectOut.back().GetV1At(idof)) > RampOptimizer::g_fRampEpsilon ) {
                                RAVELOG_VERBOSE_FORMAT("env=%d, rampndVect[%d] idof=%d: end point does not finish at the desired velocity, diff=%.15e", _envid%(iswitch - 1)%idof%RaveFabs(dq1[idof] - _cacheRampNDVectOut.back().GetV1At(idof)));
                            }
                        }
                    }
                    rampndVectOut.insert(rampndVectOut.end(), _cacheRampNDVectOut.begin(), _cacheRampNDVectOut.end());
                    rampndVectOut.back().GetX1Vect(q0);
                    rampndVectOut.back().GetV1Vect(dq0);
                }
            }

            // Collision checking here!
            if( doCheckEnvCollisionsLater || doCheckSelfCollisionsLater ) {
                if( doCheckEnvCollisionsLater && doCheckSelfCollisionsLater ) {
                    options = CFO_CheckEnvCollisions | CFO_CheckSelfCollisions;
                }
                else if( doCheckEnvCollisionsLater ) {
                    options = CFO_CheckEnvCollisions;
                }
                else {
                    options = CFO_CheckSelfCollisions;
                }

                // Instead of checking configurations sequentially from left to right, we give
                // higher priority to some configurations. Suppose rampndVectOut.size() is N.
                // First, check the ramp index: 0, 4N/8, 2N/8, 6N/8, N/8, 5N/8, 3N/8, 7N/8. Then we
                // check the remaining ramps in the usual order.

                // TODO: maybe arranging vsearchsegments totally randomly might have better average performance.
                if( bExpectedModifiedConfigurations ) {
                    // In this case, all intermediate configurations are already kept in rampndVectOut.
                    vsearchsegments.resize(rampndVectOut.size());
                }
                else {
                    size_t nconfigs = _vcacheintermediateconfigurations.size()/tol.size();
                    BOOST_ASSERT(nconfigs > 0);
                    vsearchsegments.resize(nconfigs);
                }
                for( size_t j = 0; j < vsearchsegments.size(); ++j ) {
                    vsearchsegments[j] = j;
                }
                do {
                    size_t index1, index2 = 0;

                    index1 = vsearchsegments.size() * (4/8);
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;

                    index1 = vsearchsegments.size() * (2/8);
                    if( index1 <= index2 ) {
                        break;
                    }
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;

                    index1 = vsearchsegments.size() * (6/8);
                    if( index1 <= index2 ) {
                        break;
                    }
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;

                    index1 = vsearchsegments.size() * (1/8);
                    if( index1 <= index2 ) {
                        break;
                    }
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;

                    index1 = vsearchsegments.size() * (5/8);
                    if( index1 <= index2 ) {
                        break;
                    }
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;

                    index1 = vsearchsegments.size() * (3/8);
                    if( index1 <= index2 ) {
                        break;
                    }
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;

                    index1 = vsearchsegments.size() * (7/8);
                    if( index1 <= index2 ) {
                        break;
                    }
                    std::swap(vsearchsegments[index2], vsearchsegments[index1]); index2++;
                } while (0);

#ifdef SMOOTHER2_TIMING_DEBUG
                uint32_t tStartCollisionChecking = utils::GetMicroTime();
#endif
                if( bExpectedModifiedConfigurations ) {
                    for( size_t j = 0; j < vsearchsegments.size(); ++j ) {
                        rampndVectOut[vsearchsegments[j]].GetX1Vect(q0);
                        RampOptimizer::CheckReturn ret = feas->ConfigFeasible2(q0, std::vector<dReal>(), options);
                        if( ret.retcode != 0 ) {
#ifdef SMOOTHER2_TIMING_DEBUG
                            uint32_t tFinishCollisionChecking = utils::GetMicroTime();
                            RAVELOG_DEBUG_FORMAT("env=%d, time spent for collision checking=%f s", _envid%(0.000001f*(float)(tFinishCollisionChecking - tStartCollisionChecking)));
#endif
                            return ret;
                        }
                    }
                }
                else {
                    std::vector<dReal>::const_iterator itconfig;
                    for( size_t j = 0; j < vsearchsegments.size(); ++j ) {
                        itconfig = _vcacheintermediateconfigurations.begin() + vsearchsegments[j]*tol.size();
                        q0.assign(itconfig, itconfig + tol.size());
                        RampOptimizer::CheckReturn ret = feas->ConfigFeasible2(q0, std::vector<dReal>(), options);
                        if( ret.retcode != 0 ) {
#ifdef SMOOTHER2_TIMING_DEBUG
                            uint32_t tFinishCollisionChecking = utils::GetMicroTime();
                            RAVELOG_DEBUG_FORMAT("env=%d, time spent for collision checking=%f s", _envid%(0.000001f*(float)(tFinishCollisionChecking - tStartCollisionChecking)));
#endif
                            return ret;
                        }
                    }
                }
#ifdef SMOOTHER2_TIMING_DEBUG
                uint32_t tFinishCollisionChecking = utils::GetMicroTime();
                RAVELOG_DEBUG_FORMAT("env=%d, time spent for collision checking=%f s", _envid%(0.000001f*(float)(tFinishCollisionChecking - tStartCollisionChecking)));
#endif
            }

            bool bDifferentVelocity = false;
            if( rampndVectOut.size() > 0 ) {
                for (size_t idof = 0; idof < q0.size(); ++idof) {
                    if( RaveFabs(rampndVect.back().GetX1At(idof) - rampndVectOut.back().GetX1At(idof)) > RampOptimizer::g_fRampEpsilon ) {
                        RAVELOG_VERBOSE_FORMAT("env=%d, rampndVectOut idof=%d: end point does not finish at the desired position, diff=%.15e. Rejecting...", _envid%idof%RaveFabs(rampndVect.back().GetX1At(idof) - q0[idof]));
                        return RampOptimizer::CheckReturn(CFO_FinalValuesNotReached);
                    }
                    if( RaveFabs(rampndVect.back().GetV1At(idof) - rampndVectOut.back().GetV1At(idof)) > RampOptimizer::g_fRampEpsilon ) {
                        RAVELOG_VERBOSE_FORMAT("env=%d, rampndVectOut idof=%d: end point does not finish at the desired velocity, diff=%.15e", _envid%idof%RaveFabs(rampndVect.back().GetV1At(idof) - dq0[idof]));
                        bDifferentVelocity = true;
                    }
                }
            }
            RampOptimizer::CheckReturn finalret(0);
            finalret.bDifferentVelocity = bDifferentVelocity;
            return finalret;
        }

private:
        ConstraintTrajectoryTimingParametersPtr _parameters;
        bool _bHasParameters;
        int _envid; ///< useful for logging

        // Cache
        std::vector<dReal> _vswitchtimes;
        std::vector<dReal> _q0, _q1, _dq0, _dq1;
        std::vector<uint8_t> _vsearchsegments;
        std::vector<RampOptimizer::RampND> _cacheRampNDVectIn, _cacheRampNDVectOut;
        std::vector<dReal> _vcacheintermediateconfigurations; ///< for keeping intermediate configurations that are checked in CheckPathAllConstraints

    }; // end class MyRampNDFeasibilityChecker

public:
    ParabolicSmoother2(EnvironmentBasePtr penv, std::istream& sinput) : PlannerBase(penv), _feasibilitychecker(this)
    {
        __description = "";
        _bmanipconstraints = false;
        _constraintreturn.reset(new ConstraintFilterReturn());
        _logginguniformsampler = RaveCreateSpaceSampler(GetEnv(), "mt19937");
        if (!!_logginguniformsampler) {
            _logginguniformsampler->SetSeed(utils::GetMicroTime());
        }
        _environmentid = GetEnv()->GetId();
        _vVisitedDiscretizationCache.resize(0x1000*0x1000,0); // pre-allocate in order to keep memory growth predictable
        _feasibilitychecker.SetEnvID(_environmentid); // set envid for logging purpose
    }

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr params)
    {
        EnvironmentLock lock(GetEnv()->GetMutex());
        _parameters.reset(new ConstraintTrajectoryTimingParameters());
        _parameters->copy(params);
        return _InitPlan();
    }

    virtual bool InitPlan(RobotBasePtr pbase, std::istream& isParameters)
    {
        EnvironmentLock lock(GetEnv()->GetMutex());
        _parameters.reset(new ConstraintTrajectoryTimingParameters());
        isParameters >> *_parameters;
        return _InitPlan();
    }

    bool _InitPlan()
    {
        // Allow using max iterations = 0. In such cases, will only do initial time-parameterization.
        if( _parameters->_nMaxIterations < 0 ) {
            _parameters->_nMaxIterations = 100;
        }

        _bUsePerturbation = true;
        _bmanipconstraints = (_parameters->manipname.size() > 0) && (_parameters->maxmanipspeed > 0 || _parameters->maxmanipaccel > 0);
        _feasibilitychecker.SetParameters(GetParameters());

        _interpolator.Initialize(_parameters->GetDOF(), _environmentid);

        // Initialize workspace constraints on manipulators
        if( _bmanipconstraints ) {
            if( !_manipconstraintchecker ) {
                _manipconstraintchecker.reset(new ManipConstraintChecker2(GetEnv()));
            }
            _manipconstraintchecker->Init(_parameters->manipname, _parameters->_configurationspecification, _parameters->maxmanipspeed, _parameters->maxmanipaccel);
        }

        // Initialize a uniform sampler
        if( !_uniformsampler ) {
            _uniformsampler = RaveCreateSpaceSampler(GetEnv(), "mt19937");
        }
        _uniformsampler->SetSeed(_parameters->_nRandomGeneratorSeed);

        _fileIndexMod = 10000; // for trajectory saving
        if( !!_logginguniformsampler ) {
            _fileIndex = _logginguniformsampler->SampleSequenceOneUInt32();
        }
        else {
            _fileIndex = RaveRandomInt();
        }
        _fileIndex = _fileIndex%_fileIndexMod;
#ifdef SMOOTHER2_PROGRESS_DEBUG
        _dumplevel = Level_Debug;
#else
        _dumplevel = Level_Verbose;
#endif
        _maxInitialRampTime = 0;
#ifdef SMOOTHER2_TIMING_DEBUG
        // Statistics
        _numShortcutIters = 0;
        _nCallsCheckManip = 0;
        _totalTimeCheckManip = 0;
        _nCallsInterpolator = 0;
        _totalTimeInterpolator = 0;
        _nCallsCheckPathAllConstraints = 0;
        _totalTimeCheckPathAllConstraints = 0;
        _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
        _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
        _nCallsCheckPathAllConstraintsInVain = 0;
        _totalTimeCheckPathAllConstraintsInVain = 0;
#endif

        _bUseNewHeuristic = false; // dof-depending velocity/acceleration scaling factors

        // Caching stuff
        size_t ndof = _parameters->GetDOF();
        if( _cacheCurPos.capacity() < ndof ) {
            _cacheCurPos.reserve(ndof);
        }
        if( _cacheCurVel.capacity() < ndof ) {
            _cacheCurVel.reserve(ndof);
        }
        _cacheNewPos.resize(ndof);
        _cacheNewVel.resize(ndof);
        if( _cacheX0Vect.capacity() < ndof ) {
            _cacheX0Vect.reserve(ndof);
        }
        if( _cacheX1Vect.capacity() < ndof ) {
            _cacheX1Vect.reserve(ndof);
        }
        if( _cacheV0Vect.capacity() < ndof ) {
            _cacheV0Vect.reserve(ndof);
        }
        if( _cacheV1Vect.capacity() < ndof ) {
            _cacheV1Vect.reserve(ndof);
        }
        if( _cacheX0Vect1.capacity() < ndof ) {
            _cacheX0Vect1.reserve(ndof);
        }
        if( _cacheX1Vect1.capacity() < ndof ) {
            _cacheX1Vect1.reserve(ndof);
        }
        if( _cacheVellimits.capacity() < ndof ) {
            _cacheVellimits.reserve(ndof);
        }
        if( _cacheAccelLimits.capacity() < ndof ) {
            _cacheAccelLimits.reserve(ndof);
        }
        return !!_uniformsampler;
    }

    virtual PlannerParametersConstPtr GetParameters() const
    {
        return _parameters;
    }

    virtual PlannerStatus PlanPath(TrajectoryBasePtr ptraj, int planningoptions) override
    {
        BOOST_ASSERT(!!_parameters && !!ptraj);

        if( ptraj->GetNumWaypoints() < 2 ) {
            return OPENRAVE_PLANNER_STATUS(PS_Failed);
        }

        _basetime = utils::GetMilliTime();

        if( IS_DEBUGLEVEL(_dumplevel) ) {
            // Save parameters for planning
            std::string filename = str(boost::format("%s/parabolicsmoother2_%d.parameters.xml")%RaveGetHomeDirectory()%_fileIndex);
            ofstream f(filename.c_str());
            f << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
            f << *_parameters;
            RavePrintfA(str(boost::format("env=%d, planner parameters saved to %s")%_environmentid%filename), _dumplevel);
        }
        _DumpTrajectory(ptraj, _dumplevel, 0);

        // Save velocities
        std::vector<KinBody::KinBodyStateSaverPtr> vstatesavers;
        std::vector<KinBodyPtr> vusedbodies;
        _parameters->_configurationspecification.ExtractUsedBodies(GetEnv(), vusedbodies);
        if( vusedbodies.size() == 0 ) {
            RAVELOG_WARN_FORMAT("env=%d, There is no used bodies in this configuration", _environmentid);
        }
        FOREACH(itbody, vusedbodies) {
            KinBody::KinBodyStateSaverPtr statesaver;
            if( (*itbody)->IsRobot() ) {
                statesaver.reset(new RobotBase::RobotStateSaver(RaveInterfaceCast<RobotBase>(*itbody), KinBody::Save_LinkTransformation|KinBody::Save_LinkEnable|KinBody::Save_ActiveDOF|KinBody::Save_ActiveManipulator|KinBody::Save_LinkVelocities));
            }
            else {
                statesaver.reset(new KinBody::KinBodyStateSaver(*itbody, KinBody::Save_LinkTransformation|KinBody::Save_LinkEnable|KinBody::Save_ActiveDOF|KinBody::Save_ActiveManipulator|KinBody::Save_LinkVelocities));
            }
            vstatesavers.push_back(statesaver);
        }

        uint64_t baseTime = utils::GetMicroTime();
        ConfigurationSpecification posSpec = _parameters->_configurationspecification;
        ConfigurationSpecification velSpec = posSpec.ConvertToVelocitySpecification();
        ConfigurationSpecification timeSpec;
        timeSpec.AddDeltaTimeGroup();

        // get DynamicsCollisionConstraint parameters
        std::map<std::string, int> mapIntParameters;
        std::map<std::string, dReal> mapFloatParameters;
        _parameters->GetConstraintCheckerParams(mapIntParameters, mapFloatParameters);
        //RAVELOG_DEBUG_FORMAT("param=%d;%d;%f;%f", mapIntParameters["filtermask"] % mapIntParameters["dynamicsconstraintstype"] % mapFloatParameters["dynamiclimitsaccelerationmult"] % mapFloatParameters["dynamiclimitsjerkmult"]);
        std::map<std::string, int>::const_iterator itFilterMask = mapIntParameters.find("filtermask");
        std::map<std::string, int>::const_iterator itDynamicsConstraintsType = mapIntParameters.find("dynamicsconstraintstype");
        std::map<std::string, dReal>::const_iterator itDynamicLimitsAccelerationMult = mapFloatParameters.find("dynamiclimitsaccelerationmult");
        if( itFilterMask == mapIntParameters.end() || itDynamicsConstraintsType == mapIntParameters.end() || itDynamicLimitsAccelerationMult == mapFloatParameters.end() ) {
            // TODO : how to handle this?
            RAVELOG_WARN_FORMAT("could not get parameters : nofiltermask=%d;nodynamicsconstraintstype=%d;noaccmult=%d",
                                (itFilterMask == mapIntParameters.end())%(itDynamicsConstraintsType == mapIntParameters.end())%(itDynamicLimitsAccelerationMult == mapFloatParameters.end()));
        }

        std::vector<int> vUsedConfigIndices;
        _dynamicLimitInfo.Reset();
        FOREACH(itbody, vusedbodies) {
            if( (*itbody)->IsRobot() && ((*itbody)->GetDOF() >= (int)_parameters->_vConfigVelocityLimit.size()) ) {
                posSpec.ExtractUsedIndices(KinBodyConstPtr(*itbody), _dynamicLimitInfo.vUsedDOFIndices, vUsedConfigIndices);
                if( _dynamicLimitInfo.vUsedDOFIndices.size() == _parameters->_vConfigVelocityLimit.size() ) {
                    _dynamicLimitInfo.bodyName = (*itbody)->GetName();
                    _dynamicLimitInfo.vCacheFullDOFPositions.resize((*itbody)->GetDOF());
                    _dynamicLimitInfo.vCacheFullDOFVelocities.clear();
                    _dynamicLimitInfo.vCacheFullDOFVelocities.resize((*itbody)->GetDOF(), 0.0);
                    _dynamicLimitInfo.vCacheFullDOFAccelerationLimits.resize((*itbody)->GetDOF());
                    _dynamicLimitInfo.vCacheFullDOFJerkLimits.resize((*itbody)->GetDOF());
                    _dynamicLimitInfo.fDynamicAccelerationLimitMult = itDynamicLimitsAccelerationMult->second;
                    const bool bHasDynamicLimits = (*itbody)->GetDOFDynamicAccelerationJerkLimits(_dynamicLimitInfo.vCacheFullDOFAccelerationLimits, _dynamicLimitInfo.vCacheFullDOFJerkLimits,
                                                                                                  _dynamicLimitInfo.vCacheFullDOFPositions, _dynamicLimitInfo.vCacheFullDOFVelocities);
                    const DynamicsConstraintsType dynamicsConstraintsType = (DynamicsConstraintsType)(itDynamicsConstraintsType->second);
                    _dynamicLimitInfo.bUseDynamicLimits = bHasDynamicLimits && (itFilterMask->second & CFO_CheckTimeBasedConstraints) && (dynamicsConstraintsType != DC_Unknown && dynamicsConstraintsType != DC_IgnoreTorque);
                    break;
                }
            }
        }

        std::vector<ConfigurationSpecification::Group>::const_iterator itcompatposgroup = ptraj->GetConfigurationSpecification().FindCompatibleGroup(posSpec._vgroups.at(0), false);
        OPENRAVE_ASSERT_FORMAT(itcompatposgroup != ptraj->GetConfigurationSpecification()._vgroups.end(), "Failed to find group %s in the passed-in trajectory", posSpec._vgroups.at(0).name, ORE_InvalidArguments);

        ConstraintTrajectoryTimingParametersConstPtr parameters = boost::dynamic_pointer_cast<ConstraintTrajectoryTimingParameters const>(GetParameters());

        // Initialize a parabolicpath
        RampOptimizer::ParabolicPath& parabolicpath = _cacheparabolicpath;
        parabolicpath.Reset();
        OPENRAVE_ASSERT_OP(parameters->_vConfigVelocityLimit.size(), ==, parameters->_vConfigAccelerationLimit.size());
        OPENRAVE_ASSERT_OP((int) parameters->_vConfigVelocityLimit.size(), ==, parameters->GetDOF());

        // Retrieve waypoints
        bool bPathIsPerfectlyModeled = false; // will be true if the initial interpolation is linear or quadratic
        std::vector<dReal> q(_parameters->GetDOF());
        std::vector<dReal>& waypoints = _cacheWaypoints; // to store concatenated waypoints obtained from ptraj
        std::vector<dReal>& x0Vect = _cacheX0Vect, &x1Vect = _cacheX1Vect, &v0Vect = _cacheV0Vect, &v1Vect = _cacheV1Vect, &tVect = _cacheTVect;
        RampOptimizer::RampND& tempRampND = _cacheRampND;

        if( _parameters->_hastimestamps && itcompatposgroup->interpolation == "quadratic" ) {
            RAVELOG_VERBOSE_FORMAT("env=%d, The initial trajectory is piecewise quadratic", _environmentid);

            // Convert the original OpenRAVE trajectory to a parabolicpath
            ptraj->GetWaypoint(0, x0Vect, posSpec);
            ptraj->GetWaypoint(0, v0Vect, velSpec);

            for (size_t iwaypoint = 1; iwaypoint < ptraj->GetNumWaypoints(); ++iwaypoint) {
                ptraj->GetWaypoint(iwaypoint, tVect, timeSpec);
                if( tVect.at(0) > g_fEpsilonLinear ) {
                    ptraj->GetWaypoint(iwaypoint, x1Vect, posSpec);
                    ptraj->GetWaypoint(iwaypoint, v1Vect, velSpec);
                    tempRampND.Initialize(x0Vect, x1Vect, v0Vect, v1Vect, std::vector<dReal>(), tVect[0]);
                    parabolicpath.AppendRampND(tempRampND);
                    x0Vect.swap(x1Vect);
                    v0Vect.swap(v1Vect);
                }
            }
            bPathIsPerfectlyModeled = true;
        }
        else if( _parameters->_hastimestamps && itcompatposgroup->interpolation == "cubic" ) {
            RAVELOG_VERBOSE_FORMAT("env=%d, The initial trajectory is piecewise cubic", _environmentid);

            // Convert the original OpenRAVE trajectory to a parabolicpath
            ptraj->GetWaypoint(0, x0Vect, posSpec);
            ptraj->GetWaypoint(0, v0Vect, velSpec);
            KinBodyConstPtr usedBody = _dynamicLimitInfo.bUseDynamicLimits ? GetEnv()->GetKinBody(_dynamicLimitInfo.bodyName) : KinBodyConstPtr();

            std::vector<RampOptimizer::RampND>& tempRampNDVect = _cacheRampNDVect;
            for (size_t iwaypoint = 1; iwaypoint < ptraj->GetNumWaypoints(); ++iwaypoint) {
                ptraj->GetWaypoint(iwaypoint, tVect, timeSpec);
                if( tVect.at(0) > g_fEpsilonLinear ) {
                    ptraj->GetWaypoint(iwaypoint, x1Vect, posSpec);
                    ptraj->GetWaypoint(iwaypoint, v1Vect, velSpec);

                    dReal iDeltaTime = 1/tVect[0];
                    dReal iDeltaTime2 = iDeltaTime*iDeltaTime;
                    bool isParabolic = true;
                    for (size_t jdof = 0; jdof < x0Vect.size(); ++jdof) {
                        dReal coeff = (2.0*iDeltaTime*(x0Vect[jdof] - x1Vect[jdof]) + v0Vect[jdof] + v1Vect[jdof])*iDeltaTime2;
                        if( RaveFabs(coeff) > 1e-5 ) {
                            isParabolic = false;
                        }
                    }

                    if( isParabolic ) {
                        tempRampND.Initialize(x0Vect, x1Vect, v0Vect, v1Vect, std::vector<dReal>(), tVect[0]);
                        if( !_parameters->verifyinitialpath ) {
                            tempRampND.constraintChecked = true;
                        }
                    }
                    else {
                        // We only check time-based constraints since the path is anyway likely to be modified during shortcutting.
                        if( !_ComputeRampWithZeroVelEndpoints(x0Vect, x1Vect, CFO_CheckTimeBasedConstraints, tempRampNDVect, usedBody) ) {
#ifdef SMOOTHER2_TIMING_DEBUG
                            // We don't use this stats
                            // Reset SegmentFeasible2 counters
                            _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                            _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif
                            std::string description = str(boost::format("env=%d, Failed to initialize from cubic waypoints")%_environmentid);
                            _DumpTrajectory(ptraj, _dumplevel, 1);
                            return OPENRAVE_PLANNER_STATUS(description, PS_Failed);
                        }
#ifdef SMOOTHER2_TIMING_DEBUG
                        // We don't use this stats
                        // Reset SegmentFeasible2 counters
                        _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                        _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif
                    }

                    FOREACHC(itrampnd, tempRampNDVect) {
                        parabolicpath.AppendRampND(*itrampnd);
                    }
                    x0Vect.swap(x1Vect);
                    v0Vect.swap(v1Vect);
                }
            }
        }
        else {
            if( itcompatposgroup->interpolation.size() == 0 || itcompatposgroup->interpolation == "linear" ) {
                RAVELOG_VERBOSE_FORMAT("env=%d, The initial trajectory is piecewise linear", _environmentid);
                bPathIsPerfectlyModeled = true;
            }
            else {
                RAVELOG_VERBOSE_FORMAT("env=%d, The initial trajectory is with unspecified interpolation", _environmentid);
            }

            std::vector<std::vector<dReal> >& vWaypoints = _cacheWaypointVect;
            vWaypoints.resize(0);
            if( vWaypoints.capacity() < ptraj->GetNumWaypoints() ) {
                vWaypoints.reserve(ptraj->GetNumWaypoints());
            }

            ptraj->GetWaypoints(0, ptraj->GetNumWaypoints(), waypoints, posSpec);

            // Iterate through all waypoints to remove collinear ones.
            dReal collinearThresh = 1e-14;
            for (size_t iwaypoint = 0; iwaypoint < ptraj->GetNumWaypoints(); ++iwaypoint) {
                // Copy waypoints[iwaypoint] into q
                std::copy(waypoints.begin() + iwaypoint*_parameters->GetDOF(), waypoints.begin() + (iwaypoint + 1)*_parameters->GetDOF(), q.begin());

                if( vWaypoints.size() > 1 ) {
                    // Check if the new waypoint (q) is collinear with the previous ones.
                    const std::vector<dReal>& x0 = vWaypoints[vWaypoints.size() - 2];
                    const std::vector<dReal>& x1 = vWaypoints[vWaypoints.size() - 1];
                    dReal dotProduct = 0, x0Length2 = 0, x1Length2 = 0;

                    for (size_t idof = 0; idof < q.size(); ++idof) {
                        dReal dx0 = x0[idof] - q[idof];
                        dReal dx1 = x1[idof] - q[idof];
                        dotProduct += dx0*dx1;
                        x0Length2 += dx0*dx0;
                        x1Length2 += dx1*dx1;
                    }
                    if( RaveFabs(dotProduct*dotProduct - x0Length2*x1Length2) < collinearThresh ) {
                        // Points are collinear
                        vWaypoints.back() = q;
                        continue;
                    }
                }

                // Check if the new point is not the same as the previous one
                if( vWaypoints.size() > 0 ) {
                    dReal d = 0;
                    for (size_t idof = 0; idof < q.size(); ++idof) {
                        d += RaveFabs(q[idof] - vWaypoints.back()[idof]);
                    }
                    if( d <= q.size()*std::numeric_limits<dReal>::epsilon() ) {
                        continue;
                    }
                }

                // The new point is not redundant. Add it to vWaypoints.
                vWaypoints.push_back(q);
            }

            // Time-parameterize the initial path
            if( !_SetMileStones(vWaypoints, parabolicpath) ) {
                std::string description = str(boost::format("env=%d, Failed to initialize from piecewise linear waypoints")%_environmentid);
                RAVELOG_WARN(description);
                _DumpTrajectory(ptraj, _dumplevel, 2);
                return OPENRAVE_PLANNER_STATUS(description, PS_Failed);
            }
            RAVELOG_DEBUG_FORMAT("env=%d, Finished initializing linear waypoints via _SetMileStones. #waypoint: %d -> %d", _environmentid%ptraj->GetNumWaypoints()%vWaypoints.size());
        }

        // Tell parabolicsmoother not to check constraints again if we already did (e.g. in linearsmoother, etc.)
        if( !_parameters->verifyinitialpath && bPathIsPerfectlyModeled ) {
            FOREACH(itrampnd, parabolicpath.GetRampNDVect()) {
                itrampnd->constraintChecked = true;
            }
        }

        // Main planning loop
        uint64_t mergeStartTime = 0, shortcutStartTime = 0, conversionStartTime = 0;
        try {
            _bUsePerturbation = true;
            _feasibilitychecker.tol = parameters->_vConfigResolution;
            FOREACH(it, _feasibilitychecker.tol) {
                *it *= parameters->_pointtolerance;
            }

            _progress._iteration = 0;
            if( _CallCallbacks(_progress) == PA_Interrupt ) {
                return OPENRAVE_PLANNER_STATUS(str(boost::format("env=%d, Planning was interrupted")%_environmentid), PS_Interrupted);
            }

            int numShortcuts = 0;
#ifdef SMOOTHER2_ENABLE_MERGING
            int nummerges = 0;
#endif
            if( !!parameters->_setstatevaluesfn && _parameters->_nMaxIterations > 0 ) {
                // TODO: add a check here so that we do merging only when the initial path is linear (i.e. comes directly from a linear smoother or RRT)
                mergeStartTime = utils::GetMicroTime();
#ifdef SMOOTHER2_TIMING_DEBUG
                _tShortcutStart = utils::GetMicroTime();
#endif
#ifdef SMOOTHER2_ENABLE_MERGING
                if( parameters->maxmergeiterations > 0 ) {
                    nummerges = _MergeConsecutiveSegments(parabolicpath, parameters->_fStepLength*0.99);
                    if( nummerges < 0 ) {
                        return OPENRAVE_PLANNER_STATUS(str(boost::format("env=%d, Planning was interrupted")%_environmentid), PS_Interrupted);
                    }
                }
#endif
                shortcutStartTime = utils::GetMicroTime();
                numShortcuts = _Shortcut(parabolicpath, parameters->_nMaxIterations, this, parameters->_fStepLength*0.99);
#ifdef SMOOTHER2_TIMING_DEBUG
                _tShortcutEnd = utils::GetMicroTime();
#endif
                if( numShortcuts < 0 ) {
                    return OPENRAVE_PLANNER_STATUS(str(boost::format("env=%d, Planning was interrupted")%_environmentid), PS_Interrupted);
                }
            }
            else {
                RAVELOG_DEBUG_FORMAT("env=%d, skip shortcutting since nMaxIterations=%d", _environmentid%_parameters->_nMaxIterations);
            }

            ++_progress._iteration;
            if( _CallCallbacks(_progress) == PA_Interrupt ) {
                return OPENRAVE_PLANNER_STATUS(str(boost::format("env=%d, Planning was interrupted")%_environmentid), PS_Interrupted);
            }

            // Now start converting parabolicpath to OpenRAVE trajectory
            conversionStartTime = utils::GetMicroTime();
            ConfigurationSpecification newSpec = posSpec;
            newSpec.AddDerivativeGroups(1, true);
            int waypointOffset = newSpec.AddGroup("iswaypoint", 1, "next");
            int timeOffset = -1;
            FOREACH(itgroup, newSpec._vgroups) {
                if( itgroup->name == "deltatime" ) {
                    timeOffset = itgroup->offset;
                }
                else if( velSpec.FindCompatibleGroup(*itgroup) != velSpec._vgroups.end() ) {
                    itgroup->interpolation = "linear";
                }
                else if( posSpec.FindCompatibleGroup(*itgroup) != posSpec._vgroups.end() ) {
                    itgroup->interpolation = "quadratic";
                }
            }

            // Write shortcut trajectory to dummytraj first
            if( !_pdummytraj || (_pdummytraj->GetXMLId() != ptraj->GetXMLId()) ) {
                _pdummytraj = RaveCreateTrajectory(GetEnv(), ptraj->GetXMLId());
            }
            _pdummytraj->Init(newSpec);

            // Consistency checking
            FOREACH(itrampnd, parabolicpath.GetRampNDVect()) {
                OPENRAVE_ASSERT_OP((int) itrampnd->GetDOF(), ==, _parameters->GetDOF());
            }

            RAVELOG_DEBUG_FORMAT("env=%d, start inserting the first waypoint to dummytraj", _environmentid);
            waypoints.resize(newSpec.GetDOF()); // reuse _cacheWaypoints

            ConfigurationSpecification::ConvertData(waypoints.begin(), newSpec, parabolicpath.GetRampNDVect().front().GetX0Vect(), posSpec, 1, GetEnv(), true);
            ConfigurationSpecification::ConvertData(waypoints.begin(), newSpec, parabolicpath.GetRampNDVect().front().GetV0Vect(), velSpec, 1, GetEnv(), false);
            waypoints[waypointOffset] = 1;
            waypoints.at(timeOffset) = 0;
            _pdummytraj->Insert(_pdummytraj->GetNumWaypoints(), waypoints);

            RampOptimizer::RampND& rampndTrimmed = _cacheRampND; // another reference to _cacheRampND
            RampOptimizer::RampND& frontTrimRampND = _cacheFrontTrimRampND, &backTrimRampND = _cacheBackTrimRampND;
            frontTrimRampND.Initialize(_parameters->GetDOF());
            backTrimRampND.Initialize(_parameters->GetDOF());
            std::vector<RampOptimizer::RampND>& tempRampNDVect = _cacheRampNDVect;
            dReal fTrimEdgesTime = parameters->_fStepLength*2; // we ignore collisions duration [0, fTrimEdgesTime] and [fTrimEdgesTime, duration]
            dReal fExpextedDuration = 0; // for consistency checking
            dReal durationDiscrepancyThresh = 0.01; // for consistency checking

            for (size_t irampnd = 0; irampnd < parabolicpath.GetRampNDVect().size(); ++irampnd) {
                rampndTrimmed = parabolicpath.GetRampNDVect()[irampnd];

                if( !(_parameters->_hastimestamps && itcompatposgroup->interpolation == "quadratic" && numShortcuts == 0) || !rampndTrimmed.constraintChecked ) {
                    // When we read waypoints from the initial trajectory, the re-computation of
                    // accelerations (RampND::Initialize) can introduce some small discrepancy and
                    // trigger the error although the initial trajectory is perfectly
                    // fine. Therefore, if the initial trajectory is quadratic (meaning that the
                    // checking has already been done to verify the trajectory) and there is no
                    // other modification to this trajectory, we can *safely* skip CheckRampND and
                    // go for collision checking and other constraint checking.
                    RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampND(rampndTrimmed, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit);
                    OPENRAVE_ASSERT_OP(parabolicret, ==, RampOptimizer::PCR_Normal);
                }

                // tempRampNDVect will contain the finalized result of each RampND
                tempRampNDVect.resize(1);
                tempRampNDVect[0] = rampndTrimmed; // copy rampndTrimmed into tempRampNDVect[0]
                ++_progress._iteration;

                // Check constraints if not yet checked.
                if( !rampndTrimmed.constraintChecked ) {
                    bool bTrimmedFront = false;
                    bool bTrimmedBack = false;
                    bool bCheck = true;
                    if( irampnd == 0 ) {
                        if( rampndTrimmed.GetDuration() <= fTrimEdgesTime + g_fEpsilonLinear ) {
                            // The initial RampND is too short so ignore checking
                            bCheck = false;
                        }
                        else {
                            frontTrimRampND = rampndTrimmed; // original
                            frontTrimRampND.Cut(fTrimEdgesTime, rampndTrimmed);
                            bTrimmedFront = true;
                        }
                    }
                    if( irampnd + 1 == parabolicpath.GetRampNDVect().size() ) {
                        if( rampndTrimmed.GetDuration() <= fTrimEdgesTime + g_fEpsilonLinear ) {
                            // The final RampND is too short so ignore checking
                            bCheck = false;
                        }
                        else {
                            rampndTrimmed.Cut(rampndTrimmed.GetDuration() - fTrimEdgesTime, backTrimRampND);
                            bTrimmedBack = true;
                        }
                    }

                    _bUsePerturbation = false;

                    std::vector<RampOptimizer::RampND>& rampndVectOut = _cacheRampNDVectOut;
                    if( bCheck ) {
                        RampOptimizer::CheckReturn checkret = _feasibilitychecker.Check2(rampndTrimmed, 0xffff|CFO_FromTrajectorySmoother, rampndVectOut);
#ifdef SMOOTHER2_TIMING_DEBUG
                        _nCallsCheckPathAllConstraints += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                        _totalTimeCheckPathAllConstraints += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                        if( checkret.retcode != 0 ) {
                            _nCallsCheckPathAllConstraintsInVain += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                            _totalTimeCheckPathAllConstraintsInVain += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                        }
                        // Reset SegmentFeasible2 counters
                        _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                        _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif
                        if( checkret.retcode != 0 ) {
                            RAVELOG_DEBUG_FORMAT("env=%d, Check2 for RampND %d/%d return retcode=0x%x", _environmentid%irampnd%parabolicpath.GetRampNDVect().size()%checkret.retcode);

                            bool bSuccess = false;
                            // Try to stretch the duration of the RampND in hopes of fixing constraints violation.
                            dReal newDuration = rampndTrimmed.GetDuration();
                            newDuration += 5*RampOptimizer::g_fRampEpsilon;
                            dReal timeIncrement = 0.05*newDuration;
                            size_t maxTries = 4;

                            rampndTrimmed.GetX0Vect(x0Vect);
                            rampndTrimmed.GetX1Vect(x1Vect);
                            rampndTrimmed.GetV0Vect(v0Vect);
                            rampndTrimmed.GetV1Vect(v1Vect);
                            for (size_t iDilate = 0; iDilate < maxTries; ++iDilate) {
#ifdef SMOOTHER2_TIMING_DEBUG
                                _nCallsInterpolator += 1;
                                _tStartInterpolator = utils::GetMicroTime();
#endif
                                bool result = _interpolator.ComputeNDTrajectoryFixedDuration(x0Vect, x1Vect, v0Vect, v1Vect, newDuration, parameters->_vConfigLowerLimit, parameters->_vConfigUpperLimit, parameters->_vConfigVelocityLimit, parameters->_vConfigAccelerationLimit, rampndVectOut);
#ifdef SMOOTHER2_TIMING_DEBUG
                                _tEndInterpolator = utils::GetMicroTime();
                                _totalTimeInterpolator += 0.000001f*(float)(_tEndInterpolator - _tStartInterpolator);
#endif
                                if( result ) {
                                    // Stretching is successful
                                    RAVELOG_VERBOSE_FORMAT("env=%d, duration %.15e -> %.15e", _environmentid%rampndTrimmed.GetDuration()%newDuration);
                                    RampOptimizer::CheckReturn newrampndret = _feasibilitychecker.Check2(rampndVectOut, 0xffff|CFO_FromTrajectorySmoother, tempRampNDVect);
#ifdef SMOOTHER2_TIMING_DEBUG
                                    _nCallsCheckPathAllConstraints += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                                    _totalTimeCheckPathAllConstraints += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                                    if( newrampndret.retcode != 0 ) {
                                        _nCallsCheckPathAllConstraintsInVain += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                                        _totalTimeCheckPathAllConstraintsInVain += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                                    }
                                    // Reset SegmentFeasible2 counters
                                    _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                                    _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif

                                    if( newrampndret.retcode == 0 && !newrampndret.bDifferentVelocity ) {
                                        // The new RampND passes the check. Need to re-populate tempRampNDVect with
                                        // RampNDs from rampndVectOut instead.
                                        tempRampNDVect.resize(0);
                                        if( tempRampNDVect.capacity() < rampndVectOut.size() + 1 ) {
                                            tempRampNDVect.reserve(rampndVectOut.size() + 1);
                                        }

                                        if( bTrimmedFront ) {
                                            tempRampNDVect.push_back(frontTrimRampND);
                                        }
                                        FOREACHC(itrampndVectOut, rampndVectOut) {
                                            tempRampNDVect.push_back(*itrampndVectOut);
                                        }
                                        if( bTrimmedBack ) {
                                            tempRampNDVect.push_back(backTrimRampND);
                                        }
                                        bSuccess = true;
                                        break;
                                    }
                                }

                                // ComputeNDTrajectoryFixedDuration failed or Check2 failed.
                                if( iDilate > 1 ) {
                                    newDuration += timeIncrement;
                                }
                                else {
                                    // Start slowly
                                    newDuration += 5*RampOptimizer::g_fRampEpsilon;
                                }
                            }
                            // Finished stretching.

                            if( !bSuccess ) {
                                std::string description = "";
                                if (IS_DEBUGLEVEL(Level_Verbose)) {
                                    std::stringstream ss;
                                    ss << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                                    ss << "x0 = [";
                                    SerializeValues(ss, x0Vect);
                                    ss << "]; x1 = [";
                                    SerializeValues(ss, x1Vect);
                                    ss << "]; v0 = [";
                                    SerializeValues(ss, v0Vect);
                                    ss << "]; v1 = [";
                                    SerializeValues(ss, v1Vect);
                                    ss << "]; deltatime = " << rampndTrimmed.GetDuration();
                                    description = str(boost::format("env=%d, original RampND %d/%d does not satisfy constraints. retcode=0x%x. %s")%_environmentid%irampnd%parabolicpath.GetRampNDVect().size()%checkret.retcode%ss.str());
                                }
                                else {
                                    description = str(boost::format("env=%d, original RampND %d/%d does not satisfy constraints. retcode=0x%x")%_environmentid%irampnd%parabolicpath.GetRampNDVect().size()%checkret.retcode);
                                }
                                RAVELOG_WARN(description);
                                _DumpTrajectory(ptraj, _dumplevel, 3);
                                return OPENRAVE_PLANNER_STATUS(description, PS_Failed);
                            }
                        }
                    }
                    _bUsePerturbation = true;
                    ++_progress._iteration;

                    if( _CallCallbacks(_progress) == PA_Interrupt ) {
                        return OPENRAVE_PLANNER_STATUS(str(boost::format("env=%d, Planning was interrupted")%_environmentid), PS_Interrupted);
                    }
                }// Finished checking constraints

                waypoints.resize(newSpec.GetDOF());
                FOREACH(itrampnd, tempRampNDVect) {
                    fExpextedDuration += itrampnd->GetDuration();
                    itrampnd->GetX1Vect(x1Vect);
                    ConfigurationSpecification::ConvertData(waypoints.begin(), newSpec, x1Vect.begin(), posSpec, 1, GetEnv(), true);
                    itrampnd->GetV1Vect(v1Vect);
                    ConfigurationSpecification::ConvertData(waypoints.begin(), newSpec, v1Vect.begin(), velSpec, 1, GetEnv(), false);

                    *(waypoints.begin() + timeOffset) = itrampnd->GetDuration();
                    *(waypoints.begin() + waypointOffset) = 1;
                    _pdummytraj->Insert(_pdummytraj->GetNumWaypoints(), waypoints);
                }

                if( IS_DEBUGLEVEL(Level_Verbose) ) {
                    // If verbose, do tighter bound checking
                    OPENRAVE_ASSERT_OP(RaveFabs(fExpextedDuration - _pdummytraj->GetDuration()), <=, 0.1*durationDiscrepancyThresh);
                }
            }
            OPENRAVE_ASSERT_OP(RaveFabs(fExpextedDuration - _pdummytraj->GetDuration()), <=, durationDiscrepancyThresh);
            ptraj->Swap(_pdummytraj);
        }
        catch (const std::exception& ex) {
            _DumpTrajectory(ptraj, _dumplevel, 4);
            std::string description = str(boost::format("env=%d, Main planning loop threw exception %s")%_environmentid%ex.what());
            RAVELOG_WARN(description);
            return OPENRAVE_PLANNER_STATUS(description, PS_Failed);
        }

        if( mergeStartTime != 0 && shortcutStartTime != 0 && conversionStartTime != 0 ) {
            const uint64_t currentTime = utils::GetMicroTime();
            RAVELOG_DEBUG_FORMAT("env=%d, path optimizing - computation time = %f s. (init=%fs, merge=%fs, shortcut=%fs, conv=%fs)", _environmentid%(1e-6*(currentTime - baseTime))%(1e-6*(mergeStartTime - baseTime))%(1e-6*(shortcutStartTime - mergeStartTime))%(1e-6*(conversionStartTime - shortcutStartTime))%(1e-6*(currentTime - conversionStartTime)));
        }
        else {
            RAVELOG_DEBUG_FORMAT("env=%d, path optimizing - computation time = %f s.", _environmentid%(1e-6*(utils::GetMicroTime() - baseTime)));
        }

        if( IS_DEBUGLEVEL(Level_Verbose) || (RaveGetDebugLevel() & Level_VerifyPlans) ) {
            RAVELOG_DEBUG_FORMAT("env=%d, Start sampling trajectory after shortcutting (for verification)", _environmentid);
            try {
                ptraj->Sample(x0Vect, 0); // reuse x0Vect
                RAVELOG_DEBUG_FORMAT("env=%d, Sampling for verification successful", _environmentid);
            }
            catch (const std::exception& ex) {
                std::string description = str(boost::format("env=%d, Sampling for verification failed: %s")%_environmentid%ex.what());
                RAVELOG_WARN(description);
                _DumpTrajectory(ptraj, _dumplevel, 5);
                return OPENRAVE_PLANNER_STATUS(description, PS_Failed);
            }
        }
        _DumpTrajectory(ptraj, _dumplevel, 6);

#ifdef SMOOTHER2_TIMING_DEBUG
        dReal tTotalShortcutTime = 0.000001f*(float)(_tShortcutEnd - _tShortcutStart);
        RAVELOG_INFO_FORMAT("env=%d, shortcutting time=%.15e; iter=%d; time/iter=%.15e", _environmentid%tTotalShortcutTime%_numShortcutIters%(tTotalShortcutTime/_numShortcutIters));
        RAVELOG_INFO_FORMAT("env=%d, measured %d interpolations; total exectime=%.15e; time/iter=%.15e", _environmentid%_nCallsInterpolator%_totalTimeInterpolator%(_totalTimeInterpolator/_nCallsInterpolator));
        RAVELOG_INFO_FORMAT("env=%d, measured %d checkmanips; total exectime=%.15e; time/iter=%.15e", _environmentid%_nCallsCheckManip%_totalTimeCheckManip%(_nCallsCheckManip == 0 ? 0 : _totalTimeCheckManip/_nCallsCheckManip));
        RAVELOG_INFO_FORMAT("env=%d, measured %d checkpathallconstraints; total exectime=%.15e; time/iter=%.15e", _environmentid%_nCallsCheckPathAllConstraints%_totalTimeCheckPathAllConstraints%(_nCallsCheckPathAllConstraints == 0 ? 0 : _totalTimeCheckPathAllConstraints/_nCallsCheckPathAllConstraints));
        RAVELOG_INFO_FORMAT("env=%d, measured %d checkpathallconstraints (in vain); total exectime=%.15e", _environmentid%_nCallsCheckPathAllConstraintsInVain%_totalTimeCheckPathAllConstraintsInVain);
#endif
        return _ProcessPostPlanners(RobotBasePtr(), ptraj);
    }

    virtual int ConfigFeasible(const std::vector<dReal>& q0, const std::vector<dReal>& dq0, int options)
    {
        if( _bUsePerturbation ) {
            options |= CFO_CheckWithPerturbation;
        }
        try {
            return _parameters->CheckPathAllConstraints(q0, q0, dq0, dq0, 0, IT_OpenStart, options);
        }
        catch (const std::exception& ex) {
            RAVELOG_WARN_FORMAT("env=%d, CheckPathAllConstraints threw an exception: %s", _environmentid%ex.what());
            return 0xffff|CFO_FromTrajectorySmoother;
        }
    }

    /// \brief ConfigFeasible2 does the same thing as ConfigFeasible. The difference is that it
    /// returns RampOptimizer::CheckReturn instead of an int. fTimeBasedSurpassMult is also set to
    /// some value if the configuration violates some time-based constraints.
    virtual RampOptimizer::CheckReturn ConfigFeasible2(const std::vector<dReal>& q0, const std::vector<dReal>& dq0, int options)
    {
        if( _bUsePerturbation ) {
            options |= CFO_CheckWithPerturbation;
        }
        try {
#ifdef SMOOTHER2_TIMING_DEBUG
            _nCallsCheckPathAllConstraints_SegmentFeasible2 += 1;
            _tStartCheckPathAllConstraints = utils::GetMicroTime();
#endif
            int ret = _parameters->CheckPathAllConstraints(q0, q0, dq0, dq0, 0, IT_OpenStart, options, _constraintreturn);
#ifdef SMOOTHER2_TIMING_DEBUG
            _tEndCheckPathAllConstraints = utils::GetMicroTime();
            _totalTimeCheckPathAllConstraints_SegmentFeasible2 += 0.000001f*(float)(_tEndCheckPathAllConstraints - _tStartCheckPathAllConstraints);
#endif
            RampOptimizer::CheckReturn checkret(ret);
            if( ret == CFO_CheckTimeBasedConstraints ) {
                checkret.fTimeBasedSurpassMult = 0.98 * _constraintreturn->_fTimeBasedSurpassMult;
            }
            return checkret;
        }
        catch (const std::exception& ex) {
            RAVELOG_WARN_FORMAT("env=%d, CheckPathAllConstraints threw an exception: %s", _environmentid%ex.what());
            return 0xffff|CFO_FromTrajectorySmoother;
        }
    }

    /// \brief Check if the segment interpolating (q0, dq0) and (q1, dq1) is feasible. The function
    /// first calls CheckPathAllConstraints to check all constraints. Since the input path may be
    /// modified from inside CheckPathAllConstraints, after the checking this function also try to
    /// correct any discrepancy occured.
    virtual RampOptimizer::CheckReturn SegmentFeasible2(const std::vector<dReal>& q0, const std::vector<dReal>& q1, const std::vector<dReal>& dq0, const std::vector<dReal>& dq1, dReal timeElapsed, int options, std::vector<RampOptimizer::RampND>& rampndVectOut, std::vector<dReal>& vIntermediateConfigurations)
    {
        size_t ndof = q0.size();

        if( timeElapsed <= g_fEpsilon ) {
            rampndVectOut.resize(1);
            rampndVectOut[0].Initialize(_parameters->GetDOF());
            rampndVectOut[0].SetConstant(q0, 0);
            rampndVectOut[0].SetV0Vect(dq0);
            rampndVectOut[0].SetV1Vect(dq1);
            return ConfigFeasible2(q0, dq0, options);
        }

        rampndVectOut.resize(0);
        if( _bUsePerturbation ) {
            options |= CFO_CheckWithPerturbation;
        }

        bool bExpectedModifiedConfigurations = (_parameters->fCosManipAngleThresh > -1 + g_fEpsilonLinear);
        if( bExpectedModifiedConfigurations || _bmanipconstraints ) {
            options |= CFO_FillCheckedConfiguration;
            _constraintreturn->Clear();
        }

        // Since the actual path connceting q0 and q1 (returned from CheckPathAllConstraints) may not simply be a
        // straight line due to other tool constraints, checking tool speed/accel constraints on this straight line and
        // rejecting the segment based on that is not meaningful.
        if( 0 ) {//( _bmanipconstraints && (options & CFO_CheckTimeBasedConstraints) ) {
            // Check manip constraints for early rejection
            _cacheRampNDSeg.Initialize(q0, q1, dq0, dq1, std::vector<dReal>(), timeElapsed);
            rampndVectOut.push_back(_cacheRampNDSeg);
            try {
#ifdef SMOOTHER2_TIMING_DEBUG
                _nCallsCheckManip += 1;
                _tStartCheckManip = utils::GetMicroTime();
#endif
                RampOptimizer::CheckReturn retmanip = _manipconstraintchecker->CheckManipConstraints2(rampndVectOut, IT_OpenStart, _bUseNewHeuristic);
#ifdef SMOOTHER2_TIMING_DEBUG
                _tEndCheckManip = utils::GetMicroTime();
                _totalTimeCheckManip += 0.000001f*(float)(_tEndCheckManip - _tStartCheckManip);
#endif
                if( retmanip.retcode != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                    RAVELOG_DEBUG_FORMAT("env=%d, early rejection due to manipconstraints, CheckManipConstraints2 returns retcode=0x%x", _environmentid%retmanip.retcode);
#endif
                    return retmanip;
                }
            }
            catch (const std::exception& ex) {
                RAVELOG_WARN_FORMAT("env=%d, CheckManipConstraints2 (modified=%d) threw an exception: %s", _environmentid%((int) bExpectedModifiedConfigurations)%ex.what());
                return RampOptimizer::CheckReturn(0xffff|CFO_FromTrajectorySmoother);
            }
            rampndVectOut.resize(0);
        }

        try {
#ifdef SMOOTHER2_TIMING_DEBUG
            _nCallsCheckPathAllConstraints_SegmentFeasible2 += 1;
            _tStartCheckPathAllConstraints = utils::GetMicroTime();
#endif
            int ret = _parameters->CheckPathAllConstraints(q0, q1, dq0, dq1, timeElapsed, IT_OpenStart, options, _constraintreturn);
#ifdef SMOOTHER2_TIMING_DEBUG
            _tEndCheckPathAllConstraints = utils::GetMicroTime();
            _totalTimeCheckPathAllConstraints_SegmentFeasible2 += 0.000001f*(float)(_tEndCheckPathAllConstraints - _tStartCheckPathAllConstraints);
#endif
            if( ret != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                if( _constraintreturn->_configurationtimes.size() > 0 ) {
                    RAVELOG_DEBUG_FORMAT("env=%d, rejection by CheckPathAllConstraints at timestamp=%.6e/%.6e, retcode=0x%x", _environmentid%_constraintreturn->_configurationtimes.back()%timeElapsed%ret);
                }
                else {
                    RAVELOG_DEBUG_FORMAT("env=%d, rejection by CheckPathAllConstraints, retcode=0x%x", _environmentid%ret);
                }
#endif
                RampOptimizer::CheckReturn checkret(ret);
                if( ret == CFO_CheckTimeBasedConstraints ) {
                    checkret.fTimeBasedSurpassMult = 0.98 * _constraintreturn->_fTimeBasedSurpassMult;
                }
                return checkret;
            }
        }
        catch (const std::exception& ex) {
            RAVELOG_WARN_FORMAT("env=%d, CheckPathAllConstraints threw an exception: %s", _environmentid%ex.what());
            return RampOptimizer::CheckReturn(0xffff|CFO_FromTrajectorySmoother);
        }

        // Configurations between (q0, dq0) and (q1, dq1) may have been modified.
        if( bExpectedModifiedConfigurations && _constraintreturn->_configurationtimes.size() > 0 ) {
            OPENRAVE_ASSERT_OP(_constraintreturn->_configurations.size(), ==, _constraintreturn->_configurationtimes.size()*ndof);

            std::vector<dReal>& curPos = _cacheCurPos, &newPos = _cacheNewPos, &curVel = _cacheCurVel, &newVel = _cacheNewVel;
            curPos = q0;
            curVel = dq0;

            std::vector<dReal>::const_iterator it = _constraintreturn->_configurations.begin();
            dReal curTime = 0;

            std::vector<dReal> vdofscaling(ndof); // for recording limits violation
            std::fill(vdofscaling.begin(), vdofscaling.end(), 1);
            bool bviolated = false;
            for (size_t itime = 0; itime < _constraintreturn->_configurationtimes.size(); ++itime, it += ndof) {
                std::copy(it, it + ndof, newPos.begin());
                dReal deltaTime = _constraintreturn->_configurationtimes[itime] - curTime;
                if( deltaTime > RampOptimizer::g_fRampEpsilon ) {
                    dReal iDeltaTime = 1/deltaTime;

                    // Compute the next velocity for each DOF as well as check consistency
                    bviolated = false;
                    for (size_t idof = 0; idof < ndof; ++idof) {
                        newVel[idof] = 2*iDeltaTime*(newPos[idof] - curPos[idof]) - curVel[idof];

                        // Check velocity limit
                        if( RaveFabs(newVel[idof]) > _parameters->_vConfigVelocityLimit[idof] + RampOptimizer::g_fRampEpsilon  ) {
                            bviolated = true;
                            if( 0.9*_parameters->_vConfigVelocityLimit[idof] < 0.1*RaveFabs(newVel[idof]) ) {
                                // Warn if the velocity is really too high
                                RAVELOG_WARN_FORMAT("env=%d, the new velocity for idof=%d is too high. |%.15e| > %.15e", _environmentid%idof%newVel[idof]%_parameters->_vConfigVelocityLimit[idof]);
                            }
#ifdef SMOOTHER2_PROGRESS_DEBUG
                            RAVELOG_DEBUG_FORMAT("env=%d, velocity exceeds limits after CheckPathAllConstraints, idof=%d, newVel=%.15e, vellimit=%.15e, diff=%.15e", _environmentid%idof%newVel[idof]%_parameters->_vConfigVelocityLimit[idof]%(RaveFabs(newVel[idof]) - _parameters->_vConfigVelocityLimit[idof]));
#endif
                            if( !_bUseNewHeuristic ) {
                                return RampOptimizer::CheckReturn(CFO_CheckTimeBasedConstraints, 0.9*_parameters->_vConfigVelocityLimit[idof]/RaveFabs(newVel[idof]));
                            }
                            vdofscaling[idof] = 0.9*_parameters->_vConfigVelocityLimit[idof]/RaveFabs(newVel[idof]);
                        }
                    }
                    if( bviolated ) {
                        return RampOptimizer::CheckReturn(CFO_CheckTimeBasedConstraints, vdofscaling);
                    }

                    // The computed next velocity is fine.
                    _cacheRampNDSeg.Initialize(curPos, newPos, curVel, newVel, std::vector<dReal>(), deltaTime);

                    // Now check the acceleration
                    bool bAccelChanged = false;
                    for (size_t idof = 0; idof < ndof; ++idof) {
                        if( _cacheRampNDSeg.GetAAt(idof) < -_parameters->_vConfigAccelerationLimit[idof] ) {
                            RAVELOG_VERBOSE_FORMAT("env=%d, idof=%d, accel changed: %.15e --> %.15e; diff=%.15e", _environmentid%idof%_cacheRampNDSeg.GetAAt(idof)%(-_parameters->_vConfigAccelerationLimit[idof])%(_cacheRampNDSeg.GetAAt(idof) + _parameters->_vConfigAccelerationLimit[idof]));
                            _cacheRampNDSeg.GetAAt(idof) = -_parameters->_vConfigAccelerationLimit[idof];
                            bAccelChanged = true;
                        }
                        else if( _cacheRampNDSeg.GetAAt(idof) > _parameters->_vConfigAccelerationLimit[idof] ) {
                            RAVELOG_VERBOSE_FORMAT("env=%d, idof=%d, accel changed: %.15e --> %.15e; diff=%.15e", _environmentid%idof%_cacheRampNDSeg.GetAAt(idof)%(_parameters->_vConfigAccelerationLimit[idof])%(_cacheRampNDSeg.GetAAt(idof) - _parameters->_vConfigAccelerationLimit[idof]));
                            _cacheRampNDSeg.GetAAt(idof) = _parameters->_vConfigAccelerationLimit[idof];
                            bAccelChanged = true;
                        }
                    }
                    if( bAccelChanged ) {
                        RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampND(_cacheRampNDSeg, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit);
                        if( parabolicret != RampOptimizer::PCR_Normal ) {
                            _sslog.str(""); _sslog.clear();
                            _sslog << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                            _sslog << "x0=[";
                            SerializeValues(_sslog, curPos);
                            _sslog << "]; x1=[";
                            SerializeValues(_sslog, newPos);
                            _sslog << "]; v0=[";
                            SerializeValues(_sslog, curVel);
                            _sslog << "]; v1=[";
                            SerializeValues(_sslog, newVel);
                            _sslog << "]; deltatime=" << deltaTime;

                            RAVELOG_WARN_FORMAT("env=%d, the output RampND becomes invalid (ret=%x) after fixing accelerations. %s", _environmentid%parabolicret%_sslog.str());
                            return RampOptimizer::CheckReturn(CFO_CheckTimeBasedConstraints, 0.9);
                        }
                    }
                    _cacheRampNDSeg.constraintChecked = true;

                    rampndVectOut.push_back(_cacheRampNDSeg);
                    curTime = _constraintreturn->_configurationtimes[itime];
                    curPos.swap(newPos);
                    curVel.swap(newVel);
                }
            }

            // Make sure the last configuration ends at the desired value.
            for (size_t idof = 0; idof < ndof; ++idof) {
                if( RaveFabs(curPos[idof] - q1[idof]) + g_fEpsilon > RampOptimizer::g_fRampEpsilon ) {
                    RAVELOG_WARN_FORMAT("env=%d, discrepancy at the last configuration: curPos[%d] (%.15e) != q1[%d] (%.15e)", _environmentid%idof%curPos[idof]%idof%q1[idof]);
                    return RampOptimizer::CheckReturn(CFO_FinalValuesNotReached);
                }
            }
        }
        else if( _constraintreturn->_configurationtimes.size() > 0 ) {
            // No manip tool direction constraint but CFO_FillCheckedConfiguration is enabled. We do
            // this because we want to keep the intermediate configurations for collision checking
            // at a later stage.
            vIntermediateConfigurations.insert(vIntermediateConfigurations.end(), _constraintreturn->_configurations.begin(), _constraintreturn->_configurations.end());
        }

        if( rampndVectOut.size() == 0 ) {
            _cacheRampNDSeg.Initialize(q0, q1, dq0, dq1, std::vector<dReal>(), timeElapsed);
            // Now check the acceleration
            bool bAccelChanged = false;
            for (size_t idof = 0; idof < ndof; ++idof) {
                if( _cacheRampNDSeg.GetAAt(idof) < -_parameters->_vConfigAccelerationLimit[idof] ) {
                    RAVELOG_VERBOSE_FORMAT("env=%d, idof=%d, accel changed: %.15e --> %.15e; diff=%.15e", _environmentid%idof%_cacheRampNDSeg.GetAAt(idof)%(-_parameters->_vConfigAccelerationLimit[idof])%(_cacheRampNDSeg.GetAAt(idof) + _parameters->_vConfigAccelerationLimit[idof]));
                    _cacheRampNDSeg.GetAAt(idof) = -_parameters->_vConfigAccelerationLimit[idof];
                    bAccelChanged = true;
                }
                else if( _cacheRampNDSeg.GetAAt(idof) > _parameters->_vConfigAccelerationLimit[idof] ) {
                    RAVELOG_VERBOSE_FORMAT("env=%d, idof=%d, accel changed: %.15e --> %.15e; diff=%.15e", _environmentid%idof%_cacheRampNDSeg.GetAAt(idof)%(_parameters->_vConfigAccelerationLimit[idof])%(_cacheRampNDSeg.GetAAt(idof) - _parameters->_vConfigAccelerationLimit[idof]));
                    _cacheRampNDSeg.GetAAt(idof) = _parameters->_vConfigAccelerationLimit[idof];
                    bAccelChanged = true;
                }
            }
            if( bAccelChanged ) { // Make sure the modification is valid
                RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampND(_cacheRampNDSeg, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit);
                if( parabolicret != RampOptimizer::PCR_Normal ) {
                    _sslog.str(""); _sslog.clear();
                    _sslog << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                    _sslog << "x0=[";
                    SerializeValues(_sslog, q0);
                    _sslog << "]; x1=[";
                    SerializeValues(_sslog, q1);
                    _sslog << "]; v0=[";
                    SerializeValues(_sslog, dq0);
                    _sslog << "]; v1=[";
                    SerializeValues(_sslog, dq1);
                    _sslog << "]; deltatime=" << timeElapsed;

                    RAVELOG_WARN_FORMAT("env=%d, the output RampND becomes invalid (ret=%x) after fixing accelerations. %s", _environmentid%parabolicret%_sslog.str());
                    return RampOptimizer::CheckReturn(CFO_CheckTimeBasedConstraints, 0.9);
                }
            }
            _cacheRampNDSeg.constraintChecked = true;
            rampndVectOut.push_back(_cacheRampNDSeg);
        }

        if( _bmanipconstraints && (options & CFO_CheckTimeBasedConstraints) ) {
            try {
#ifdef SMOOTHER2_TIMING_DEBUG
                _nCallsCheckManip += 1;
                _tStartCheckManip = utils::GetMicroTime();
#endif
                RampOptimizer::CheckReturn retmanip = _manipconstraintchecker->CheckManipConstraints2(rampndVectOut, IT_Closed, _bUseNewHeuristic);
#ifdef SMOOTHER2_TIMING_DEBUG
                _tEndCheckManip = utils::GetMicroTime();
                _totalTimeCheckManip += 0.000001f*(float)(_tEndCheckManip - _tStartCheckManip);
#endif
                if( retmanip.retcode != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                    RAVELOG_VERBOSE_FORMAT("env=%d, CheckManipConstraints2 returns retcode=0x%x", _environmentid%retmanip.retcode);
#endif
                    return retmanip;
                }
            }
            catch (const std::exception& ex) {
                RAVELOG_VERBOSE_FORMAT("env=%d, CheckManipConstraints2 (modified=%d) threw an exception: %s", _environmentid%((int) bExpectedModifiedConfigurations)%ex.what());
                return RampOptimizer::CheckReturn(0xffff|CFO_FromTrajectorySmoother);
            }
        }

        return RampOptimizer::CheckReturn(0);
    }

    virtual dReal Rand()
    {
        return _uniformsampler->SampleSequenceOneReal(IT_OpenEnd);
    }

    virtual bool NeedDerivativeForFeasibility()
    {
        return true;
    }

protected:

    enum ShortcutStatus
    {
        SS_Successful = 1,
        SS_TimeInstantsTooClose = 2,       // the sampled time instants t0 and t1 are closer than the specified threshold
        SS_RedundantShortcut = 3,          // the sampled time instants t0 and t1 fall into the same bins as a previously failed shortcut. see vVisitedDiscretization for details.
        SS_InitialInterpolationFailed = 4, // interpolation fails.
        SS_InterpolatedSegmentTooLong = 5, // interpolated segment from t0 to t1 is not shorter than t1 - t0 by at least minTimeStep
        SS_InterpolatedSegmentTooLongFromSlowDown = 6, // interpolated segment from t0 to t1 is not shorter than t1 - t0 by at least minTimeStep because of reduced vel/accel limits
        SS_Check2CollisionFailed = 7,      // interpolated segment is not collision free.
        SS_Check2Failed = 8,               // interpolated segment violates some constraints that are not 0x1 (collision) or 0x4 (time-based).
        SS_MaxManipSpeedFailed = 9,        // vel and/or accel multipliers get too low because of max manip speed
        SS_MaxManipAccelFailed = 10,       // vel and/or accel multipliers get too low because of max manip accel
        SS_SlowDownFailed = 11,            // vel and/or accel multipliers get too low becasue of other time-based constraints
        SS_LastSegmentFailed = 12,         // interpolation failed or segment too long or check2 failed or ending with different velocity
        SS_StateSettingFailed = 13,        // error occured when setting a state.
    };

    /// \brief Keep related information about a zero-velocity point so that we can focus the sampling more around these
    /// points when shortcutting.
    struct ZeroVelPointInfo
    {
        ZeroVelPointInfo() : point(-1), leftneighbor(-1), rightneighbor(-1) {
        }
        dReal point;         // zero-velocity point, i.e., time instant when all dof velocities are zero.
        dReal leftneighbor;  // the first switch time to the left of this zero-velocity point
        dReal rightneighbor; // the first switch time to the right of this zero-velocity point
    };

    /// \brief Time-parameterize the ordered set of waypoints to a trajectory that stops at every
    /// waypoint. _SetMilestones also adds some extra waypoints to the original set if any two
    /// consecutive waypoints are too far apart.
    bool _SetMileStones(const std::vector<std::vector<dReal> >& vWaypoints, RampOptimizer::ParabolicPath& parabolicpath)
    {
        _vZeroVelPointInfos.clear();
        _vZeroVelPointInfos.reserve(vWaypoints.size());

        size_t ndof = _parameters->GetDOF();
        parabolicpath.Reset();
        RAVELOG_VERBOSE_FORMAT("env=%d, Initial numwaypoints = %d", _environmentid%vWaypoints.size());

        if( vWaypoints.size() == 1 ) {
            std::vector<RampOptimizer::RampND>& rampndVect = _cacheRampNDVect;
            rampndVect.resize(1);
            rampndVect[0].Initialize(_parameters->GetDOF());
            rampndVect[0].SetConstant(vWaypoints[0], 0);
            parabolicpath.Initialize(rampndVect[0]);
        }
        else if( vWaypoints.size() > 1 ) {
            int options = CFO_CheckTimeBasedConstraints;
            if( !_parameters->verifyinitialpath ) {
                options = options & (~CFO_CheckEnvCollisions) & (~CFO_CheckSelfCollisions);
                RAVELOG_VERBOSE_FORMAT("env=%d, Initial path verification disabled using options=0x%x", _environmentid%options);
            }

            // In some cases (e.g. when there are manipulator constraints), the midpoint 0.5*(x0 +
            // x1) may not satisfy the constraints. Instead of returning failure, we try to compute
            // a better midpoint.
            std::vector<std::vector<dReal> >& vNewWaypoints = _cacheNewWaypointsVect;
            std::vector<uint8_t> vForceInitialChecking(vWaypoints.size(), 0);

            if( !!_parameters->_neighstatefn ) {
                std::vector<dReal> xmid(ndof), xmidDelta(ndof);
                vNewWaypoints = vWaypoints;

                // We add more waypoints in between the original consecutive waypoints x0 and x1 if
                // the constraint-satisfying middle point (computed using _neighstatefn) is far from
                // the expected middle point 0.5*(x0 + x1).
                dReal distThresh = 0.00001;
                int nConsecutiveExpansionsAllowed = 10; // adding too many intermediate waypoints is considered bad
                int nConsecutiveExpansions = 0;
                size_t iwaypoint = 0; // keeps track of the total number of (fianlized) waypoints
                while (iwaypoint + 1 < vNewWaypoints.size()) {
                    // xmidDelta is the difference between the expected middle point and the initial point
                    for (size_t idof = 0; idof < ndof; ++idof) {
                        xmidDelta[idof] = 0.5*(vNewWaypoints[iwaypoint + 1][idof] - vNewWaypoints[iwaypoint][idof]);
                    }

                    xmid = vNewWaypoints[iwaypoint];
                    if( _parameters->SetStateValues(xmid) != 0 ) {
                        RAVELOG_WARN_FORMAT("env=%d, Could not set values at waypoint %d", _environmentid%iwaypoint);
                        return false;
                    }
                    // Steer vNewWaypoints[iwaypoint] by xmidDelta. The resulting state is stored in xmid.
                    if( _parameters->_neighstatefn(xmid, xmidDelta, NSO_OnlyHardConstraints) == NSS_Failed ) {
                        RAVELOG_WARN_FORMAT("env=%d, Failed to get the neighbor of waypoint %d", _environmentid%iwaypoint);
                        return false;
                    }

                    // Check if xmid is far from the expected point.
                    dReal dist = 0;
                    for (size_t idof = 0; idof < ndof; ++idof) {
                        dReal fExpected = 0.5*(vNewWaypoints[iwaypoint + 1][idof] + vNewWaypoints[iwaypoint][idof]);
                        dReal fError = fExpected - xmid[idof];
                        dist += fError*fError;
                    }
                    if( dist > distThresh ) {
                        RAVELOG_DEBUG_FORMAT("env=%d, Adding extra midpoint between waypoints %d and %d, dist = %.15e", _environmentid%iwaypoint%(iwaypoint + 1)%dist);
                        vNewWaypoints.insert(vNewWaypoints.begin() + iwaypoint + 1, xmid);
                        vForceInitialChecking[iwaypoint + 1] = 1;
                        vForceInitialChecking.insert(vForceInitialChecking.begin() + iwaypoint + 1, 1);
                        nConsecutiveExpansions += 2;
                        if( nConsecutiveExpansions > nConsecutiveExpansionsAllowed ) {
                            _sslog.str(""); _sslog.clear();
                            _sslog << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                            _sslog << "env=" << _environmentid << ", Too many consecutive expansions. Segment conecting waypoints " << iwaypoint << " and " << (iwaypoint + 1) << " is bad. waypoint0=[";
                            SerializeValues(_sslog, vNewWaypoints[iwaypoint]);
                            _sslog << "]; waypoint1=[";
                            SerializeValues(_sslog, vNewWaypoints[iwaypoint + 1]);
                            _sslog << "];";
                            RAVELOG_WARN(_sslog.str());
                            return false;
                        }
                        continue;
                    }
                    if( nConsecutiveExpansions > 0 ) {
                        nConsecutiveExpansions--;
                    }
                    iwaypoint += 1;
                }
            }
            else {
                // No _neighstatefn.
                vNewWaypoints = vWaypoints;
            }
            // Finished preparation of waypoints. Now continue to time-parameterize the path.

            OPENRAVE_ASSERT_OP(vNewWaypoints[0].size(), ==, ndof);
            std::vector<RampOptimizer::RampND>& rampndVect = _cacheRampNDVect;
            size_t numWaypoints = vNewWaypoints.size();
            size_t curIndex = 0;
            KinBodyConstPtr usedBody = _dynamicLimitInfo.bUseDynamicLimits ? GetEnv()->GetKinBody(_dynamicLimitInfo.bodyName) : KinBodyConstPtr();
            for (size_t iwaypoint = 1; iwaypoint < numWaypoints; ++iwaypoint) {
                OPENRAVE_ASSERT_OP(vNewWaypoints[iwaypoint].size(), ==, ndof);

                if( !_ComputeRampWithZeroVelEndpoints(vNewWaypoints[iwaypoint - 1], vNewWaypoints[iwaypoint], options, rampndVect, usedBody, iwaypoint, numWaypoints) ) {
#ifdef SMOOTHER2_TIMING_DEBUG
                    // We don't use this stats
                    // Reset SegmentFeasible2 counters
                    _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                    _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif
                    std::stringstream ss;
                    ss << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                    ss << "env=" << _environmentid << ", Failed to time-parameterize the path connecting waypoints " << (iwaypoint - 1) << " and " << iwaypoint << ". waypoint0=[";
                    SerializeValues(ss, vNewWaypoints[iwaypoint - 1]);
                    ss << "]; waypoint1=[";
                    SerializeValues(ss, vNewWaypoints[iwaypoint]);
                    ss << "];";
                    RAVELOG_WARN(ss.str());
                    return false;
                }
#ifdef SMOOTHER2_TIMING_DEBUG
                // We don't use this stats
                // Reset SegmentFeasible2 counters
                _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif

                if( !_parameters->verifyinitialpath && !vForceInitialChecking[iwaypoint] ) {
                    FOREACH(itrampnd, rampndVect) {
                        itrampnd->constraintChecked = true;
                    }
                }

                // Keep track of zero-velocity waypoints
                dReal duration = 0;
                FOREACHC(itrampnd, rampndVect) {
                    duration += itrampnd->GetDuration();
                    parabolicpath.AppendRampND(*itrampnd);
                }
                if( duration > _maxInitialRampTime ) {
                    _maxInitialRampTime = duration;
                }
                curIndex += rampndVect.size();
                ZeroVelPointInfo zerovelpointinfo;
                if( _vZeroVelPointInfos.size() == 0 ) {
                    zerovelpointinfo.point = duration;
                }
                else {
                    zerovelpointinfo.point = _vZeroVelPointInfos.back().point + duration;
                    _vZeroVelPointInfos.back().rightneighbor += rampndVect.front().GetDuration();
                }
                zerovelpointinfo.leftneighbor = zerovelpointinfo.point - rampndVect.back().GetDuration();
                zerovelpointinfo.rightneighbor = zerovelpointinfo.point; // will be updated to the correct value in the next iteration.
                _vZeroVelPointInfos.push_back(zerovelpointinfo);
            }
            if( _vZeroVelPointInfos.size() > 0 ) {
                _vZeroVelPointInfos.pop_back(); // now containing all zero-velocity points except the start and the end
            }
        }
        return true;
    }

    /// \brief Interpolate two given waypoints with a trajectory which starts and ends with zero
    /// velocities. Manip constraints (if available) is also taken care of by gradually scaling
    /// vellimits and accellimits down until the constraints are no longer violated. Therefore, the
    /// usedBody : if not null pointer and dynamic limit is necessary, use it to compute the dynamic limits.
    /// output trajectory (rampnd) is guaranteed to feasible.
    bool _ComputeRampWithZeroVelEndpoints(const std::vector<dReal>& x0VectIn, const std::vector<dReal>& x1VectIn, int options, std::vector<RampOptimizer::RampND>& rampndVectOut, KinBodyConstPtr usedBody, size_t iwaypoint=0, size_t numWaypoints=0)
    {
        // Cache
        std::vector<dReal> &x0Vect = _cacheX0Vect1, &x1Vect = _cacheX1Vect1, &v0Vect = _cacheV0Vect, &v1Vect = _cacheV1Vect;
        std::vector<dReal> &vellimits = _cacheVellimits, &accellimits = _cacheAccelLimits;
        vellimits = _parameters->_vConfigVelocityLimit;
        accellimits = _parameters->_vConfigAccelerationLimit;

        // update limits based on necessray conditions
        v0Vect.clear();
        v0Vect.resize(vellimits.size(), 0.0);
        v1Vect.clear();
        v1Vect.resize(vellimits.size(), 0.0);
        _UpdateLimitsByNecessaryConditions(vellimits, accellimits, x0VectIn, x1VectIn, v0Vect, v1Vect, usedBody);

        dReal fCurVelMult = 1.0;
        // Now setting fVelMultCutOff to 0 since when manip speed/accel limits are very low, we might get very small velmult
        // from SegmentFeasible2. It might not be a good idea to have a universal cutoff value for this multiplier.
        dReal fVelMultCutOff = 0; // stop trying if the ratio between the current vellimits and the original vellimits is less than this value
        int itry = 0;
        int numTries = 1000; // number of times allowed to scale down vellimits and accellimits
        RampOptimizer::CheckReturn retseg(0);
        std::vector<dReal> _temp(0);
        for (; itry < numTries; ++itry) {
            bool res = _interpolator.ComputeZeroVelNDTrajectory(x0VectIn, x1VectIn, vellimits, accellimits, rampndVectOut);
            if( !res ) {
                retseg.retcode = 0xffff|CFO_FromTrajectorySmoother;
                _sslog.str(""); _sslog.clear();
                _sslog << std::setprecision(std::numeric_limits<dReal>::digits10+1);
                _sslog << "x0=[";
                SerializeValues(_sslog, x0VectIn);
                _sslog << "]; x1=[";
                SerializeValues(_sslog, x1VectIn);
                _sslog << "]; curvellimits=[";
                SerializeValues(_sslog, vellimits);
                _sslog << "]; curaccellimits=[";
                SerializeValues(_sslog, accellimits);
                _sslog << "]";
                RAVELOG_WARN_FORMAT("env=%d, segment (%d, %d); numWaypoints=%d; ComputeZeroVelNDTrajectory failed. fCurVelMult=%f; itry=%d; %s", _environmentid%(iwaypoint - 1)%iwaypoint%numWaypoints%fCurVelMult%itry%_sslog.str());
                return false;
            }

            size_t irampnd = 0;
            rampndVectOut[0].GetX0Vect(x0Vect);
            rampndVectOut[0].GetV0Vect(v0Vect);
            FOREACHC(itrampnd, rampndVectOut) {
                itrampnd->GetX1Vect(x1Vect);
                itrampnd->GetV1Vect(v1Vect);

                retseg = SegmentFeasible2(x0Vect, x1Vect, v0Vect, v1Vect, itrampnd->GetDuration(), options, _cacheRampNDVectOut1, _temp);
                if( 0 ) {
                    // For debugging
                    _sslog.str(""); _sslog.clear();
                    _sslog << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                    _sslog << "x0=[";
                    SerializeValues(_sslog, x0Vect);
                    _sslog << "]; x1=[";
                    SerializeValues(_sslog, x1Vect);
                    _sslog << "]; v0=[";
                    SerializeValues(_sslog, v0Vect);
                    _sslog << "]; v1=[";
                    SerializeValues(_sslog, v1Vect);
                    _sslog << "];";
                    RAVELOG_WARN(_sslog.str());
                }

                if( retseg.retcode != 0 ) {
                    break;
                }
                if( retseg.bDifferentVelocity ) {
                    RAVELOG_WARN_FORMAT("env=%d, SegmentFeasible2 returns different final velocities", _environmentid);
                    retseg.retcode = CFO_FinalValuesNotReached;
                    break;
                }
                x0Vect.swap(x1Vect);
                v0Vect.swap(v1Vect);
            }
            if( retseg.retcode == 0 ) {
                break;
            }
            else if( retseg.retcode == CFO_CheckTimeBasedConstraints ) {
                RAVELOG_VERBOSE_FORMAT("env=%d, segment (%d, %d); numWaypoints=%d; scaling vellimits and accellimits by %.15e, itry=%d", _environmentid%(iwaypoint - 1)%iwaypoint%numWaypoints%retseg.fTimeBasedSurpassMult%itry);
                fCurVelMult *= retseg.fTimeBasedSurpassMult;
                if( fCurVelMult < fVelMultCutOff ) {
                    // The velocity multiplier falls below the cut off, so stop.
                    break;
                }
                RampOptimizer::ScaleVector(vellimits, retseg.fTimeBasedSurpassMult);
                RampOptimizer::ScaleVector(accellimits, retseg.fTimeBasedSurpassMult*retseg.fTimeBasedSurpassMult);
            }
            else {
                _sslog.str(""); _sslog.clear();
                _sslog << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
                _sslog << "x0=[";
                SerializeValues(_sslog, x0Vect);
                _sslog << "]; x1=[";
                SerializeValues(_sslog, x1Vect);
                _sslog << "]; v0=[";
                SerializeValues(_sslog, v0Vect);
                _sslog << "]; v1=[";
                SerializeValues(_sslog, v1Vect);
                _sslog << "]; curvellimits=[";
                SerializeValues(_sslog, vellimits);
                _sslog << "]; curaccellimits=[";
                SerializeValues(_sslog, accellimits);
                _sslog << "]; deltatime=" << (rampndVectOut[irampnd].GetDuration());
                RAVELOG_WARN_FORMAT("env=%d, segment (%d, %d); numWaypoints=%d; SegmentFeasibile2 returned error 0x%x; %s, giving up....", _environmentid%(iwaypoint - 1)%iwaypoint%numWaypoints%retseg.retcode%_sslog.str());
                return false;
            }
        }
        if( retseg.retcode != 0 ) {
            _sslog.str(""); _sslog.clear();
            _sslog << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
            _sslog << "x0=[";
            SerializeValues(_sslog, x0VectIn);
            _sslog << "]; x1=[";
            SerializeValues(_sslog, x1VectIn);
            _sslog << "]; curvellimits=[";
            SerializeValues(_sslog, vellimits);
            _sslog << "]; curaccellimits=[";
            SerializeValues(_sslog, accellimits);
            _sslog << "]";
            RAVELOG_WARN_FORMAT("env=%d, segment (%d, %d); numWaypoints=%d; ramp initialization failed. fCurVelMult=%f; itry=%d; retcode=0x%x; %s", _environmentid%(iwaypoint - 1)%iwaypoint%numWaypoints%fCurVelMult%itry%retseg.retcode%_sslog.str());
            return false;
        }
        return true;
    }

    /// \brief Figure out the direction of the acceleration of the given RampND (negative, zero, or
    /// positive). Assume that every DOF accelerates in the same direction.
    int _CheckRampNDAcceleration(RampOptimizer::RampND& rampnd)
    {
        dReal sum = 0;
        for( size_t idof = 0; idof < rampnd.GetDOF(); ++idof ) {
            sum += rampnd.GetAAt(idof);
        }
        if( sum < -RampOptimizer::g_fRampEpsilon ) {
            return -1;
        }
        else if( sum > RampOptimizer::g_fRampEpsilon ) {
            return 1;
        }
        else {
            return 0;
        }
    }

    /// \brief Merge consecutive trajectory segments. Here we try to remove each zeroVelPoint by
    /// merging the ramps before and after the zeroVelPoint. The content of this function is basically
    /// almost identical to _Shortcut except that instead of sampling two time instants t0, t1 at
    /// each iteration, we deterministically choose them to be time instants before and after a
    /// zeroVelPoint, respectively.
    int _MergeConsecutiveSegments(RampOptimizer::ParabolicPath& parabolicpath, dReal minTimeStep)
    {
        int nummerges = 0;
        if( _vZeroVelPointInfos.size() == 0 ) {
            return nummerges;
        }

        _DumpParabolicPath(parabolicpath, _dumplevel, 2);

#ifdef SMOOTHER2_PROGRESS_DEBUG
        std::vector<int>& vShortcutStats = _vShortcutStats; // vShortcutStats[SS_X] keeps the number of times a shortcut iter finishes with the status SS_X
        vShortcutStats.reserve(20);
        vShortcutStats.resize(20);
        std::fill(vShortcutStats.begin(), vShortcutStats.end(), 0);

        std::stringstream& shortcutprogress = _ssshortcutprogress;
        shortcutprogress.str(""); shortcutprogress.clear();
        shortcutprogress << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
#endif

        std::vector<RampOptimizer::RampND> rampndVect = parabolicpath.GetRampNDVect(); // for convenience

        // Caching stuff
        std::vector<RampOptimizer::RampND>& shortcutRampNDVect = _cacheRampNDVect; // for storing interpolated trajectory
        std::vector<RampOptimizer::RampND>& shortcutRampNDVectOut = _cacheRampNDVectOut, &shortcutRampNDVectOut1 = _cacheRampNDVectOut1; // for storing checked trajectory
        std::vector<dReal>& x0Vect = _cacheX0Vect, &x1Vect = _cacheX1Vect, &v0Vect = _cacheV0Vect, &v1Vect = _cacheV1Vect;
        std::vector<dReal>& tempX0Vect = _cacheTempX0Vect, &tempV0Vect = _cacheTempV0Vect;
        const dReal tOriginal = parabolicpath.GetDuration(); // the original trajectory duration before being shortcut
        dReal tTotal = tOriginal; // keeps track of the latest trajectory duration

        std::vector<dReal>& vellimits = _cacheVellimits, &accellimits = _cacheAccelLimits;

        // Various parameters for shortcutting
        int numSlowDowns = 0; // counts the number of times we slow down the trajectory (via vel/accel scaling) because of manip constraints
        int nTimeBasedConstraintsFailed = 0;

        std::vector<dReal> velReductionFactors, accelReductionFactors;
        velReductionFactors.resize(rampndVect.front().GetDOF());
        accelReductionFactors.resize(rampndVect.front().GetDOF());

#ifdef SMOOTHER2_PROGRESS_DEBUG
        uint32_t latestSuccessfulShortcutTimestamp = utils::GetMicroTime(), curtime;
#endif
        KinBodyConstPtr usedBody = _dynamicLimitInfo.bUseDynamicLimits ? GetEnv()->GetKinBody(_dynamicLimitInfo.bodyName) : KinBodyConstPtr();

        // Main shortcut loop
        size_t index;
        size_t iters = 0;
        size_t numIters = _vZeroVelPointInfos.size();
        for (index = 0; index < _vZeroVelPointInfos.size(); ++index, ++iters) { // _vZeroVelPointInfos.size() dynamically changes
            // Sample t0 and t1. We could possibly add some heuristics here to get higher quality
            // shortcuts
            dReal t0 = _vZeroVelPointInfos.at(index).leftneighbor;
            dReal t1 = _vZeroVelPointInfos.at(index).rightneighbor;

            // std::stringstream ss; ss << "index=" << index << ", zeroVelPoints=[";
            // FOREACHC(itval, _zeroVelPoints) {
            //  ss << *itval << ", ";
            // }
            // ss << "]; " << "zeroVelPointNeighbors=[";
            // FOREACHC(itval, _zeroVelPointNeighbors) {
            //  ss << "(" << itval->first << ", " << itval->second << "), ";
            // }
            // ss << "];";
            // RAVELOG_DEBUG_FORMAT("env=%d; %s", _environmentid%ss.str());

#ifdef SMOOTHER2_PROGRESS_DEBUG
            shortcutprogress << utils::GetMicroTime() << " " << tTotal << " " << t0 << " " << t1 << " ";
#endif

            uint32_t iIterProgress = 0; // used for debugging purposes

            // Perform shortcut
            try {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, start shortcutting with t0=%.15e; t1=%.15e", _environmentid%iters%numIters%t0%t1);
#endif
                int i0, i1;
                dReal u0, u1;
                parabolicpath.FindRampNDIndex(t0, i0, u0);
                parabolicpath.FindRampNDIndex(t1, i1, u1);

                rampndVect[i0].EvalPos(u0, x0Vect);
                if( _parameters->SetStateValues(x0Vect) != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                    ++vShortcutStats[SS_StateSettingFailed];
                    shortcutprogress << SS_StateSettingFailed << "\n";
#endif
                    continue;
                }
                iIterProgress +=  0x10000000;
                _parameters->_getstatefn(x0Vect);
                iIterProgress += 0x10000000;

                rampndVect[i1].EvalPos(u1, x1Vect);
                if( _parameters->SetStateValues(x1Vect) != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                    ++vShortcutStats[SS_StateSettingFailed];
                    shortcutprogress << SS_StateSettingFailed << "\n";
#endif
                    continue;
                }
                iIterProgress +=  0x10000000;
                _parameters->_getstatefn(x1Vect);

                rampndVect[i0].EvalVel(u0, v0Vect);
                rampndVect[i1].EvalVel(u1, v1Vect);
                ++_progress._iteration;

                vellimits = _parameters->_vConfigVelocityLimit;
                accellimits = _parameters->_vConfigAccelerationLimit;

                if( _bmanipconstraints && _manipconstraintchecker && _bUseNewHeuristic ) {
                    // pass
                    // do nothing only when the new heuristic is used while having manipconstraints. otherwise, proceed normally
                }
                else {
                    for (size_t j = 0; j < _parameters->_vConfigVelocityLimit.size(); ++j) {
                        // Adjust vellimits and accellimits
                        dReal fminvel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j])); // the scaled vellimits must be at least this value
                        {
                            dReal f = max(fminvel, _parameters->_vConfigVelocityLimit[j]);
                            if( vellimits[j] > f ) {
                                vellimits[j] = f;
                            }
                        }

                        {
                            dReal f = _parameters->_vConfigAccelerationLimit[j];
                            if( accellimits[j] > f ) {
                                accellimits[j] = f;
                            }
                        }
                    }
                }

                // update limits based on necessray conditions
                _UpdateLimitsByNecessaryConditions(vellimits, accellimits, x0Vect, x1Vect, v0Vect, v1Vect, usedBody);

                std::vector<dReal> reductionFactors2; // keeps track of the reduction factors got from this shortcut

                dReal fCurVelMult = 1.0;
                dReal fCurAccelMult = 1.0;

                bool bSuccess = false;
                size_t maxSlowDownTries = 100;
                std::fill(velReductionFactors.begin(), velReductionFactors.end(), 1); // Reset reductionfactors
                std::fill(accelReductionFactors.begin(), accelReductionFactors.end(), 1); // Reset reductionfactors
                size_t iSlowDownDueToManip = 0;
                bool bShortcutTimeExceeded = false;
                for (size_t iSlowDown = 0; iSlowDown < maxSlowDownTries; ++iSlowDown) {
                    // compute interpolation. only at the first slowdown iteration, call dedicated function to combine interpolation and computing of initial guess of vellimits and accellimits
                    dReal segmentTime = 0;
                    if( iSlowDown == 0 ) {
                        bool bSlowedDown;
                        if( !_ComputeInterpolationWithInitialGuess(shortcutRampNDVect, segmentTime, vellimits, accellimits, bSlowedDown, fCurVelMult, fCurAccelMult, iIterProgress,
                                                                   t0, t1, minTimeStep, x0Vect, x1Vect, v0Vect, v1Vect, iSlowDown, iters, numIters, usedBody) ) {
                            break;
                        }
                        if( bSlowedDown ) {
                            numSlowDowns += 1;
                            nTimeBasedConstraintsFailed++;
                        }
                    }
                    else {
                        if( !_ComputeInterpolation(shortcutRampNDVect, segmentTime, iIterProgress,
                                                   t0, t1, minTimeStep, x0Vect, x1Vect, v0Vect, v1Vect, vellimits, accellimits, iSlowDown, iters, numIters) ) {
                            break;
                        }
                    }

                    if( _CallCallbacks(_progress) == PA_Interrupt ) {
                        return -1;
                    }
                    if (_parameters->_nMaxPlanningTime > 0) {
                        uint32_t elapsedtime = utils::GetMilliTime() - _basetime;
                        if( elapsedtime >= _parameters->_nMaxPlanningTime ) {
                            bShortcutTimeExceeded = true;
                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut time exceeded (%dms) so breaking. iter=%d < %d", _environmentid%elapsedtime%iters%numIters);
                            break;
                        }
                    }
                    iIterProgress += 0x1000;

                    RampOptimizer::CheckReturn retcheck(0);
                    iIterProgress += 0x10;

                    do { // Start checking constraints.
                        if( _parameters->SetStateValues(x1Vect) != 0 ) {
                            std::stringstream s;
                            s << std::setprecision(RampOptimizer::g_nPrec) << "x1 = [";
                            SerializeValues(s, x1Vect);
                            s << "];";
                            RAVELOG_VERBOSE_FORMAT("env=%d, shortcut iter=%d/%d, cannot set state: %s", _environmentid%iters%numIters%s.str());
                            retcheck.retcode = CFO_StateSettingError;
#ifdef SMOOTHER2_PROGRESS_DEBUG
                            ++vShortcutStats[SS_StateSettingFailed];
                            shortcutprogress << SS_StateSettingFailed << "\n";
#endif
                            break;
                        }
                        _parameters->_getstatefn(x1Vect);
                        iIterProgress += 0x10;

                        retcheck = _feasibilitychecker.Check2(shortcutRampNDVect, 0xffff|CFO_FromTrajectorySmoother, shortcutRampNDVectOut);
#ifdef SMOOTHER2_TIMING_DEBUG
                        _nCallsCheckPathAllConstraints += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                        _totalTimeCheckPathAllConstraints += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                        if( retcheck.retcode != 0 ) {
                            _nCallsCheckPathAllConstraintsInVain += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                            _totalTimeCheckPathAllConstraintsInVain += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                        }
                        // Reset SegmentFeasible2 counters
                        _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                        _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif

                        iIterProgress += 0x10;

                        if( retcheck.retcode != 0 ) {
                            // Shortcut does not pass CheckPathAllConstraints
#ifdef SMOOTHER2_PROGRESS_DEBUG
                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, iSlowDown=%d, shortcut does not pass Check2, retcode=0x%x.\n", _environmentid%iters%numIters%iSlowDown%retcheck.retcode);
                            if( retcheck.retcode == 1 ) {
                                ++vShortcutStats[SS_Check2CollisionFailed];
                                shortcutprogress << SS_Check2CollisionFailed << "\n";
                            }
                            else if( retcheck.retcode != CFO_CheckTimeBasedConstraints ) {
                                ++vShortcutStats[SS_Check2Failed];
                                shortcutprogress << SS_Check2Failed << "\n";
                            }
#endif
                            break;
                        }

                        // CheckPathAllConstraints (called viaSegmentFeasible2 inside Check2) may be
                        // modifying the original shortcutcurvesnd due to constraints. Therefore, we
                        // have to reset vellimits and accellimits so that they are above those of
                        // the modified trajectory.
                        for (size_t irampnd = 0; irampnd < shortcutRampNDVectOut.size(); ++irampnd) {
                            for (size_t jdof = 0; jdof < shortcutRampNDVectOut[irampnd].GetDOF(); ++jdof) {
                                dReal fminvel = max(RaveFabs(shortcutRampNDVectOut[irampnd].GetV0At(jdof)), RaveFabs(shortcutRampNDVectOut[irampnd].GetV1At(jdof)));
                                if( vellimits[jdof] < fminvel ) {
                                    vellimits[jdof] = fminvel;
                                }
                            }
                        }

                        // The interpolated segment passes constraints checking. Now see if it is modified such that it does not end with the desired velocity.
                        if( retcheck.bDifferentVelocity && shortcutRampNDVectOut.size() > 0 ) {
                            RAVELOG_VERBOSE_FORMAT("env=%d, new shortcut is *not* aligned with boundary values after running Check2. Start fixing the last segment.", _environmentid);
                            // Modification inside Check2 results in the shortcut trajectory not ending at the desired velocity v1.
                            dReal allowedStretchTime = (t1 - t0) - (segmentTime + minTimeStep); // the time that this segment is allowed to stretch out such that it is still a useful shortcut

                            shortcutRampNDVectOut.back().GetX0Vect(tempX0Vect);
                            shortcutRampNDVectOut.back().GetV0Vect(tempV0Vect);
#ifdef SMOOTHER2_TIMING_DEBUG
                            _nCallsInterpolator += 1;
                            _tStartInterpolator = utils::GetMicroTime();
#endif
                            bool res2 = _interpolator.ComputeArbitraryVelNDTrajectory(tempX0Vect, x1Vect, tempV0Vect, v1Vect, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, vellimits, accellimits, shortcutRampNDVect, true);
#ifdef SMOOTHER2_TIMING_DEBUG
                            _tEndInterpolator = utils::GetMicroTime();
                            _totalTimeInterpolator += 0.000001f*(float)(_tEndInterpolator - _tStartInterpolator);
#endif
                            if( !res2 ) {
                                // This may be because we cannot fix joint limit violation
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, failed to InterpolateArbitraryVelND to correct the final velocity", _environmentid);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                retcheck.retcode = CFO_FinalValuesNotReached;
                                break;
                            }

                            dReal lastSegmentTime = 0;
                            FOREACHC(itrampnd, shortcutRampNDVect) {
                                lastSegmentTime += itrampnd->GetDuration();
                            }
                            if( lastSegmentTime - shortcutRampNDVectOut.back().GetDuration() > allowedStretchTime ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, the modified last segment duration is too long to be useful(%.15e s.)", _environmentid%iters%numIters%lastSegmentTime);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                retcheck.retcode = CFO_FinalValuesNotReached;
                                break;
                            }

                            retcheck = _feasibilitychecker.Check2(shortcutRampNDVect, 0xffff|CFO_FromTrajectorySmoother, shortcutRampNDVectOut1);
#ifdef SMOOTHER2_TIMING_DEBUG
                            _nCallsCheckPathAllConstraints += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                            _totalTimeCheckPathAllConstraints += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                            if( retcheck.retcode != 0 ) {
                                _nCallsCheckPathAllConstraintsInVain += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                                _totalTimeCheckPathAllConstraintsInVain += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                            }
                            // Reset SegmentFeasible2 counters
                            _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                            _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif

                            if( retcheck.retcode != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, final segment fixing failed. retcode=0x%x", _environmentid%retcheck.retcode);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                break;
                            }
                            else if( retcheck.bDifferentVelocity ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, after final segment fixing, shortcutRampND still does not end at the desired velocity", _environmentid);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                retcheck.retcode = CFO_FinalValuesNotReached;
                                break;
                            }
                            else {
                                // Otherwise, this segment is good.
                                RAVELOG_VERBOSE_FORMAT("env=%d, final velocity correction for the last segment successful", _environmentid);
                                shortcutRampNDVectOut.pop_back();
                                shortcutRampNDVectOut.insert(shortcutRampNDVectOut.end(), shortcutRampNDVectOut1.begin(), shortcutRampNDVectOut1.end());

                                // Check consistency
                                if( IS_DEBUGLEVEL(Level_Verbose) ) {
                                    shortcutRampNDVectOut.front().GetX0Vect(x0Vect);
                                    shortcutRampNDVectOut.back().GetX1Vect(x1Vect);
                                    shortcutRampNDVectOut.front().GetV0Vect(v0Vect);
                                    shortcutRampNDVectOut.back().GetV1Vect(v1Vect);
                                    RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampNDs(shortcutRampNDVectOut, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit, x0Vect, x1Vect, v0Vect, v1Vect);
                                    OPENRAVE_ASSERT_OP(parabolicret, ==, RampOptimizer::PCR_Normal);
                                }
                            }
                        }
                        else {
                            RAVELOG_VERBOSE_FORMAT("env=%d, new shortcut is aligned with boundary values after running Check2", _environmentid);
                            break;
                        }
                    } while (0);
                    // Finished checking constraints. Now see what retcheck.retcode is
                    iIterProgress += 0x1000;

                    if( retcheck.retcode == 0 ) {
                        // Shortcut is successful.
                        bSuccess = true;
                        break;
                    }
                    else if( retcheck.retcode == CFO_CheckTimeBasedConstraints ) {
                        // CFO_CheckTimeBasedConstraints can be caused by the following
                        // - torque limit violation
                        // - manipulator speed/accel constraint violation
                        // - joint velocity adjustment done in SegmentFeasible2
                        nTimeBasedConstraintsFailed++;

                        // Scale down vellimits and/or accellimits based on which constraints are violated.
                        // In case manip speed is exceeded, only vellimits is scaled down. Otherwise, both vellimits and accellimits are scaled down.
                        if( _bmanipconstraints && _manipconstraintchecker ) {
                            // Scale down vellimits and accellimits independently according to the violated constraint (manipspeed/manipaccel)
                            {
                                // After computing the new vellimits and accellimits and they don't work, we gradually scale vellimits/accellimits down
                                dReal fVelMult, fAccelMult;
                                bool maxManipSpeedViolated = false, maxManipAccelViolated = false;
                                if( retcheck.fMaxManipAccel > _parameters->maxmanipaccel ) {
                                    ++iSlowDownDueToManip;
                                    // Manipaccel is violated. We scale both vellimits and accellimits down.
                                    maxManipAccelViolated = true;
                                    if( _bUseNewHeuristic && retcheck.vReductionFactors.size() > 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                        std::stringstream ss; ss << "env=" << _environmentid << ", maxManipAccelViolated=1 (";
                                        ss << retcheck.fMaxManipAccel << " > " << _parameters->maxmanipaccel << "); reductionFactors=[";
                                        FOREACHC(itval, retcheck.vReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; velReductionFactors=[";
                                        FOREACHC(itval, velReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; accelReductionFactors=[";
                                        FOREACHC(itval, accelReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "];";
                                        RAVELOG_DEBUG(ss.str());
#endif
                                        for( size_t j = 0; j < vellimits.size(); ++j ) {
                                            vellimits[j] *= RaveSqrt(retcheck.vReductionFactors[j]);
                                            accellimits[j] *= retcheck.vReductionFactors[j];
                                            velReductionFactors[j] *= RaveSqrt(retcheck.vReductionFactors[j]);
                                            accelReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        }
                                    }
                                    else {
                                        fAccelMult = retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                                        fCurAccelMult *= fAccelMult;
                                        if( fCurAccelMult < 0.0001 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: maxmanipaccel violated but fCurAccelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurAccelMult);
                                            ++vShortcutStats[SS_MaxManipAccelFailed];
                                            shortcutprogress << SS_MaxManipAccelFailed << "\n";
#endif
                                            break;
                                        }
                                        {
                                            fVelMult = retcheck.fTimeBasedSurpassMult; // larger scaling factor, less reduction. Use a square root here since the velocity has the factor t while the acceleration has t^2
                                            fCurVelMult *= fVelMult;
                                            if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: maxmanipaccel violated but fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                                ++vShortcutStats[SS_MaxManipAccelFailed];
                                                shortcutprogress << SS_MaxManipAccelFailed << "\n";
#endif
                                                break;
                                            }
                                            for (size_t j = 0; j < vellimits.size(); ++j) {
                                                dReal fMinVel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                                vellimits[j] = max(fMinVel, fVelMult * vellimits[j]);
                                            }
                                        }
                                        for (size_t j = 0; j < accellimits.size(); ++j) {
                                            accellimits[j] *= fAccelMult;
                                        }
                                    }
                                }
                                else if( retcheck.fMaxManipSpeed > _parameters->maxmanipspeed ) {
                                    ++iSlowDownDueToManip;
                                    // Manipspeed is violated. We don't scale down accellimits.
                                    maxManipSpeedViolated = true;
                                    if( _bUseNewHeuristic && retcheck.vReductionFactors.size() > 0 && !(retcheck.fMaxManipAccel > _parameters->maxmanipaccel)) {
                                        // do vel scaling without accel scaling only when accel limit is not violated
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                        std::stringstream ss; ss << "env=" << _environmentid << ", maxManipSpeedViolated=1 (";
                                        ss << retcheck.fMaxManipSpeed << " > " << _parameters->maxmanipspeed << "); reductionFactors=[";
                                        FOREACHC(itval, retcheck.vReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; velReductionFactors=[";
                                        FOREACHC(itval, velReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; accelReductionFactors=[";
                                        FOREACHC(itval, accelReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "];";
                                        RAVELOG_DEBUG(ss.str());
#endif
                                        for( size_t j = 0; j < vellimits.size(); ++j ) {
                                            vellimits[j] *= retcheck.vReductionFactors[j];
                                            velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        }
                                    }
                                    else {
                                        fVelMult = retcheck.fTimeBasedSurpassMult;
                                        fCurVelMult *= fVelMult;
                                        if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: maxmanipspeed violated but fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                            ++vShortcutStats[SS_MaxManipSpeedFailed];
                                            shortcutprogress << SS_MaxManipSpeedFailed << "\n";

#endif
                                            break;
                                        }
                                        for (size_t j = 0; j < vellimits.size(); ++j) {
                                            dReal fMinVel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            vellimits[j] = max(fMinVel, fVelMult * vellimits[j]);
                                        }
                                    }
                                }

                                if( !maxManipSpeedViolated && !maxManipAccelViolated ) {
                                    // Got a failure due to time-based constraints but manip speed/accel are not
                                    // violated. This causes by velocity limits violation from ramps that have been
                                    // modified by CheckPathAllConstraints.
                                    if( _bUseNewHeuristic && retcheck.vReductionFactors.size() > 0 ) {
                                        // do vel scaling without accel scaling
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                        std::stringstream ss; ss << "env=" << _environmentid << ", reductionFactors=[";
                                        FOREACHC(itval, retcheck.vReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; velReductionFactors=[";
                                        FOREACHC(itval, velReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; accelReductionFactors=[";
                                        FOREACHC(itval, accelReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "];";
                                        RAVELOG_DEBUG(ss.str());
#endif
                                        for( size_t j = 0; j < vellimits.size(); ++j ) {
                                            dReal fMinVelLimit = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            if( vellimits[j] * retcheck.vReductionFactors[j] < fMinVelLimit ) {
                                                // In this case, we cannot use the recommended scaling factor since
                                                // after scaling, the vellimits will fall below max(v0, v1). So we set
                                                // vellimits to be max(v0, v1) instead.
                                                velReductionFactors[j] *= (fMinVelLimit / vellimits[j]);
                                                vellimits[j] = fMinVelLimit + RampOptimizer::g_fRampEpsilon;
                                            }
                                            else {
                                                vellimits[j] *= retcheck.vReductionFactors[j];
                                                velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                            }
                                            // vellimits[j] *= retcheck.vReductionFactors[j];
                                            // velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        }
                                    }
                                    else {
                                        fVelMult = retcheck.fTimeBasedSurpassMult;
                                        fAccelMult = retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                                        fCurVelMult *= fVelMult;
                                        fCurAccelMult *= fAccelMult;
                                        if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: modified ramps exceed vellimits/accellimits but fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                            ++vShortcutStats[SS_Check2Failed];
                                            shortcutprogress << SS_Check2Failed << "\n";
#endif
                                            break;
                                        }
                                        if( fCurAccelMult < 0.0001 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: modified ramps exceed vellimits/accellimits but fCurAccelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurAccelMult);
                                            ++vShortcutStats[SS_Check2Failed];
                                            shortcutprogress << SS_Check2Failed << "\n";
#endif
                                            break;
                                        }
                                        for (size_t j = 0; j < vellimits.size(); ++j) {
                                            dReal fMinVel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            vellimits[j] = max(fMinVel, fVelMult * vellimits[j]);
                                            accellimits[j] *= fAccelMult;
                                        }
                                    }
                                }
                                numSlowDowns += 1;
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, maxManipSpeedViolated=%d, maxManipAccelViolated=%d, fTimeBasedSurpassMult=%.15e; fCurVelMult=%.15e; fCurAccelMult=%.15e, numSlowDowns=%d", _environmentid%maxManipSpeedViolated%maxManipAccelViolated%retcheck.fTimeBasedSurpassMult%fCurVelMult%fCurAccelMult%numSlowDowns);
#endif
                            }
                        }
                        else {
                            // Scale down vellimits and accellimits using the normal procedure
                            fCurVelMult *= retcheck.fTimeBasedSurpassMult;
                            fCurAccelMult *= retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                            if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                ++vShortcutStats[SS_SlowDownFailed];
                                shortcutprogress << SS_SlowDownFailed << "\n";
#endif
                                break;
                            }
                            if( fCurAccelMult < 0.0001 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: fCurAccelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurAccelMult);
                                ++vShortcutStats[SS_SlowDownFailed];
                                shortcutprogress << SS_SlowDownFailed << "\n";
#endif
                                break;
                            }

                            numSlowDowns += 1;
                            for (size_t j = 0; j < vellimits.size(); ++j) {
                                dReal fMinVel =  max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                vellimits[j] = max(fMinVel, retcheck.fTimeBasedSurpassMult * vellimits[j]);
                                accellimits[j] *= retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                            }
                        }
                    }
                    else {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                        RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, rejecting shortcut due to constraint 0x%x", _environmentid%iters%numIters%retcheck.retcode);
#endif
                        break;
                    }
                    iIterProgress += 0x1000;
                } // Finished slowing down the shortcut
                if (bShortcutTimeExceeded) {
                    break;
                }

                if( !bSuccess ) {
                    // Shortcut failed. Continue to the next iteration.
                    continue;
                }

                if( shortcutRampNDVectOut.size() == 0 ) {
                    RAVELOG_WARN_FORMAT("env=%d, shortcutpath is empty!", _environmentid);
                    continue;
                }

                // Now this shortcut is really successful
                ++nummerges;
#ifdef SMOOTHER2_PROGRESS_DEBUG
                ++vShortcutStats[SS_Successful];
                shortcutprogress << SS_Successful << "\n";
                latestSuccessfulShortcutTimestamp = utils::GetMicroTime();
#endif

                nTimeBasedConstraintsFailed = 0; // reset

                // Keep track of zero-velocity waypoints
                dReal segmentTime = 0;
                FOREACHC(itrampnd, shortcutRampNDVectOut) {
                    segmentTime += itrampnd->GetDuration();
                }
                dReal diff = (t1 - t0) - segmentTime;

                size_t writeIndex = 0;
                for( size_t readIndex = 0; readIndex < _vZeroVelPointInfos.size(); ++readIndex ) {
                    if( _vZeroVelPointInfos[readIndex].point <= t0 ) {
                        writeIndex += 1;
                    }
                    else if( _vZeroVelPointInfos[readIndex].point <= t1 ) {
                        // Do nothing
                    }
                    else {
                        // Update all zero-velocity points after t1
                        _vZeroVelPointInfos[writeIndex] = _vZeroVelPointInfos[readIndex];
                        _vZeroVelPointInfos[writeIndex].point -= diff;
                        _vZeroVelPointInfos[writeIndex].leftneighbor -= diff;
                        _vZeroVelPointInfos[writeIndex].rightneighbor -= diff;
                        ++writeIndex;
                    }
                }
                _vZeroVelPointInfos.resize(writeIndex);
                --index;

                // Now replace the original trajectory segment by the shortcut
                parabolicpath.ReplaceSegment(t0, t1, shortcutRampNDVectOut);
                iIterProgress += 0x10000000;

                rampndVect = parabolicpath.GetRampNDVect();

                // Check consistency
                if( IS_DEBUGLEVEL(Level_Verbose) ) {
                    rampndVect.front().GetX0Vect(x0Vect);
                    rampndVect.back().GetX1Vect(x1Vect);
                    rampndVect.front().GetV0Vect(v0Vect);
                    rampndVect.back().GetV1Vect(v1Vect);
                    RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampNDs(rampndVect, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit, x0Vect, x1Vect, v0Vect, v1Vect);
                    OPENRAVE_ASSERT_OP(parabolicret, ==, RampOptimizer::PCR_Normal);
                }
                iIterProgress += 0x10000000;

                tTotal = parabolicpath.GetDuration();
                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d successful, numSlowDowns=%d, tTotal=%.15e", _environmentid%iters%numIters%numSlowDowns%tTotal);
            }
            catch (const std::exception& ex) {
                RAVELOG_WARN_FORMAT("env=%d, An exception happened during shortcut iteration progress = 0x%x: %s", _environmentid%iIterProgress%ex.what());
            }
        }

        // Report status
        RAVELOG_DEBUG_FORMAT("env=%d, merging ramps finished, successful=%d, slowdowns=%d, endTime: %.15e -> %.15e; diff = %.15e", _environmentid%nummerges%numSlowDowns%tOriginal%tTotal%(tOriginal - tTotal));
        _DumpParabolicPath(parabolicpath, _dumplevel, 3);
#ifdef SMOOTHER2_PROGRESS_DEBUG
        curtime = utils::GetMicroTime();
        RAVELOG_DEBUG_FORMAT("env=%d, shortcut stats:\n  successful=%d\n  initialInterpolationFailed=%d\n  interpolatedSegmentTooLong=%d\n  interpolatedSegmentTooLongFromSlowDown=%d\n  timeInstantsTooClose=%d\n  check2CollisionFailed=%d\n  check2Failed=%d\n  lastSegmentFailed=%d\n  maxManipSpeedFailed=%d\n  maxManipAccelFailed=%d\n  slowDownFailed=%d\n  stateSettingFailed=%d\n  redundantShortcut=%d\n  _vZeroVelPointInfos.size()=%d\n  time since last successful shortcut=%.15e\n  final duration ratio=%.15e", _environmentid%vShortcutStats[SS_Successful]%vShortcutStats[SS_InitialInterpolationFailed]%vShortcutStats[SS_InterpolatedSegmentTooLong]%vShortcutStats[SS_InterpolatedSegmentTooLongFromSlowDown]%vShortcutStats[SS_TimeInstantsTooClose]%vShortcutStats[SS_Check2CollisionFailed]%vShortcutStats[SS_Check2Failed]%vShortcutStats[SS_LastSegmentFailed]%vShortcutStats[SS_MaxManipSpeedFailed]%vShortcutStats[SS_MaxManipAccelFailed]%vShortcutStats[SS_SlowDownFailed]%vShortcutStats[SS_StateSettingFailed]%vShortcutStats[SS_RedundantShortcut]%_vZeroVelPointInfos.size()%(0.000001f*(float)(curtime - latestSuccessfulShortcutTimestamp))%(tTotal/tOriginal));
        {
            std::string shortcutprogressfilename = str(boost::format("%s/shortcutprogress%d.xml")%RaveGetHomeDirectory()%_fileIndex);
            std::ofstream f(shortcutprogressfilename.c_str());
            f << shortcutprogress.str();
            RAVELOG_DEBUG_FORMAT("env=%d, shortcutprogress saved to %s", _environmentid%shortcutprogressfilename);
        }
#endif

        return nummerges;
    }

    /// \brief Return the number of successful shortcut.
    int _Shortcut(RampOptimizer::ParabolicPath& parabolicpath, int numIters, RampOptimizer::RandomNumberGeneratorBase* rng, dReal minTimeStep)
    {
        int numShortcuts = 0;
        _DumpParabolicPath(parabolicpath, _dumplevel, 0);

#ifdef SMOOTHER2_PROGRESS_DEBUG
        std::vector<int>& vShortcutStats = _vShortcutStats; // vShortcutStats[SS_X] keeps the number of times a shortcut iter finishes with the status SS_X
        vShortcutStats.reserve(20);
        vShortcutStats.resize(20);
        std::fill(vShortcutStats.begin(), vShortcutStats.end(), 0);

        std::stringstream& shortcutprogress = _ssshortcutprogress;
        shortcutprogress.str(""); shortcutprogress.clear();
        shortcutprogress << std::setprecision(std::numeric_limits<dReal>::digits10 + 1);
#endif

        std::vector<RampOptimizer::RampND> rampndVect = parabolicpath.GetRampNDVect(); // for convenience

        // Caching stuff
        std::vector<RampOptimizer::RampND>& shortcutRampNDVect = _cacheRampNDVect; // for storing interpolated trajectory
        std::vector<RampOptimizer::RampND>& shortcutRampNDVectOut = _cacheRampNDVectOut, &shortcutRampNDVectOut1 = _cacheRampNDVectOut1; // for storing checked trajectory
        std::vector<dReal>& x0Vect = _cacheX0Vect, &x1Vect = _cacheX1Vect, &v0Vect = _cacheV0Vect, &v1Vect = _cacheV1Vect;
        std::vector<dReal>& tempX0Vect = _cacheTempX0Vect, &tempV0Vect = _cacheTempV0Vect;
        const dReal tOriginal = parabolicpath.GetDuration(); // the original trajectory duration before being shortcut
        dReal tTotal = tOriginal; // keeps track of the latest trajectory duration

        std::vector<dReal>& vellimits = _cacheVellimits, &accellimits = _cacheAccelLimits;

        // Various parameters for shortcutting
        int numSlowDowns = 0; // counts the number of times we slow down the trajectory (via vel/accel scaling) because of manip constraints
        dReal fStartTimeVelMult = 1.0; // this is the multiplier for scaling down the *initial* velocity in each shortcut iteration. If manip constraints
                                       // or dynamic constraints are used, then this will track the most recent successful multiplier. The idea is that if the
                                       // recent successful multiplier is some low value, say 0.1, it is unlikely that using the full vel/accel limits, i.e.,
                                       // multiplier = 1.0, will succeed the next time
        dReal fStartTimeAccelMult = 1.0;

        std::vector<dReal> velReductionFactors, accelReductionFactors;
        velReductionFactors.resize(rampndVect.front().GetDOF());
        accelReductionFactors.resize(rampndVect.front().GetDOF());

        // Parameters & variables for early shortcut termination
        size_t nItersFromPrevSuccessful = 0;        // keeps track of the most recent successful shortcut iteration
        size_t nCutoffIters = std::max(_parameters->nshortcutcycles, min(100, numIters/2)); // we stop shortcutting if no progress has been made in the past nCutoffIters iterations
        size_t nTimeBasedConstraintsFailed = 0;     // the number of times that time-based constraints fail between two consecutive successful shortcuts (reset
        // every time a shortcut attempt is successful)

        dReal score = 1.0;                 // if the current iteration is successful, we calculate a score
        dReal currentBestScore = 0.0;      // keeps track of the best shortcut score so far
        dReal iCurrentBestScore = DBL_MAX;
        dReal cutoffRatio = _parameters->durationImprovementCutoffRatio; // we stop shortcutting if the progress made is considered too little (score/currentBestScore < cutoffRatio)

        dReal specialShortcutWeight = 0.1; // if the sampled number is less than this weight, we sample t0 and t1 around a zerovelpoint
                                           // (instead of randomly sample in the whole range) to try to shortcut and remove it.
        dReal specialShortcutCutoffTime = 0.75; // when doind special shortcut, we sample one of the remaining zero-velocity waypoints. Then we try to
                                                // shortcut in the range twaypoint +/- specialShortcutCutoffTime

        dReal fiMinDiscretization = 4.0/(minTimeStep); // mindiscretization is basically the step length to discretize the current trajectory so as to record if
                                                       // the two sampled time instances fall into the same two bins. If so, skip the rest of computation.
        std::vector<uint8_t>& vVisitedDiscretization = _vVisitedDiscretizationCache;
        vVisitedDiscretization.clear();
        int nEndTimeDiscretization = 0;

#ifdef SMOOTHER2_PROGRESS_DEBUG
        uint32_t latestSuccessfulShortcutTimestamp = utils::GetMicroTime(), curtime;
#endif
        KinBodyConstPtr usedBody = _dynamicLimitInfo.bUseDynamicLimits ? GetEnv()->GetKinBody(_dynamicLimitInfo.bodyName) : KinBodyConstPtr();

        // Main shortcut loop
        int iters = 0;
        for (iters = 0; iters < numIters; ++iters) {
            if( tTotal < minTimeStep ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, tTotal=%.15e is too short to continue shortcutting", _environmentid%iters%numIters%tTotal);
#endif
                break;
            }

            if( nItersFromPrevSuccessful + nTimeBasedConstraintsFailed > nCutoffIters  ) {
                // There has been no progress in the last nCutoffIters iterations. Stop right away.
                break;
            }
            nItersFromPrevSuccessful += 1;
            fStartTimeVelMult = 1;
            fStartTimeAccelMult = 1;

            // Sample t0 and t1. We could possibly add some heuristics here to get higher quality
            // shortcuts
            dReal t0, t1;
            if( iters == 0 ) {
                t0 = 0;
                t1 = tTotal;
            }
            else if( (_vZeroVelPointInfos.size() > 0 && rng->Rand() <= specialShortcutWeight) || (numIters - iters <= (int)_vZeroVelPointInfos.size()) ) {
                /* We consider shortcutting around a zerovelpoint (the time instant of an original
                   waypoint which has not yet been shortcut) when there are some zerovelpoints left
                   and either
                   - the random number falls below the threshold, or
                   - there are not so many shortcut iterations left (compared to the number of zerovelpoints)
                 */
                size_t index = _uniformsampler->SampleSequenceOneUInt32()%_vZeroVelPointInfos.size();
                const dReal tCenter = _vZeroVelPointInfos[index].point;
                _SampleTimeAroundCenter(t0, t1,
                                        rng->Rand(), rng->Rand(), tTotal, minTimeStep, tCenter, specialShortcutCutoffTime);

                if( numIters - iters <= (int)_vZeroVelPointInfos.size() ) {
                    // By the time we reach here, it is likely that these multipliers have been
                    // scaled down to be very small. Try resetting it in hopes that it helps produce
                    // some successful shortcuts.
                    fStartTimeVelMult = max(0.8, fStartTimeVelMult);
                    fStartTimeAccelMult = max(0.8, fStartTimeAccelMult);
                }
            }
            else {
                // Proceed normally
                _SampleTime(t0, t1,
                            rng->Rand(), rng->Rand(), tTotal, minTimeStep);
                // 2019/04/26: Might be too constrained to only allow time instants that are not further apart than the largest ramp time. _maxInitialRampTime could be small due to various reasons. In such cases, shortcut performance will be poor.
                // if( t1 - t0 > 2*_maxInitialRampTime ) {
                //     t1 = t0 + 2*_maxInitialRampTime;
                // }
            }

#ifdef SMOOTHER2_PROGRESS_DEBUG
            shortcutprogress << utils::GetMicroTime() << " " << tTotal << " " << t0 << " " << t1 << " ";
#endif

#ifdef SMOOTHER2_DISABLE_VVISITEDDISCRETIZATION
#else
            {
                if( vVisitedDiscretization.size() == 0 ) {
                    nEndTimeDiscretization = (int)(tTotal*fiMinDiscretization)+1;

                    // if nEndTimeDiscretization is too big, then just ignore vVisitedDiscretization
                    if( nEndTimeDiscretization <= 0x1000 ) {
                        vVisitedDiscretization.resize(nEndTimeDiscretization*nEndTimeDiscretization,0);
                    }
                }

                // Keep track of time slots that have already been previously checked (and failed)
                int t0Index = t0*fiMinDiscretization;
                int t1Index = t1*fiMinDiscretization;
                size_t testPairIndex = t0Index*nEndTimeDiscretization + t1Index;
                if( testPairIndex < vVisitedDiscretization.size() ) {
                    if( vVisitedDiscretization[testPairIndex] ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                        RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: the sampled t0=%.15e and t1=%.15e have been tested", _environmentid%iters%numIters%t0%t1);
                        ++vShortcutStats[SS_RedundantShortcut];
                        shortcutprogress << SS_RedundantShortcut << "\n";
#endif
                        continue;
                    }
                }

                if( 0 ) {//( _bmanipconstraints && _manipconstraintchecker ) {
                    // In case there are manipconstraints, we also mark neighbor pairs of timeindices as checked
                    for( int t0TestIndex = t0Index - 1; t0TestIndex < t0Index + 2; ++t0TestIndex ) {
                        for( int t1TestIndex = t1Index - 1; t1TestIndex < t1Index + 2; ++t1TestIndex ) {
                            if( t0TestIndex >=0 && t1TestIndex >= 0 && t0TestIndex < nEndTimeDiscretization && t1TestIndex < nEndTimeDiscretization ) {
                                testPairIndex = t0TestIndex*nEndTimeDiscretization + t1TestIndex;
                                if( testPairIndex < vVisitedDiscretization.size() ) {
                                    vVisitedDiscretization[testPairIndex] = 1;
                                }
                            }
                        }
                    }
                }
                else {
                    if( testPairIndex < vVisitedDiscretization.size() ) {
                        vVisitedDiscretization[testPairIndex] = 1;
                    }
                }
            }
#endif

            uint32_t iIterProgress = 0; // used for debugging purposes

            // Perform shortcut
            try {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, start shortcutting with t0=%.15e; t1=%.15e; fStartTimeVelMult=%.15e; fStartTimeAccelMult=%.15e", _environmentid%iters%numIters%t0%t1%fStartTimeVelMult%fStartTimeAccelMult);
#endif
                int i0, i1;
                dReal u0, u1;
                parabolicpath.FindRampNDIndex(t0, i0, u0);
                parabolicpath.FindRampNDIndex(t1, i1, u1);

                rampndVect[i0].EvalPos(u0, x0Vect);
                if( _parameters->SetStateValues(x0Vect) != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                    ++vShortcutStats[SS_StateSettingFailed];
                    shortcutprogress << SS_StateSettingFailed << "\n";
#endif
                    continue;
                }
                iIterProgress +=  0x10000000;
                _parameters->_getstatefn(x0Vect);
                iIterProgress += 0x10000000;

                rampndVect[i1].EvalPos(u1, x1Vect);
                if( _parameters->SetStateValues(x1Vect) != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                    ++vShortcutStats[SS_StateSettingFailed];
                    shortcutprogress << SS_StateSettingFailed << "\n";
#endif
                    continue;
                }
                iIterProgress +=  0x10000000;
                _parameters->_getstatefn(x1Vect);

                rampndVect[i0].EvalVel(u0, v0Vect);
                rampndVect[i1].EvalVel(u1, v1Vect);
                ++_progress._iteration;

                vellimits = _parameters->_vConfigVelocityLimit;
                accellimits = _parameters->_vConfigAccelerationLimit;

                if( _bmanipconstraints && _manipconstraintchecker && _bUseNewHeuristic ) {
                    // pass
                    // do nothing only when the new heuristic is used while having manipconstraints. otherwise, proceed normally
                }
                else {
                    for (size_t j = 0; j < _parameters->_vConfigVelocityLimit.size(); ++j) {
                        // Adjust vellimits and accellimits
                        dReal fminvel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j])); // the scaled vellimits must be at least this value
                        {
                            dReal f = max(fminvel, fStartTimeVelMult * _parameters->_vConfigVelocityLimit[j]);
                            if( vellimits[j] > f ) {
                                vellimits[j] = f;
                            }
                        }

                        {
                            dReal f = fStartTimeAccelMult * _parameters->_vConfigAccelerationLimit[j];
                            if( accellimits[j] > f ) {
                                accellimits[j] = f;
                            }
                        }
                    }
                }

                // update limits based on necessray conditions
                _UpdateLimitsByNecessaryConditions(vellimits, accellimits, x0Vect, x1Vect, v0Vect, v1Vect, usedBody);

                std::vector<dReal> reductionFactors2; // keeps track of the reduction factors got from this shortcut

                dReal fCurVelMult = fStartTimeVelMult;
                dReal fCurAccelMult = fStartTimeAccelMult;

                bool bSuccess = false;
                size_t maxSlowDownTries = 100;
                std::fill(velReductionFactors.begin(), velReductionFactors.end(), 1); // Reset reductionfactors
                std::fill(accelReductionFactors.begin(), accelReductionFactors.end(), 1); // Reset reductionfactors
                size_t iSlowDownDueToManip = 0;
                bool bShortcutTimeExceeded = false;
                for (size_t iSlowDown = 0; iSlowDown < maxSlowDownTries; ++iSlowDown) {
                    // compute interpolation. only at the first slowdown iteration, call dedicated function to combine interpolation and computing of initial guess of vellimits and accellimits
                    dReal segmentTime = 0;
                    if( iSlowDown == 0 ) {
                        bool bSlowedDown;
                        if( !_ComputeInterpolationWithInitialGuess(shortcutRampNDVect, segmentTime, vellimits, accellimits, bSlowedDown, fCurVelMult, fCurAccelMult, iIterProgress,
                                                                   t0, t1, minTimeStep, x0Vect, x1Vect, v0Vect, v1Vect, iSlowDown, iters, numIters, usedBody) ) {
                            break;
                        }
                        if( bSlowedDown ) {
                            numSlowDowns += 1;
                            nTimeBasedConstraintsFailed++;
                        }
                    }
                    else {
                        if( !_ComputeInterpolation(shortcutRampNDVect, segmentTime, iIterProgress,
                                                   t0, t1, minTimeStep, x0Vect, x1Vect, v0Vect, v1Vect, vellimits, accellimits, iSlowDown, iters, numIters) ) {
                            break;
                        }
                    }

                    if( _CallCallbacks(_progress) == PA_Interrupt ) {
                        return -1;
                    }
                    if (_parameters->_nMaxPlanningTime > 0) {
                        uint32_t elapsedtime = utils::GetMilliTime() - _basetime;
                        if( elapsedtime >= _parameters->_nMaxPlanningTime ) {
                            bShortcutTimeExceeded = true;
                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut time exceeded (%dms) so breaking. iter=%d < %d", _environmentid%elapsedtime%iters%numIters);
                            break;
                        }
                    }
                    iIterProgress += 0x1000;

                    RampOptimizer::CheckReturn retcheck(0);
                    iIterProgress += 0x10;

                    do { // Start checking constraints.
                        if( _parameters->SetStateValues(x1Vect) != 0 ) {
                            std::stringstream s;
                            s << std::setprecision(RampOptimizer::g_nPrec) << "x1 = [";
                            SerializeValues(s, x1Vect);
                            s << "];";
                            RAVELOG_VERBOSE_FORMAT("env=%d, shortcut iter=%d/%d, cannot set state: %s", _environmentid%iters%numIters%s.str());
                            retcheck.retcode = CFO_StateSettingError;
#ifdef SMOOTHER2_PROGRESS_DEBUG
                            ++vShortcutStats[SS_StateSettingFailed];
                            shortcutprogress << SS_StateSettingFailed << "\n";
#endif
                            break;
                        }
                        _parameters->_getstatefn(x1Vect);
                        iIterProgress += 0x10;

                        retcheck = _feasibilitychecker.Check2(shortcutRampNDVect, 0xffff|CFO_FromTrajectorySmoother, shortcutRampNDVectOut);
#ifdef SMOOTHER2_TIMING_DEBUG
                        _nCallsCheckPathAllConstraints += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                        _totalTimeCheckPathAllConstraints += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                        if( retcheck.retcode != 0 ) {
                            _nCallsCheckPathAllConstraintsInVain += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                            _totalTimeCheckPathAllConstraintsInVain += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                        }
                        // Reset SegmentFeasible2 counters
                        _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                        _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif

                        iIterProgress += 0x10;

                        if( retcheck.retcode != 0 ) {
                            // Shortcut does not pass CheckPathAllConstraints
#ifdef SMOOTHER2_PROGRESS_DEBUG
                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, iSlowDown=%d, shortcut does not pass Check2, retcode=0x%x.\n", _environmentid%iters%numIters%iSlowDown%retcheck.retcode);
                            if( retcheck.retcode == 1 ) {
                                ++vShortcutStats[SS_Check2CollisionFailed];
                                shortcutprogress << SS_Check2CollisionFailed << "\n";
                            }
                            else if( retcheck.retcode != CFO_CheckTimeBasedConstraints ) {
                                ++vShortcutStats[SS_Check2Failed];
                                shortcutprogress << SS_Check2Failed << "\n";
                            }
#endif
                            break;
                        }

                        // CheckPathAllConstraints (called viaSegmentFeasible2 inside Check2) may be
                        // modifying the original shortcutcurvesnd due to constraints. Therefore, we
                        // have to reset vellimits and accellimits so that they are above those of
                        // the modified trajectory.
                        for (size_t irampnd = 0; irampnd < shortcutRampNDVectOut.size(); ++irampnd) {
                            for (size_t jdof = 0; jdof < shortcutRampNDVectOut[irampnd].GetDOF(); ++jdof) {
                                dReal fminvel = max(RaveFabs(shortcutRampNDVectOut[irampnd].GetV0At(jdof)), RaveFabs(shortcutRampNDVectOut[irampnd].GetV1At(jdof)));
                                if( vellimits[jdof] < fminvel ) {
                                    vellimits[jdof] = fminvel;
                                }
                            }
                        }

                        // The interpolated segment passes constraints checking. Now see if it is modified such that it does not end with the desired velocity.
                        if( retcheck.bDifferentVelocity && shortcutRampNDVectOut.size() > 0 ) {
                            RAVELOG_VERBOSE_FORMAT("env=%d, new shortcut is *not* aligned with boundary values after running Check2. Start fixing the last segment.", _environmentid);
                            // Modification inside Check2 results in the shortcut trajectory not ending at the desired velocity v1.
                            dReal allowedStretchTime = (t1 - t0) - (segmentTime + minTimeStep); // the time that this segment is allowed to stretch out such that it is still a useful shortcut

                            shortcutRampNDVectOut.back().GetX0Vect(tempX0Vect);
                            shortcutRampNDVectOut.back().GetV0Vect(tempV0Vect);
#ifdef SMOOTHER2_TIMING_DEBUG
                            _nCallsInterpolator += 1;
                            _tStartInterpolator = utils::GetMicroTime();
#endif
                            bool res2 = _interpolator.ComputeArbitraryVelNDTrajectory(tempX0Vect, x1Vect, tempV0Vect, v1Vect, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, vellimits, accellimits, shortcutRampNDVect, true);
#ifdef SMOOTHER2_TIMING_DEBUG
                            _tEndInterpolator = utils::GetMicroTime();
                            _totalTimeInterpolator += 0.000001f*(float)(_tEndInterpolator - _tStartInterpolator);
#endif
                            if( !res2 ) {
                                // This may be because we cannot fix joint limit violation
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, failed to InterpolateArbitraryVelND to correct the final velocity", _environmentid);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                retcheck.retcode = CFO_FinalValuesNotReached;
                                break;
                            }

                            dReal lastSegmentTime = 0;
                            FOREACHC(itrampnd, shortcutRampNDVect) {
                                lastSegmentTime += itrampnd->GetDuration();
                            }
                            if( lastSegmentTime - shortcutRampNDVectOut.back().GetDuration() > allowedStretchTime ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, the modified last segment duration is too long to be useful(%.15e s.)", _environmentid%iters%numIters%lastSegmentTime);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                retcheck.retcode = CFO_FinalValuesNotReached;
                                break;
                            }

                            retcheck = _feasibilitychecker.Check2(shortcutRampNDVect, 0xffff|CFO_FromTrajectorySmoother, shortcutRampNDVectOut1);
#ifdef SMOOTHER2_TIMING_DEBUG
                            _nCallsCheckPathAllConstraints += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                            _totalTimeCheckPathAllConstraints += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                            if( retcheck.retcode != 0 ) {
                                _nCallsCheckPathAllConstraintsInVain += _nCallsCheckPathAllConstraints_SegmentFeasible2;
                                _totalTimeCheckPathAllConstraintsInVain += _totalTimeCheckPathAllConstraints_SegmentFeasible2;
                            }
                            // Reset SegmentFeasible2 counters
                            _nCallsCheckPathAllConstraints_SegmentFeasible2 = 0;
                            _totalTimeCheckPathAllConstraints_SegmentFeasible2 = 0;
#endif

                            if( retcheck.retcode != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, final segment fixing failed. retcode=0x%x", _environmentid%retcheck.retcode);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                break;
                            }
                            else if( retcheck.bDifferentVelocity ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, after final segment fixing, shortcutRampND still does not end at the desired velocity", _environmentid);
                                ++vShortcutStats[SS_LastSegmentFailed];
                                shortcutprogress << SS_LastSegmentFailed << "\n";
#endif
                                retcheck.retcode = CFO_FinalValuesNotReached;
                                break;
                            }
                            else {
                                // Otherwise, this segment is good.
                                RAVELOG_VERBOSE_FORMAT("env=%d, final velocity correction for the last segment successful", _environmentid);
                                shortcutRampNDVectOut.pop_back();
                                shortcutRampNDVectOut.insert(shortcutRampNDVectOut.end(), shortcutRampNDVectOut1.begin(), shortcutRampNDVectOut1.end());

                                // Check consistency
                                if( IS_DEBUGLEVEL(Level_Verbose) ) {
                                    shortcutRampNDVectOut.front().GetX0Vect(x0Vect);
                                    shortcutRampNDVectOut.back().GetX1Vect(x1Vect);
                                    shortcutRampNDVectOut.front().GetV0Vect(v0Vect);
                                    shortcutRampNDVectOut.back().GetV1Vect(v1Vect);
                                    RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampNDs(shortcutRampNDVectOut, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit, x0Vect, x1Vect, v0Vect, v1Vect);
                                    OPENRAVE_ASSERT_OP(parabolicret, ==, RampOptimizer::PCR_Normal);
                                }
                            }
                        }
                        else {
                            RAVELOG_VERBOSE_FORMAT("env=%d, new shortcut is aligned with boundary values after running Check2", _environmentid);
                            break;
                        }
                    } while (0);
                    // Finished checking constraints. Now see what retcheck.retcode is
                    iIterProgress += 0x1000;

                    if( retcheck.retcode == 0 ) {
                        // Shortcut is successful.
                        bSuccess = true;
                        break;
                    }
                    else if( retcheck.retcode == CFO_CheckTimeBasedConstraints ) {
                        // CFO_CheckTimeBasedConstraints can be caused by the following
                        // - torque limit violation
                        // - manipulator speed/accel constraint violation
                        // - joint velocity adjustment done in SegmentFeasible2
                        nTimeBasedConstraintsFailed++;

                        // Scale down vellimits and/or accellimits
                        if( _bmanipconstraints && _manipconstraintchecker ) {
                            // Scale down vellimits and accellimits independently according to the violated constraint (manipspeed/manipaccel)
                            {
                                // After computing the new vellimits and accellimits and they don't work, we gradually scale vellimits/accellimits down
                                dReal fVelMult, fAccelMult;
                                bool maxManipSpeedViolated = false, maxManipAccelViolated = false;
                                if( retcheck.fMaxManipAccel > _parameters->maxmanipaccel ) {
                                    ++iSlowDownDueToManip;
                                    // Manipaccel is violated. We scale both vellimits and accellimits down.
                                    maxManipAccelViolated = true;
                                    if( _bUseNewHeuristic && retcheck.vReductionFactors.size() > 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                        std::stringstream ss; ss << "env=" << _environmentid << ", maxManipAccelViolated=1 (";
                                        ss << retcheck.fMaxManipAccel << " > " << _parameters->maxmanipaccel << "); reductionFactors=[";
                                        FOREACHC(itval, retcheck.vReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; velReductionFactors=[";
                                        FOREACHC(itval, velReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; accelReductionFactors=[";
                                        FOREACHC(itval, accelReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "];";
                                        RAVELOG_DEBUG(ss.str());
#endif
                                        for( size_t j = 0; j < vellimits.size(); ++j ) {
                                            dReal fMinVelLimit = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            fVelMult = RaveSqrt(retcheck.vReductionFactors[j]);
                                            if( vellimits[j] * fVelMult < fMinVelLimit ) {
                                                // In this case, we cannot use the recommended scaling factor since
                                                // after scaling, the vellimits will fall below max(v0, v1). So we set
                                                // vellimits to be max(v0, v1) instead.
                                                velReductionFactors[j] *= (fMinVelLimit / vellimits[j]);
                                                vellimits[j] = fMinVelLimit + RampOptimizer::g_fRampEpsilon;
                                            }
                                            else {
                                                vellimits[j] *= fVelMult;
                                                velReductionFactors[j] *= fVelMult;
                                            }
                                            accellimits[j] *= retcheck.vReductionFactors[j];
                                            accelReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        }
                                        // for( size_t j = 0; j < vellimits.size(); ++j ) {
                                        //     vellimits[j] *= RaveSqrt(retcheck.vReductionFactors[j]);
                                        //     accellimits[j] *= retcheck.vReductionFactors[j];
                                        //     velReductionFactors[j] *= RaveSqrt(retcheck.vReductionFactors[j]);
                                        //     accelReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        // }
                                    }
                                    else {
                                        fAccelMult = retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                                        fCurAccelMult *= fAccelMult;
                                        if( fCurAccelMult < 0.0001 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: maxmanipaccel violated but fCurAccelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurAccelMult);
                                            ++vShortcutStats[SS_MaxManipAccelFailed];
                                            shortcutprogress << SS_MaxManipAccelFailed << "\n";
#endif
                                            break;
                                        }
                                        {
                                            fVelMult = retcheck.fTimeBasedSurpassMult; // larger scaling factor, less reduction. Use a square root here since the velocity has the factor t while the acceleration has t^2
                                            fCurVelMult *= fVelMult;
                                            if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: maxmanipaccel violated but fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                                ++vShortcutStats[SS_MaxManipAccelFailed];
                                                shortcutprogress << SS_MaxManipAccelFailed << "\n";
#endif
                                                break;
                                            }
                                            for (size_t j = 0; j < vellimits.size(); ++j) {
                                                dReal fMinVel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                                vellimits[j] = max(fMinVel, fVelMult * vellimits[j]);
                                            }
                                        }
                                        for (size_t j = 0; j < accellimits.size(); ++j) {
                                            accellimits[j] *= fAccelMult;
                                        }
                                    }
                                }
                                else if( retcheck.fMaxManipSpeed > _parameters->maxmanipspeed ) {
                                    ++iSlowDownDueToManip;
                                    // Manipspeed is violated. We don't scale down accellimits.
                                    maxManipSpeedViolated = true;
                                    if( _bUseNewHeuristic && retcheck.vReductionFactors.size() > 0 && !(retcheck.fMaxManipAccel > _parameters->maxmanipaccel)) {
                                        // do vel scaling without accel scaling only when accel limit is not violated
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                        std::stringstream ss; ss << "env=" << _environmentid << ", maxManipSpeedViolated=1 (";
                                        ss << retcheck.fMaxManipSpeed << " > " << _parameters->maxmanipspeed << "); reductionFactors=[";
                                        FOREACHC(itval, retcheck.vReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; velReductionFactors=[";
                                        FOREACHC(itval, velReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; accelReductionFactors=[";
                                        FOREACHC(itval, accelReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "];";
                                        RAVELOG_DEBUG(ss.str());
#endif
                                        for( size_t j = 0; j < vellimits.size(); ++j ) {
                                            dReal fMinVelLimit = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            if( vellimits[j] * retcheck.vReductionFactors[j] < fMinVelLimit ) {
                                                // In this case, we cannot use the recommended scaling factor since
                                                // after scaling, the vellimits will fall below max(v0, v1). So we set
                                                // vellimits to be max(v0, v1) instead.
                                                velReductionFactors[j] *= (fMinVelLimit / vellimits[j]);
                                                vellimits[j] = fMinVelLimit + RampOptimizer::g_fRampEpsilon;
                                            }
                                            else {
                                                vellimits[j] *= retcheck.vReductionFactors[j];
                                                velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                            }
                                            // vellimits[j] *= retcheck.vReductionFactors[j];
                                            // velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        }
                                    }
                                    else {
                                        fVelMult = retcheck.fTimeBasedSurpassMult;
                                        fCurVelMult *= fVelMult;
                                        if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: maxmanipspeed violated but fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                            ++vShortcutStats[SS_MaxManipSpeedFailed];
                                            shortcutprogress << SS_MaxManipSpeedFailed << "\n";

#endif
                                            break;
                                        }
                                        for (size_t j = 0; j < vellimits.size(); ++j) {
                                            dReal fMinVel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            vellimits[j] = max(fMinVel, fVelMult * vellimits[j]);
                                        }
                                    }
                                }

                                if( !maxManipSpeedViolated && !maxManipAccelViolated ) {
                                    // Got a failure due to time-based constraints but manip speed/accel are not
                                    // violated. This causes by velocity limits violation from ramps that have been
                                    // modified by CheckPathAllConstraints.
                                    if( _bUseNewHeuristic && retcheck.vReductionFactors.size() > 0 ) {
                                        // do vel scaling without accel scaling
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                        std::stringstream ss; ss << "env=" << _environmentid << ", reductionFactors=[";
                                        FOREACHC(itval, retcheck.vReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; velReductionFactors=[";
                                        FOREACHC(itval, velReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "]; accelReductionFactors=[";
                                        FOREACHC(itval, accelReductionFactors) {
                                            ss << *itval << ", ";
                                        }
                                        ss << "];";
                                        RAVELOG_DEBUG(ss.str());
#endif
                                        for( size_t j = 0; j < vellimits.size(); ++j ) {
                                            dReal fMinVelLimit = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            if( vellimits[j] * retcheck.vReductionFactors[j] < fMinVelLimit ) {
                                                // In this case, we cannot use the recommended scaling factor since
                                                // after scaling, the vellimits will fall below max(v0, v1). So we set
                                                // vellimits to be max(v0, v1) instead.
                                                velReductionFactors[j] *= (fMinVelLimit / vellimits[j]);
                                                vellimits[j] = fMinVelLimit + RampOptimizer::g_fRampEpsilon;
                                            }
                                            else {
                                                vellimits[j] *= retcheck.vReductionFactors[j];
                                                velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                            }
                                            // vellimits[j] *= retcheck.vReductionFactors[j];
                                            // velReductionFactors[j] *= retcheck.vReductionFactors[j];
                                        }
                                    }
                                    else {
                                        fVelMult = retcheck.fTimeBasedSurpassMult;
                                        fAccelMult = retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                                        fCurVelMult *= fVelMult;
                                        fCurAccelMult *= fAccelMult;
                                        if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: modified ramps exceed vellimits/accellimits but fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                            ++vShortcutStats[SS_Check2Failed];
                                            shortcutprogress << SS_Check2Failed << "\n";
#endif
                                            break;
                                        }
                                        if( fCurAccelMult < 0.0001 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: modified ramps exceed vellimits/accellimits but fCurAccelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurAccelMult);
                                            ++vShortcutStats[SS_Check2Failed];
                                            shortcutprogress << SS_Check2Failed << "\n";
#endif
                                            break;
                                        }
                                        for (size_t j = 0; j < vellimits.size(); ++j) {
                                            dReal fMinVel = max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                            vellimits[j] = max(fMinVel, fVelMult * vellimits[j]);
                                            accellimits[j] *= fAccelMult;
                                        }
                                    }
                                }
                                numSlowDowns += 1;
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, maxManipSpeedViolated=%d, maxManipAccelViolated=%d, fTimeBasedSurpassMult=%.15e; fCurVelMult=%.15e; fCurAccelMult=%.15e, numSlowDowns=%d", _environmentid%maxManipSpeedViolated%maxManipAccelViolated%retcheck.fTimeBasedSurpassMult%fCurVelMult%fCurAccelMult%numSlowDowns);
#endif
                            }
                        }
                        else {
                            // Scale down vellimits and accellimits using the normal procedure
                            fCurVelMult *= retcheck.fTimeBasedSurpassMult;
                            fCurAccelMult *= retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                            if( fCurVelMult < 0.01 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: fCurVelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurVelMult);
                                ++vShortcutStats[SS_SlowDownFailed];
                                shortcutprogress << SS_SlowDownFailed << "\n";
#endif
                                break;
                            }
                            if( fCurAccelMult < 0.0001 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d: fCurAccelMult is too small (%.15e). continue to the next iteration", _environmentid%iters%numIters%fCurAccelMult);
                                ++vShortcutStats[SS_SlowDownFailed];
                                shortcutprogress << SS_SlowDownFailed << "\n";
#endif
                                break;
                            }

                            numSlowDowns += 1;
                            for (size_t j = 0; j < vellimits.size(); ++j) {
                                dReal fMinVel =  max(RaveFabs(v0Vect[j]), RaveFabs(v1Vect[j]));
                                vellimits[j] = max(fMinVel, retcheck.fTimeBasedSurpassMult * vellimits[j]);
                                accellimits[j] *= retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult;
                            }
                        }
                    }
                    else {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                        RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, rejecting shortcut due to constraint 0x%x", _environmentid%iters%numIters%retcheck.retcode);
#endif
                        break;
                    }
                    iIterProgress += 0x1000;
                } // Finished slowing down the shortcut
                if (bShortcutTimeExceeded) {
                    break;
                }

                if( !bSuccess ) {
                    // Shortcut failed. Continue to the next iteration.
                    continue;
                }

                if( shortcutRampNDVectOut.size() == 0 ) {
                    RAVELOG_WARN_FORMAT("env=%d, shortcutpath is empty!", _environmentid);
                    continue;
                }

                // Now this shortcut is really successful
                ++numShortcuts;
#ifdef SMOOTHER2_PROGRESS_DEBUG
                ++vShortcutStats[SS_Successful];
                shortcutprogress << SS_Successful << "\n";
                latestSuccessfulShortcutTimestamp = utils::GetMicroTime();
#endif

                nTimeBasedConstraintsFailed = 0; // reset
                vVisitedDiscretization.clear(); // have to clear so that can recreate the visited nodes

                // Keep track of zero-velocity waypoints
                dReal segmentTime = 0;
                FOREACHC(itrampnd, shortcutRampNDVectOut) {
                    segmentTime += itrampnd->GetDuration();
                }
                dReal diff = (t1 - t0) - segmentTime;

                size_t writeIndex = 0;
                for( size_t readIndex = 0; readIndex < _vZeroVelPointInfos.size(); ++readIndex ) {
                    if( _vZeroVelPointInfos[readIndex].point <= t0 ) {
                        writeIndex += 1;
                    }
                    else if( _vZeroVelPointInfos[readIndex].point <= t1 ) {
                        // Do nothing.
                    }
                    else {
                        // Update all zero-velocity points after t1
                        _vZeroVelPointInfos[writeIndex] = _vZeroVelPointInfos[readIndex];
                        _vZeroVelPointInfos[writeIndex].point -= diff;
                        _vZeroVelPointInfos[writeIndex].leftneighbor -= diff;
                        _vZeroVelPointInfos[writeIndex].rightneighbor -= diff;
                        writeIndex += 1;
                    }
                }
                _vZeroVelPointInfos.resize(writeIndex);

                // Now replace the original trajectory segment by the shortcut
                parabolicpath.ReplaceSegment(t0, t1, shortcutRampNDVectOut);
                iIterProgress += 0x10000000;

                rampndVect = parabolicpath.GetRampNDVect();

                // Check consistency
                if( IS_DEBUGLEVEL(Level_Verbose) ) {
                    rampndVect.front().GetX0Vect(x0Vect);
                    rampndVect.back().GetX1Vect(x1Vect);
                    rampndVect.front().GetV0Vect(v0Vect);
                    rampndVect.back().GetV1Vect(v1Vect);
                    RampOptimizer::ParabolicCheckReturn parabolicret = RampOptimizer::CheckRampNDs(rampndVect, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigAccelerationLimit, x0Vect, x1Vect, v0Vect, v1Vect);
                    OPENRAVE_ASSERT_OP(parabolicret, ==, RampOptimizer::PCR_Normal);
                }
                iIterProgress += 0x10000000;

                tTotal = parabolicpath.GetDuration();

                // Calculate the score
                score = diff/nItersFromPrevSuccessful;
                if( score > currentBestScore) {
                    currentBestScore = score;
                    iCurrentBestScore = 1.0/currentBestScore;
                }
                nItersFromPrevSuccessful = 0;

                RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d successful, numSlowDowns=%d, tTotal=%.15e, t0=%.15e, t1=%.15e, fVelAccMult=[%f, %f]->[%f,%f], score=%.15e, bestScore=%.15e", _environmentid%iters%numIters%numSlowDowns%tTotal%t0%t1%fStartTimeVelMult%fStartTimeAccelMult%fCurVelMult%fCurAccelMult%score%currentBestScore);

                if( (score*iCurrentBestScore < cutoffRatio) && (numShortcuts > 5)) {
                    // We have already shortcut for a bit (numShortcuts > 5). The progress made in
                    // this iteration is below the curoff ratio. If we continue, it is unlikely that
                    // we will make much more progress. So stop here.
                    break;
                }
            }
            catch (const std::exception& ex) {
                RAVELOG_WARN_FORMAT("env=%d, An exception happened during shortcut iteration progress = 0x%x: %s", _environmentid%iIterProgress%ex.what());
            }
        }

        // Report status
        if( iters == numIters ) {
            RAVELOG_DEBUG_FORMAT("env=%d, finished at shortcut iter=%d (normal exit), successful=%d, slowdowns=%d, endTime: %.15e -> %.15e; diff = %.15e", _environmentid%iters%numShortcuts%numSlowDowns%tOriginal%tTotal%(tOriginal - tTotal));
        }
        else if( score*iCurrentBestScore < cutoffRatio ) {
            RAVELOG_DEBUG_FORMAT("env=%d, finished at shortcut iter=%d (current score falls below %.15e), successful=%d, slowdowns=%d, endTime: %.15e -> %.15e; diff = %.15e", _environmentid%iters%cutoffRatio%numShortcuts%numSlowDowns%tOriginal%tTotal%(tOriginal - tTotal));
        }
        else if( nItersFromPrevSuccessful + nTimeBasedConstraintsFailed > nCutoffIters ) {
            RAVELOG_DEBUG_FORMAT("env=%d, finished at shortcut iter=%d (did not make progress in the last %d iterations and time-based constraints failed %d times), successful=%d, slowdowns=%d, endTime: %.15e -> %.15e; diff = %.15e", _environmentid%iters%nItersFromPrevSuccessful%nTimeBasedConstraintsFailed%numShortcuts%numSlowDowns%tOriginal%tTotal%(tOriginal - tTotal));
        }
        else {
            RAVELOG_DEBUG_FORMAT("env=%d, finished at shortcut iter=%d, successful=%d, slowdowns=%d, endTime: %.15e -> %.15e; diff = %.15e", _environmentid%iters%numShortcuts%numSlowDowns%tOriginal%tTotal%(tOriginal - tTotal));
        }
        _DumpParabolicPath(parabolicpath, _dumplevel, 1);
#ifdef SMOOTHER2_PROGRESS_DEBUG
        curtime = utils::GetMicroTime();
        RAVELOG_DEBUG_FORMAT("env=%d, shortcut stats:\n  successful=%d\n  initialInterpolationFailed=%d\n  interpolatedSegmentTooLong=%d\n  interpolatedSegmentTooLongFromSlowDown=%d\n  timeInstantsTooClose=%d\n  check2CollisionFailed=%d\n  check2Failed=%d\n  lastSegmentFailed=%d\n  maxManipSpeedFailed=%d\n  maxManipAccelFailed=%d\n  slowDownFailed=%d\n  stateSettingFailed=%d\n  redundantShortcut=%d\n  _vZeroVelPointInfos.size()=%d\n  time since last successful shortcut=%.15e\n  final duration percentage=%.15e", _environmentid%vShortcutStats[SS_Successful]%vShortcutStats[SS_InitialInterpolationFailed]%vShortcutStats[SS_InterpolatedSegmentTooLong]%vShortcutStats[SS_InterpolatedSegmentTooLongFromSlowDown]%vShortcutStats[SS_TimeInstantsTooClose]%vShortcutStats[SS_Check2CollisionFailed]%vShortcutStats[SS_Check2Failed]%vShortcutStats[SS_LastSegmentFailed]%vShortcutStats[SS_MaxManipSpeedFailed]%vShortcutStats[SS_MaxManipAccelFailed]%vShortcutStats[SS_SlowDownFailed]%vShortcutStats[SS_StateSettingFailed]%vShortcutStats[SS_RedundantShortcut]%_vZeroVelPointInfos.size()%(0.000001f*(float)(curtime - latestSuccessfulShortcutTimestamp))%(tTotal/tOriginal));
        {
            std::string shortcutprogressfilename = str(boost::format("%s/shortcutprogress%d.xml")%RaveGetHomeDirectory()%_fileIndex);
            std::ofstream f(shortcutprogressfilename.c_str());
            f << shortcutprogress.str();
            RAVELOG_DEBUG_FORMAT("env=%d, shortcutprogress saved to %s", _environmentid%shortcutprogressfilename);
        }
#endif
#ifdef SMOOTHER2_TIMING_DEBUG
        _numShortcutIters = iters;
#endif

        return numShortcuts;
    }

    /// \brief dump ParabolicPath.
    /// \param[in] parabolicpath : parabolicpath to dump
    /// \param[in] level : debug level
    /// \param[in] option : option to determine the additional file name.
    void _DumpParabolicPath(const RampOptimizer::ParabolicPath& parabolicpath, const DebugLevel level, const int option) const
    {
        if( !IS_DEBUGLEVEL(level) ) {
            return;
        }

        std::string filename;
        if( option == 0 ) {
            filename = str(boost::format("%s/parabolicpath%d.beforeshortcut.xml")%RaveGetHomeDirectory()%_fileIndex);
        }
        else if( option == 1 ) {
            filename = str(boost::format("%s/parabolicpath%d.aftershortcut.xml")%RaveGetHomeDirectory()%_fileIndex);
        }
        else if( option == 2 ) {
            filename = str(boost::format("%s/parabolicpath%d.beforemerge.xml")%RaveGetHomeDirectory()%_fileIndex);
        }
        else if( option == 3 ) {
            filename = str(boost::format("%s/parabolicpath%d.aftermerge.xml")%RaveGetHomeDirectory()%_fileIndex);
        }
        else {
            filename = str(boost::format("%s/parabolicpath%d.xml")%RaveGetHomeDirectory()%_fileIndex);
        }
        ofstream f(filename.c_str());
        f << std::setprecision(RampOptimizer::g_nPrec);
        parabolicpath.Serialize(f);
        // RavePrintfA(str(boost::format("env=%d, Wrote a parabolicpath to %s (duration = %.15e, num=%d)")%_environmentid%filename%parabolicpath.GetDuration()%parabolicpath.GetRampNDVect().size()), level);
        RAVELOG_DEBUG_FORMAT("env=%d, parabolicpath saved to %s (duration=%.15e, num=%d)", _environmentid%filename%parabolicpath.GetDuration()%parabolicpath.GetRampNDVect().size());
        return;
    }

    /// \brief dump openrave trajectory
    /// \param[in] ptraj : trajectory to dump
    /// \param[in] level : debug level
    /// \param[in] option : option to determine the additional file name.
    void _DumpTrajectory(TrajectoryBasePtr ptraj, const DebugLevel level, const int option)
    {
        if( IS_DEBUGLEVEL(level) ) {
            std::string filename = _DumpTrajectory(ptraj, option);
            RavePrintfA(str(boost::format("env=%d, Wrote trajectory to %s")%_environmentid%filename), level);
        }
    }

    /// \brief dump openrave trajectory
    /// \param[in] ptraj : trajectory to dump
    /// \param[in] level : debug level
    /// \param[in] option : option to determine the additional file name.
    /// \return filename to dump
    std::string _DumpTrajectory(TrajectoryBasePtr ptraj, const int option)
    {
        std::string filename;
        switch( option )
        {
        case 0:
            filename = str(boost::format("%s/parabolicsmoother2_%d_init.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        case 1:
            filename = str(boost::format("%s/parabolicsmoother2_%d_convfromcubic.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        case 2:
            filename = str(boost::format("%s/parabolicsmoother2_%d_setmilestonefail.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        case 3:
            filename = str(boost::format("%s/parabolicsmoother2_%d_constraintfail.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        case 4:
            filename = str(boost::format("%s/parabolicsmoother2_%d_exception.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        case 5:
            filename = str(boost::format("%s/parabolicsmoother2_%d_sampleverifyfail.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        case 6:
            filename = str(boost::format("%s/parabolicsmoother2_%d_result.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        default:
            filename = str(boost::format("%s/parabolicsmoother2_%d.traj.xml")%RaveGetHomeDirectory()%_fileIndex);
            break;
        }
        ofstream f(filename.c_str());
        f << std::setprecision(RampOptimizer::g_nPrec);
        ptraj->serialize(f);
        return filename;
    }

    /// \brief ensure that t0 and t1 for smoother have valid values.
    ///        1. at least, within range in [0, tTotal]
    ///        2. t0 <= t1
    ///        this ensure is used just in case, since the computed t0 and t1 might violate these assumptions due to numerical epsilons even if it's feasible theoretically.
    inline void _EnsureValidlySampledTimes(dReal& t0, dReal& t1, const dReal tTotal) const
    {
        t0 = std::max(0.0, std::min(t0, tTotal));
        t1 = std::max(0.0, std::min(t1, tTotal));
        if( t0 > t1 ) {
            RampOptimizer::Swap(t0, t1);
        }
    }

    /// \brief sample times t0 and t1 in [0, tTotal].
    ///        constraints
    ///          0 <= t0 <= tTotal
    ///          0 <= t1 <= tTotal
    ///          t0 + minTimeStep <= t1
    ///          where, minTimeStep >= 0, tTotal >= minTimeStep
    ///        <=>
    ///          0  <= t0 <= tTotal - minTimeStep
    ///          t0 + minTimeStep <= t1 <= tTotal
    ///          where, minTimeStep >= 0, tTotal >= minTimeStep
    ///        note that t0 and t1 are always feasible
    /// \param[out] t0, t1 : t0 and t1
    /// \param[in] r0, r1 : ratio in [0.0, 1.0] to sample t0 and t1 within range.
    /// \param[in] tTotal : max time. tTotal > 0
    /// \param[in] minTimeStep : min time between t1 and t0. minTimeStep>=0.
    void _SampleTime(dReal& t0, dReal& t1,
                     const dReal r0, const dReal r1,
                     const dReal tTotal, const dReal minTimeStep) const
    {
        // sample within range
        t0 = r0*(tTotal - minTimeStep);
        const dReal fMinT1 = t0 + minTimeStep;
        t1 = r1*(tTotal - fMinT1) + fMinT1;
        // ensure
        _EnsureValidlySampledTimes(t0, t1, tTotal);
    }

    /// \brief sample times t0 and t1 in [0, tTotal] around tCenter.
    ///        memo for previous implementation to sample t0 and t1
    ///          t0 = tCenter - r0'*min(cutoffTime, tCenter) <-> tCenter-cutoffTime<=t0<=t or 0<=t0<=tCenter <-> max(0, tCenter-cutoffTime)<=t0<=t
    ///          t1 = tCenter + r1'*min(cutoffTime, tTotal-tCenter) <-> tCenter<=t1<=tCenter+cutoffTime or tCenter<=t1<=tTotal <-> tCenter<=t1<=min(tTotal, tCenter+cutoffTime)
    ///
    ///        constraints
    ///          max(0, tCenter-cutoffTime) <= t0 <= tCenter
    ///          tCenter <= t1 <= min(tTotal, tCenter+cutoffTime)
    ///          t0 + minTimeStep <= t1
    ///          where, minTimeStep >= 0, tTotal >= minTimeStep, cutoffTime>0, tTotal>=tCenter>=0
    ///        <=>
    ///          max(0, tCenter-cutoffTime) <= t0 <= min(tCenter, min(tTotal, tCenter+cutoffTime) - minTimeStep)
    ///          max(t0 + minTimeStep, tCenter) <= t1 <= min(tTotal, tCenter+cutoffTime)
    ///          where, minTimeStep >= 0, tTotal >= minTimeStep, cutoffTime>0
    ///
    ///        case 1. if t0 is feasible, t1 is also feasible in the above.
    ///        case 2. if t0 is infeasible, relax the condition to solve the problem.
    ///                among the constraints, cutoffTime is not mandatory. if relax cutoffTime, the problem becomes feasible by solving the following.
    ///
    ///          0 <= t0 <= min(tCenter, tTotal - minTimeStep)
    ///          max(t0 + minTimeStep, tCenter) <= t1 <= tTotal
    ///          where, minTimeStep >= 0, tTotal >= minTimeStep, tTotal>=tCenter>=0
    ///
    /// \param[out] t0, t1 : t0 and t1
    /// \param[in] r0, r1 : ratio in [0.0, 1.0] to sample t0 and t1 within range.
    /// \param[in] tTotal : max time
    /// \param[in] minTimeStep : min time between t1 and t0
    /// \param[in] tCenterArg : center of time. t0 <= tCenterArg <= t1
    /// \param[in] cutoffTime : cutoff time of range of t0 and t1 sampling.
    void _SampleTimeAroundCenter(dReal& t0, dReal& t1,
                                 const dReal r0, const dReal r1,
                                 const dReal tTotal, const dReal minTimeStep,
                                 const dReal tCenterArg, const dReal cutoffTime) const
    {
        // first, make sure 0<=tCenter<=tTotal. otherwise, something wrong with the caller side.
        const dReal tCenter = std::max(std::min(tCenterArg, tTotal), 0.0);
        // compute tentative min max
        dReal fMaxT1 = std::min(tTotal, tCenter + cutoffTime);
        dReal fMinT0 = std::max(0.0, tCenter - cutoffTime);
        dReal fMaxT0 = std::min(tCenter, fMaxT1 - minTimeStep);
        // if t0 range is infeasible (case1), re-write min max to relax the condition.
        if( fMaxT0 < fMinT0 ) {
            fMaxT0 = std::min(tCenter, tTotal - minTimeStep);
            fMinT0 = 0.0;
            fMaxT1 = tTotal;
        }
        // sample within range
        t0 = r0*(fMaxT0 - fMinT0) + fMinT0;
        const dReal fMinT1 = std::max(tCenter, t0 + minTimeStep); // fMinT1 <= tTotal is respected
        t1 = r1*(fMaxT1 - fMinT1) + fMinT1;
        // ensure
        _EnsureValidlySampledTimes(t0, t1, tTotal);
    }

    /// \brief compute interpolation. expected to be called from _MergeConsecutiveSegments and _Shortcut.
    ///        note that internally uses debugging member variables (_vShortcutStats and _ssshortcutprogress) if SMOOTHER2_PROGRESS_DEBUG is enabled.
    /// \param[out] shortcutRampNDVect : vector of ramps to contain resultant interpolation result.
    /// \param[out] segmentTime : resultant segment time
    /// \param[out] iIterProgress : for debugging purpose
    /// \param[in] t0, t1, minTimeStep : time values used in shortcut iteration.
    /// \param[in] x0Vect, x1Vect, v0Vect, v1Vect : positions and velocities at t0 and t1. used for interpolation.
    /// \param[in] vellimits, accellimits : vel/accel limits used for interpolation.
    /// \return true if successful, e.g. interpolation is successful and resultant segment time is not too long.
    bool _ComputeInterpolation(std::vector<RampOptimizer::RampND>& shortcutRampNDVect, dReal& segmentTime, uint32_t& iIterProgress,
                               const dReal t0, const dReal t1, const dReal minTimeStep,
                               const std::vector<dReal>& x0Vect, const std::vector<dReal>& x1Vect, const std::vector<dReal>& v0Vect, const std::vector<dReal>& v1Vect,
                               const std::vector<dReal>& vellimits, const std::vector<dReal>& accellimits,
                               const size_t iSlowDown, const int iters, const int numIters)
    {
#ifdef SMOOTHER2_TIMING_DEBUG
        _nCallsInterpolator += 1;
        _tStartInterpolator = utils::GetMicroTime();
#endif
        bool res = _interpolator.ComputeArbitraryVelNDTrajectory(x0Vect, x1Vect, v0Vect, v1Vect, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, vellimits, accellimits, shortcutRampNDVect, true);
#ifdef SMOOTHER2_TIMING_DEBUG
        _tEndInterpolator = utils::GetMicroTime();
        _totalTimeInterpolator += 0.000001f*(float)(_tEndInterpolator - _tStartInterpolator);
#endif
        iIterProgress += 0x1000;
        if( !res ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, initial interpolation failed.", _environmentid%iters%numIters);
            ++_vShortcutStats[SS_InitialInterpolationFailed];
            _ssshortcutprogress << SS_InitialInterpolationFailed << "\n";
#endif
            return false;
        }

        // Check if the shortcut makes a significant improvement
        segmentTime = 0;
        FOREACHC(itrampnd, shortcutRampNDVect) {
            segmentTime += itrampnd->GetDuration();
        }
        if( segmentTime + minTimeStep > t1 - t0 ) {
            // RAVELOG_VERBOSE_FORMAT("env=%d, shortcut iter=%d/%d, rejecting shortcut from t0 = %.15e to t1 = %.15e, %.15e > %.15e, minTimeStep = %.15e, final trajectory duration = %.15e s.",
            //                        _environmentid%iters%numIters%t0%t1%segmentTime%(t1 - t0)%minTimeStep%parabolicpath.GetDuration());
#ifdef SMOOTHER2_PROGRESS_DEBUG
            RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, rejecting since it will not make significant improvement. originalSegmentTime=%.15e, newSegmentTime=%.15e, diff=%.15e, minTimeStep=%.15e", _environmentid%iters%numIters%(t1 - t0)%segmentTime%(t1 - t0 - segmentTime)%minTimeStep);
            if( iSlowDown == 0 ) {
                ++_vShortcutStats[SS_InterpolatedSegmentTooLong];
                _ssshortcutprogress << SS_InterpolatedSegmentTooLong << "\n";
            }
            else {
                ++_vShortcutStats[SS_InterpolatedSegmentTooLongFromSlowDown];
                _ssshortcutprogress << SS_InterpolatedSegmentTooLongFromSlowDown << "\n";
            }
#endif
            return false;
        }

#ifdef SMOOTHER2_PROGRESS_DEBUG
        RAVELOG_DEBUG_FORMAT("env=%d, shortcut iter=%d/%d, finished initial interpolation. originalSegmentTime=%.15e, newSegmentTime=%.15e, diff=%.15e, minTimeStep=%.15e", _environmentid%iters%numIters%(t1 - t0)%segmentTime%(t1 - t0 - segmentTime)%minTimeStep);
#endif
        return true;
    }


    /// \brief compute dynamic limits. for now, acc limits only
    ///        all vectors in arguments have same size and order config, like _parameters->_vConfigVelocityLimit.
    /// \param[out] vAccelLimits : resultant acceleration limits.
    /// \param[in] xVect, vVect : pos and vel.
    /// \param[in] usedBody : body to compute dynamic limits
    void _ComputeDynamicLimits(std::vector<dReal>& vAccelLimits,
                               const std::vector<dReal>& xVect, const std::vector<dReal>& vVect, const KinBody& usedBody)
    {
        vAccelLimits.resize(xVect.size());
        for(int iDOF = 0; iDOF < (int)_dynamicLimitInfo.vUsedDOFIndices.size(); ++iDOF) {
            _dynamicLimitInfo.vCacheFullDOFPositions[_dynamicLimitInfo.vUsedDOFIndices[iDOF]] = xVect[iDOF];
            _dynamicLimitInfo.vCacheFullDOFVelocities[_dynamicLimitInfo.vUsedDOFIndices[iDOF]] = max(-_parameters->_vConfigVelocityLimit[iDOF], min(vVect[iDOF], _parameters->_vConfigVelocityLimit[iDOF])); // just in case, clamp by config velocity limits.
        }
        usedBody.GetDOFDynamicAccelerationJerkLimits(_dynamicLimitInfo.vCacheFullDOFAccelerationLimits, _dynamicLimitInfo.vCacheFullDOFJerkLimits,
                                                     _dynamicLimitInfo.vCacheFullDOFPositions, _dynamicLimitInfo.vCacheFullDOFVelocities);
        for(int iDOF = 0; iDOF < (int)_dynamicLimitInfo.vUsedDOFIndices.size(); ++iDOF) {
            vAccelLimits[iDOF] = _dynamicLimitInfo.vCacheFullDOFAccelerationLimits[_dynamicLimitInfo.vUsedDOFIndices[iDOF]]*_dynamicLimitInfo.fDynamicAccelerationLimitMult;
        }
    }

    /// \brief update limits by necesary conditions.
    ///        all vectors in arguments have same size and order config, like _parameters->_vConfigVelocityLimit.
    ///
    ///        current time-based constraints:
    ///          - dynamic acc limits
    ///            |ddq| <= a(q, dq)
    ///          - manip speed limits
    ///            |J(q) dq|^2 <= |dr+|^2
    ///          - manip accel limits
    ///            |J(q) ddq + dJ(q, dq) dq|^2 <= |ddr+|^2
    ///          - toruqe limits:
    ///            tq- <= M(q)ddq + C(q, dq) <= tq+
    ///          in this function, we first focus on boundary condition (t0 and t1). thus, q and dq are assume to be known.
    ///          dynamics acc limits becomes closed-form inequalities about ddq, while others are not. thus, we only support dynamic acc limits only in this function. (btw, manip speed is not relevant in this function).
    /// \param[out] vVelLimits, vAccelLimits : resultant acceleration limits. expected to have the default value in it.
    /// \param[in] vVelocityLimits : just in case, clamp velocity by velocity limits, since dynamic limit might be ill-condition.
    /// \param[in] x0Vect, v0Vect, x1Vect, v1Vect : positions(q) and velocities(dq) at t0 and t1.
    void _UpdateLimitsByNecessaryConditions(std::vector<dReal>& vVelLimits, std::vector<dReal>& vAccelLimits,
                                            const std::vector<dReal>& x0Vect, const std::vector<dReal>& x1Vect,
                                            const std::vector<dReal>& v0Vect, const std::vector<dReal>& v1Vect,
                                            const KinBodyConstPtr usedBody)
    {
        if( _dynamicLimitInfo.bUseDynamicLimits && !!usedBody ) {
            // acceleration limits should at least satisfy the dynamic acceleration limits at t0 and t1.
            // check dynamic acceleration limit at t0
            std::vector<dReal>& vTmpALim = _dynamicLimitInfo.vCacheTmpALim;
            _ComputeDynamicLimits(vTmpALim, x0Vect, v0Vect, *usedBody);
            for(int iDOF = 0; iDOF < (int)_dynamicLimitInfo.vUsedDOFIndices.size(); ++iDOF) {
                vAccelLimits[iDOF] = min(vTmpALim[iDOF], vAccelLimits[iDOF]);
            }
            // check dynamic acceleration limit at t1
            _ComputeDynamicLimits(vTmpALim, x1Vect, v1Vect, *usedBody);
            for(int iDOF = 0; iDOF < (int)_dynamicLimitInfo.vUsedDOFIndices.size(); ++iDOF) {
                vAccelLimits[iDOF] = min(vTmpALim[iDOF], vAccelLimits[iDOF]);
            }
        }
    }

    /// \brief check timebased constraints as initial guess of slowdown loop. see also _ComputeInterpolationWithInitialGuess.
    ///        the purpose of this function is roughly estimate the feasible vellimits and accellimits for the given rampndVect.
    ///        this function does not quit early on violation of some element of rampndVect. this function tries to compute the worst values over all rampndVect to reduce the risk to violate the limits in Check2.
    ///        this function assumes that slowdown along with Check2 is computationally expensive and roughly checking the whole rampndVect might be slightly efficient.
    ///        this function handles the mostly same time-based conditions checked in Check2. but, this function considers these in dedicated ways to compute initial guess. the method to reduce limits is different for the types of constraints.
    ///
    /// \param[in/out] vellimits, accellimits : when input, should contain initial values. when output, resultant limits are computed in this function. size and order are same as _parameters's config.
    /// \param[in] v0Vect, v1Vect : velocities at t0 and t1 (boundary condition)
    /// \param[in] rampndVect : vector of rampnd to check
    /// \param[in] usedBody : if not null pointer, enable dynamics limits computation.
    /// \param[in] fReductionRatio : ratio of how much computed reduction factor is adopted. should be in (0.0, 1.0]. used to interpolate between the computed reduction factor and max of it, e.g. 1.0. if fReductionRatio is smaller, do not slowdown vellimits and accellimits that much. if larger, aggressively slowdonw these limits.
    RampOptimizer::CheckReturn _CheckTimeBasedConstraintsForInitialSlowDown(std::vector<dReal>& vellimits, std::vector<dReal>& accellimits,
                                                                            const std::vector<dReal>& v0Vect, const std::vector<dReal>& v1Vect,
                                                                            const std::vector<RampOptimizer::RampND>& rampndVect,
                                                                            const KinBodyConstPtr usedBody,
                                                                            const dReal fReductionRatio)
    {
        RampOptimizer::CheckReturn retcheck(0);
        if( rampndVect.size() == 0 ) {
            return retcheck;
        }

        const dReal fiSearchVelAccelMult = 1/_parameters->fSearchVelAccelMult; // inverse of fSearchVelAccelMult. multiplied values should be clamped by 1.0 later on. see also the documentation of same parameter in _Shortcut function. 1) larger fSearchVelAccelMult -> mostly, more conservative and less chance to violate the true limits checked in Check2 function (e.g. less time consuming). 2) smaller fSearchVelAccelMult -> mostly, more aggressive and more chance to violate the true limits checked in Check2 function (e.g. more time consuming).
        const dReal nDOF = vellimits.size();

        // original limits before this function is called. the rampndVect is computed based on these original limits and we should scaled down these.
        std::vector<dReal>& vOrgVelLimits = _vCacheOrgVelLimits, vOrgAccelLimits = _vCacheOrgAccelLimits;
        vOrgVelLimits = vellimits;
        vOrgAccelLimits = accellimits;

        // check dynamic limits.
        //   - nature of dynamic limits (for now)
        //     |ddq| <= a(q, dq)
        //     dynamic limits depends on q and dq. once ddq violation is obseved, we can reduce dq or ddq. while reducing of ddq has always solution, dq might not always have solution.
        //   - slowdown method
        //     scale down only the joints which violate the limits. because this function is just an estimation, if GetAVec is not violating the dynamic limit while accellimits violates the dynamic limit, do not clamp the limit for now not to avoid from making the limit too conservative.
        //     different from main slowdown loop, do not scale down the vel limits for now, since reducing of dq might not always have solution.
        //     for now, do not multiply fiSearchVelAccelMult nor fReductionRatio. compared with manip accel/speed, dynamic limits are closed-form and less inaccurate.
        bool bHasDynamicLimitViolation = false; // true if dynamic limit violation
        if( _dynamicLimitInfo.bUseDynamicLimits && !!usedBody ) {
            std::vector<dReal>& vTmpX = _dynamicLimitInfo.vCacheTmpX, vTmpV = _dynamicLimitInfo.vCacheTmpV, vTmpA = _dynamicLimitInfo.vCacheTmpA, vTmpALim = _dynamicLimitInfo.vCacheTmpALim;
            const KinBody& usedBodyRef = *usedBody;
            for(int iRamp = 0; iRamp < (int)rampndVect.size(); ++iRamp) {
                const RampOptimizer::RampND& tmpRamp = rampndVect[iRamp];
                tmpRamp.GetAVect(vTmpA);
                if( iRamp == 0 ) { // compute only iRamp==0. iRamp>0, skip updating of dynamic limits, since we can assume that consecutive ramps have continuous boundary condition. e.g. iRamp-1's X1 and V1 == iRamp's X0 and V0.
                    tmpRamp.GetX0Vect(vTmpX);
                    tmpRamp.GetV0Vect(vTmpV);
                    _ComputeDynamicLimits(vTmpALim, vTmpX, vTmpV, usedBodyRef);
                }
                for(int iDOF = 0; iDOF < nDOF; ++iDOF) {
                    const dReal fAccLim = vTmpALim[iDOF];
                    const dReal fCurAcc = RaveFabs(vTmpA[iDOF]);
                    if( fCurAcc > fAccLim ) {
                        //accellimits[iDOF] = min(accellimits[iDOF], accellimits[iDOF]*fAccLim/fCurAcc*fiSearchVelAccelMult);
                        accellimits[iDOF] = min(accellimits[iDOF], fAccLim);
                        bHasDynamicLimitViolation = true;
                    }
                }
                tmpRamp.GetX1Vect(vTmpX);
                tmpRamp.GetV1Vect(vTmpV);
                _ComputeDynamicLimits(vTmpALim, vTmpX, vTmpV, usedBodyRef);
                for(int iDOF = 0; iDOF < nDOF; ++iDOF) {
                    const dReal fAccLim = vTmpALim[iDOF];
                    const dReal fCurAcc = RaveFabs(vTmpA[iDOF]);
                    if( fCurAcc > fAccLim ) {
                        //accellimits[iDOF] = min(accellimits[iDOF], accellimits[iDOF]*fAccLim/fCurAcc*fiSearchVelAccelMult);
                        accellimits[iDOF] = min(accellimits[iDOF], fAccLim);
                        bHasDynamicLimitViolation = true;
                    }
                }
            }
        }

        // check manip accel/speed
        //   - nature of manip accel/speed constraints
        //     |J(q) dq|^2 <= |dr+|^2
        //     |J(q) ddq + dJ(q, dq) dq|^2 <= |ddr+|^2
        //     depending on q, dq, and ddq. once ddq violation is obseved, we can reduce dq and/or ddq. reducing of only dq or reducing of only ddq might not always have solution. reducing of both dq and ddq always has solution. this relationship is case-by-case
        //   - how reducing affects (example of manip accel)
        //     for example, let violation ratio ka = |ddr|/|ddr+|, kv=ka^{1/2}
        //     if we scale by ka and kv, e.g. use ka*ddq and kv*dq, we get:
        //       |J(q) (ka*ddq) + dJ(q, (kv*dq)) (kv*dq)|^2 ~= (|ka|*|J(q) ddq + dJ(q, dq) dq|)^2  =  (|ka|*|ddr|)^2
        //     and
        //       (|ka|*|ddr|)^2 <= |ddr+|^2
        //     but, we might have larger scaling mult because:
        //       - in the smoother's algorithm, we don't scale ddq and dq themselves, but we scale the limits, ddq+ and dq+. ddq and dq are the results from ramp creation based on newly scaled ddq+ and dq+. thus, the relationship is not straightforward.
        //       - due to the newly computed ramp, the profiles of q, dq, and ddq might be different.
        //     thus,
        //       - if we scale down ka and kv, we might get conservative ramps.
        //       - if we gradually scale down with using larger mults than ka and kv, we might get more aggressive ramps
        //   - slowdown method
        //     scale down the vel and acc limits, mostly in the same manner of main slowdown loop.
        //     this function is called from _ComputeInterpolationWithInitialGuess, and we iterate slowdown loop several times by using fReductionRatio and fiSearchVelAccelMult to get more aggressive ramp.
        //     note : different from the old initial guess computation by GetMaxVelocitiesAccelerations, 1) use same computation as later slowdown iteration. 2) scale down vellimits as well, since scaling down of the acc limits might not be enough to scale down the max manip accel theoretically. 3) slowdown by GetMaxVelocitiesAccelerations is bit inaccurate than that by CheckManipConstraints2. in addition, the behavior looks slightly less deterministic (sometimes return larger limits, but sometimes slower limits).
        bool bHasManipViolation = false;
        if( _bmanipconstraints ) {
            try {
                std::vector<RampOptimizer::RampND>& rampndVectTmp = _cacheRampNDVectInitialGuess;
                const int iLastElement = (int)rampndVect.size()-1;
                for(int iRamp = 0; iRamp < (int)rampndVect.size(); ++iRamp) {
                    rampndVectTmp.clear();
                    rampndVectTmp.push_back(rampndVect[iRamp]);
#ifdef SMOOTHER2_TIMING_DEBUG
                    _nCallsCheckManip += 1;
                    _tStartCheckManip = utils::GetMicroTime();
#endif
                    retcheck = _manipconstraintchecker->CheckManipConstraints2(rampndVectTmp, IT_Closed, _bUseNewHeuristic);
#ifdef SMOOTHER2_TIMING_DEBUG
                    _tEndCheckManip = utils::GetMicroTime();
                    _totalTimeCheckManip += 0.000001f*(float)(_tEndCheckManip - _tStartCheckManip);
#endif
                    if( retcheck.retcode != 0 ) {
#ifdef SMOOTHER2_PROGRESS_DEBUG
                        RAVELOG_VERBOSE_FORMAT("env=%d, CheckManipConstraints2 returns retcode=0x%x", _environmentid%retcheck.retcode);
#endif
                        if( retcheck.retcode == CFO_CheckTimeBasedConstraints ) {
                            // if CFO_CheckTimeBasedConstraints, process
                            bHasManipViolation = true;
                            if( _parameters->maxmanipaccel > 0 && _parameters->maxmanipaccel < retcheck.fMaxManipAccel ) {
                                // reduce accel limits
                                const dReal fAccMult = min(retcheck.fTimeBasedSurpassMult*retcheck.fTimeBasedSurpassMult * fiSearchVelAccelMult, 1.0)*fReductionRatio + (1-fReductionRatio);
                                for(int iDOF = 0; iDOF < nDOF; ++iDOF) {
                                    accellimits[iDOF] = min(accellimits[iDOF], vOrgAccelLimits[iDOF]*fAccMult);
                                }
                                // since manip accel ddr is related to joint vel dq, reduce vel limits
                                if( iRamp != 0 && iRamp != iLastElement ) {
                                    // note that we cannot reduce vellimits at t0 and t1, since velocities of t0 and t1 are boundary condition. so, for t0 or t1, skip reducing the limits.
                                    // iRamp=0's end condition and iRamp=iLastElement's start condition can have velocity reducing. but, somehow, if we reduce them, got slightly worse computation. so, here we simply skip iRamp=0 and iRamp=iLastElement.
                                    const dReal fVelMult = min(retcheck.fTimeBasedSurpassMult * fiSearchVelAccelMult, 1.0)*fReductionRatio + (1-fReductionRatio);
                                    for(int iDOF = 0; iDOF < nDOF; ++iDOF) {
                                        const dReal fMinVel = max(RaveFabs(v0Vect[iDOF]), RaveFabs(v1Vect[iDOF]));
                                        vellimits[iDOF] = max(fMinVel, vOrgVelLimits[iDOF]*fVelMult);
                                    }
                                }
                            }
                            else if( _parameters->maxmanipspeed > 0 && _parameters->maxmanipspeed < retcheck.fMaxManipSpeed ) {
                                // reduce vel limits.
                                if( iRamp != 0 && iRamp != iLastElement ) {
                                    // note that we cannot reduce vellimits at t0 and t1, since velocities of t0 and t1 are boundary condition. so, for t0 or t1, skip reducing the limits.
                                    // iRamp=0's end condition and iRamp=iLastElement's start condition can have velocity reducing. but, somehow, if we reduce them, got slightly worse computation. so, here we simply skip iRamp=0 and iRamp=iLastElement.
                                    const dReal fVelMult = min(retcheck.fTimeBasedSurpassMult * fiSearchVelAccelMult, 1.0)*fReductionRatio + (1-fReductionRatio);
                                    for(int iDOF = 0; iDOF < nDOF; ++iDOF) {
                                        const dReal fMinVel = max(RaveFabs(v0Vect[iDOF]), RaveFabs(v1Vect[iDOF]));
                                        vellimits[iDOF] = max(fMinVel, vOrgVelLimits[iDOF]*fVelMult);
                                    }
                                }
                            }
                        }
                        else {
                            // if non CFO_CheckTimeBasedConstraints error, return immediately
                            return retcheck;
                        }
                    }
                }
            }
            catch (const std::exception& ex) {
                RAVELOG_VERBOSE_FORMAT("env=%d, CheckManipConstraints2 threw an exception: %s", _environmentid%ex.what());
                return RampOptimizer::CheckReturn(0xffff|CFO_FromTrajectorySmoother);
            }
        }

        // set retcheck
        if( (bHasDynamicLimitViolation || bHasManipViolation) && (retcheck.retcode == 0) ) {
            retcheck.retcode = CFO_CheckTimeBasedConstraints;
        }
        return retcheck;
    }

    /// \brief compute vel/accel mults, which are ratios aginst the max config limits. note that config limits should not be zero.
    /// \param[out] fVelMult, fAccelMult : mult i n[0.0, 1.0]. if there are several values among different DOFs, take min, e.g. worst.
    /// \param[in] vVelLimits, vAccelLimits : vel and accel limits. same size and order as config limits.
    void _ComputeVelAccelMults(dReal& fVelMult, dReal& fAccelMult,
                               const std::vector<dReal>& vVelLimits, const std::vector<dReal>& vAccelLimits) const
    {
        fVelMult = 1.0;
        fAccelMult = 1.0;
        for(int iDOF = 0; iDOF < (int)vVelLimits.size(); ++iDOF) {
            fVelMult = min(vVelLimits[iDOF]/_parameters->_vConfigVelocityLimit[iDOF], fVelMult);
            fAccelMult = min(vAccelLimits[iDOF]/_parameters->_vConfigAccelerationLimit[iDOF], fAccelMult);
        }
    }

    /// \brief compute interpolation while computing the initial guess of vellimits and accellimits.
    ///        expected to be called at the first slowdown iteration (e.g. iSlowDown=0) inside of _MergeConsecutiveSegments and _Shortcut.
    ///        this function tries interpolation and then rough limit checking to reduce the vellimits and accellimits so that computed shortcutRampNDVect respects vellimits and accellimits.
    ///
    ///        caller's main slowdown loop can check the constraints via Check2 function. Check2 function is different from this function in the following aspects:
    ///          - Check2 checks the constrints more in detail. (for example, sampled points in CheckPathAllConstraints)
    ///          - CheckPathAllConstraints can modify the ramp. the modified ramp might be different from shortcutRampNDVect in this function. thus, checking based on shortcutRampNDVect is not perfet.
    ///          - Slowdown logic and limit reduction in the main slowdown loop of the caller is slightly different from that in _CheckTimeBasedConstraintsForInitialSlowDown. _CheckTimeBasedConstraintsForInitialSlowDown is customized for initial guess computation.
    ///
    ///        the motivation to compute initial guess is:
    ///          - in Check2 function, limits are checked along with collision checking. collision checking would be computationally expensive. it would be preferable to slowdown the ramps initially before Check2 function. especially, there would be more chance to reject the ramp which causes too long duration before Check2.
    ///          - due to various reasons (for example, ramp modification by CheckPathAllConstraints), checking based on shortcutRampNDVect is not perfet. 1) but, even if there is such ramp modification, this function is supposed to give the good initial guess. 2) if there is no such ramp modification, this function gives the constraint-respecting ramps.
    ///          - because we can roughly slowdown the ramp before Check2, we can spend computation time to get slightly better limits to respect the constraints. thus, more chance to get better duration and better shortcut results.
    ///
    ///        current time-based constraints:
    ///          - dynamic acc limits
    ///            |ddq| <= a(q, dq)
    ///          - manip speed limits
    ///            |J(q) dq|^2 <= |dr+|^2
    ///          - manip accel limits
    ///            |J(q) ddq + dJ(q, dq) dq|^2 <= |ddr+|^2
    ///          - toruqe limits: TODO : torque limits are not fully supported in this function yet!!
    ///            tq- <= M(q)ddq + C(q, dq) <= tq+
    ///
    ///        internal loop
    ///          - different from the caller's main loop, this function has internal loop to gradually reduce the limits, instead of reducing the limit in one iteration.
    ///          - in the above constraints, dynamic acc limits can be written closed-form inequalities of ddq. however, not for other limits right now. in addition, we need to re-compute the ramps once limits are reduced. thus, reducing the limits gradually can lead more chance to get better solutions. see also _CheckTimeBasedConstraintsForInitialSlowDown in detail.
    ///
    ///        note that internally uses debugging member variables (_vShortcutStats and _ssshortcutprogress) if SMOOTHER2_PROGRESS_DEBUG is enabled in _ComputeInterpolation.
    ///
    /// \param[out] shortcutRampNDVect : vector of ramps to contain resultant interpolation result.
    /// \param[out] segmentTime : resultant segment time
    /// \param[out] vellimits, accellimits : caller should give vel/accel limits first. this function tries to slow down these limits in this function. vellimits and accellimits contain the resultant limits as initial guess.
    /// \param[out] bSlowedDown : true if slowdown occurred
    /// \param[out] fVelMult, fAccelMult : mults against the config limits in [0.0, 1.0]. should not be used for further limit computation. only for tracking breaking condition from the main slowdown loop to detect too much slowdown. only computed when returning true.
    /// \param[out] iIterProgress : for debugging purpose
    /// \param[in] t0, t1, minTimeStep : time values used in shortcut iteration.
    /// \param[in] x0Vect, x1Vect, v0Vect, v1Vect : positions and velocities at t0 and t1 (boundary conditions). used for interpolation.
    /// \return true if there is not apparent failure, e.g. there is no interpolation nor duration failure of _ComputeInterpolation.
    bool _ComputeInterpolationWithInitialGuess(std::vector<RampOptimizer::RampND>& shortcutRampNDVect, dReal& segmentTime,
                                               std::vector<dReal>& vellimits, std::vector<dReal>& accellimits,
                                               bool& bSlowedDown, dReal& fVelMult, dReal& fAccelMult, uint32_t& iIterProgress,
                                               const dReal t0, const dReal t1, const dReal minTimeStep,
                                               const std::vector<dReal>& x0Vect, const std::vector<dReal>& x1Vect, const std::vector<dReal>& v0Vect, const std::vector<dReal>& v1Vect,
                                               const size_t iSlowDown, const int iters, const int numIters,
                                               const KinBodyConstPtr usedBody,
                                               const int nMaxInitialGuessSlowDown = 3)
    {
        bSlowedDown = false;
        const int iLastInitialGuessSlowDown = nMaxInitialGuessSlowDown-1;
        for(int iInitialGuessSlowDown = 0; iInitialGuessSlowDown < nMaxInitialGuessSlowDown; ++iInitialGuessSlowDown) {
            if( !_ComputeInterpolation(shortcutRampNDVect, segmentTime, iIterProgress,
                                       t0, t1, minTimeStep, x0Vect, x1Vect, v0Vect, v1Vect, vellimits, accellimits, iSlowDown, iters, numIters) ) {
                return false;
            }
            const dReal fReductionRatio = (iInitialGuessSlowDown == iLastInitialGuessSlowDown) ? 1.0 : 0.5; // if the last loop, use the computed limits (e.g. 1.0). otherwise, use slightly larger limits (e.g. 0.5) to compute like bisection method.
            RampOptimizer::CheckReturn retcheck = _CheckTimeBasedConstraintsForInitialSlowDown(vellimits, accellimits, v0Vect, v1Vect, shortcutRampNDVect, usedBody, fReductionRatio);
            if( retcheck.retcode == 0 ) {
                // if initial guess check passed, return true. note that this does not guarantee to pass Check2 later on, because checked ramp and conditions are slightly different.
                _ComputeVelAccelMults(fVelMult, fAccelMult, vellimits, accellimits);
                return true;
            }
            else if( retcheck.retcode != CFO_CheckTimeBasedConstraints ) {
                // if failed but not coming from CFO_CheckTimeBasedConstraints, we cannot continue slowdown loop and return immediately.
                return false;
            }
            bSlowedDown = true;
        }

        // based on the latest vellimits and accellimits, compute the interpolation for caller's slowdown loop.
        if( !_ComputeInterpolation(shortcutRampNDVect, segmentTime, iIterProgress,
                                   t0, t1, minTimeStep, x0Vect, x1Vect, v0Vect, v1Vect, vellimits, accellimits, iSlowDown, iters, numIters) ) {
            return false;
        }
        // even if initial guess check didn't pass, return true. reaching here does not mean failing of the main slowdown loop, thus no need to reject such ramp and limits and Check2 function can handle the ramp and limits later on. the reasons are:
        // - failing of _CheckTimeBasedConstraintsForInitialSlowDown does not mean failing of Check2 in the main slowdown loop later on.
        // - in the last iteration, we might have already reached to the limit respecting ramp. but the after the last iteration in the above, we don't check the limits and we don't know it's successful or not.
        _ComputeVelAccelMults(fVelMult, fAccelMult, vellimits, accellimits);
        return true;
    }

    /// Members
    int _environmentid;
    ConstraintTrajectoryTimingParametersPtr _parameters;
    SpaceSamplerBasePtr _uniformsampler;        ///< used for planning, seed is controlled
    ConstraintFilterReturnPtr _constraintreturn;
    MyRampNDFeasibilityChecker _feasibilitychecker;
    boost::shared_ptr<ManipConstraintChecker2> _manipconstraintchecker;
    TrajectoryBasePtr _pdummytraj;
    PlannerProgress _progress;
    bool _bUsePerturbation;
    bool _bmanipconstraints;
    std::vector<ZeroVelPointInfo> _vZeroVelPointInfos; ///< keeps track of all zero-velocity points so that we can focus on shortcutting around these points.
    RampOptimizer::ParabolicInterpolator _interpolator;
    dReal _maxInitialRampTime; ///< max duration of traj segment between two consecutive waypoints
                               /// after calling _SetMileStones. this serves as a cap for how far a
                               /// pair of sampled time instants t0, t1 can be.
    uint32_t _basetime; ///< timestamp at the beginning of PlanPath. used for checking computation time.

    // for logging
    SpaceSamplerBasePtr _logginguniformsampler; ///< used for logging, seed is randomly set
    uint32_t _fileIndexMod; ///< maximum number of trajectory index allowed when saving
    uint32_t _fileIndex; ///< file index used for saved trajectory naming.
    DebugLevel _dumplevel;  ///< minimum debug level which triggers trajectory saving
#ifdef SMOOTHER2_PROGRESS_DEBUG
    std::vector<int> _vShortcutStats; ///< keeps track of the number of times a shortcut iter finishes with each ShortcutStatus
    std::stringstream _ssshortcutprogress; ///< keep track of shortcutprogress as stringstream. only for debuggin purpose
#endif

    /// Caching stuff
    RampOptimizer::ParabolicPath _cacheparabolicpath, _cacheparabolicpath2;
    std::vector<dReal> _cacheWaypoints; ///< stores concatenated waypoints obtained from the input trajectory
    std::vector<std::vector<dReal> > _cacheWaypointVect; ///< each element is a vector storing a waypoint
    std::vector<dReal> _cacheX0Vect, _cacheX1Vect, _cacheV0Vect, _cacheV1Vect, _cacheTVect; ///< used in PlanPath and _Shortcut
    std::vector<dReal> _cacheTempX0Vect, _cacheTempV0Vect; ///< used in _Shortcut
    RampOptimizer::RampND _cacheRampND, _cacheFrontTrimRampND, _cacheBackTrimRampND;
    std::vector<RampOptimizer::RampND> _cacheRampNDVect; ///< use cases: 1. being passed to _ComputeRampWithZeroVelEndpoints when retrieving cubic waypoints from input traj
                                                         ///             2. in _SetMileStones: being passed to _ComputeRampWithZeroVelEndpoints
                                                         ///             3. handles the finalized set of RampNDs before writing to OpenRAVE trajectory
                                                         ///             4. in _Shortcut
    std::vector<RampOptimizer::RampND> _cacheRampNDVectOut; ///< used to handle output from Check function in PlanPath, also used in _Shortcut

    // in SegmentFeasible2
    std::vector<dReal> _cacheCurPos, _cacheNewPos, _cacheCurVel, _cacheNewVel;
    RampOptimizer::RampND _cacheRampNDSeg;

    // in _SetMileStones
    std::vector<std::vector<dReal> > _cacheNewWaypointsVect;

    // in _ComputeRampWithZeroVelEndpoints
    std::vector<dReal> _cacheX0Vect1, _cacheX1Vect1; ///< need to have another copies of x0 and x1 vectors. For v0 and v1 vectors, we can reuse to ones above.
    std::vector<dReal> _cacheVellimits, _cacheAccelLimits; ///< stores current velocity and acceleration limits, also used in _Shortcut
    std::vector<RampOptimizer::RampND> _cacheRampNDVectOut1; ///< stores output from the check function, also used in _Shortcut
    std::vector<RampOptimizer::RampND> _cacheRampNDVectInitialGuess; ///< cached vector used for initial guess computation

    // in _Shortcut
    std::vector<uint8_t> _vVisitedDiscretizationCache;

#ifdef SMOOTHER2_TIMING_DEBUG
    // Statistics
    uint32_t _tShortcutStart, _tShortcutEnd;
    int _numShortcutIters;

    size_t _nCallsCheckManip;
    dReal _totalTimeCheckManip;
    uint32_t _tStartCheckManip, _tEndCheckManip;

    size_t _nCallsInterpolator;
    dReal _totalTimeInterpolator;
    uint32_t _tStartInterpolator, _tEndInterpolator;

    size_t _nCallsCheckPathAllConstraints, _nCallsCheckPathAllConstraintsInVain;
    dReal _totalTimeCheckPathAllConstraints, _totalTimeCheckPathAllConstraintsInVain;
    // variables with suffix _SegmentFeasible2 are for measurement inside ConfigFeasible2 and
    // SegmentFeasible2. will be reset every time after a call to Check2 (because it is after a call
    // to Check2 we will know if this amount of calls to CheckPathAllConstriants are beneficial)
    size_t _nCallsCheckPathAllConstraints_SegmentFeasible2;
    dReal _totalTimeCheckPathAllConstraints_SegmentFeasible2;
    uint32_t _tStartCheckPathAllConstraints, _tEndCheckPathAllConstraints;
#endif

    bool _bUseNewHeuristic;

    std::stringstream _sslog; // for logging purpose

    /// \brief info to use dynamic limits.
    struct DynamicLimitInfo
    {
        /// \brief reset
        void Reset()
        {
            bUseDynamicLimits = false;
            vUsedDOFIndices.clear();
            bodyName.clear();
        };

        std::vector<int> vUsedDOFIndices; ///< used openrave dof indices
        std::string bodyName; ///< used body name in _parameters->_configurationspecification
        bool bUseDynamicLimits; ///< true if use dynamic limits.
        dReal fDynamicAccelerationLimitMult; ///< additional mult for dynamic acceleration limits. smoother should respect [original dynamic acceleration limit]x[fDynamicAccelerationLimitMult].
        std::vector<dReal> vCacheFullDOFPositions, vCacheFullDOFVelocities, vCacheFullDOFAccelerationLimits, vCacheFullDOFJerkLimits; ///< cached vectors for dynamic limits. openrave fulldof kinematics order and size is GetDOF.
        std::vector<dReal> vCacheTmpX, vCacheTmpV, vCacheTmpA, vCacheTmpALim; ///< cached vectors for _CheckTimeBasedConstraintsForInitialSlowDown, ...etc. order and size are same as _parameters->_configurationspecification.
    };
    DynamicLimitInfo _dynamicLimitInfo; ///< info to use dynamic limits.
    std::vector<dReal> _vCacheOrgAccelLimits, _vCacheOrgVelLimits; ///< cached vectors for _CheckTimeBasedConstraintsForInitialSlowDown, ...etc. order and size are same as _parameters->_configurationspecification.

}; // end class ParabolicSmoother2

PlannerBasePtr CreateParabolicSmoother2(EnvironmentBasePtr penv, std::istream& sinput)
{
    return PlannerBasePtr(new ParabolicSmoother2(penv, sinput));
}

} // end namespace rplanners

#ifdef RAVE_REGISTER_BOOST
#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()
BOOST_TYPEOF_REGISTER_TYPE(RampOptimizer::Ramp)
BOOST_TYPEOF_REGISTER_TYPE(RampOptimizer::ParabolicCurve)
BOOST_TYPEOF_REGISTER_TYPE(RampOptimizer::RampND)
BOOST_TYPEOF_REGISTER_TYPE(RampOptimizer::ParabolicPath)
#endif
