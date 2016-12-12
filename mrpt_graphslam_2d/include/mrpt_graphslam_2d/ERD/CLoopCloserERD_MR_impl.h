/* +---------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)               |
   |                          http://www.mrpt.org/                             |
   |                                                                           |
   | Copyright (c) 2005-2016, Individual contributors, see AUTHORS file        |
   | See: http://www.mrpt.org/Authors - All rights reserved.                   |
   | Released under BSD License. See details in http://www.mrpt.org/License    |
   +---------------------------------------------------------------------------+ */

#ifndef CLOOPCLOSERERD_MR_IMPL_H
#define CLOOPCLOSERERD_MR_IMPL_H

namespace mrpt { namespace graphslam { namespace deciders {

// Ctors, Dtors
template<class GRAPH_t>
CLoopCloserERD_MR<GRAPH_t>::CLoopCloserERD_MR() {
	// CLoopCloser Ctor is automatically called.

	this->is_mr_slam_class = true;
	this->initializeLoggers("CLoopCloserERD_MR");
}

template<class GRAPH_t>
CLoopCloserERD_MR<GRAPH_t>::~CLoopCloserERD_MR() {
	// CLoopCloser Dtor is automatically called.
}

// Member methods implementations
template<class GRAPH_t>
bool CLoopCloserERD_MR<GRAPH_t>::updateState(
		mrpt::obs::CActionCollectionPtr action,
		mrpt::obs::CSensoryFramePtr observations,
		mrpt::obs::CObservationPtr observation ) {

	parent::updateState(action, observations, observation);

	// search for possible edges with the other agent's graph.
	// TODO

}

} } } // end of namespaces

#endif /* end of include guard: CLOOPCLOSERERD_MR_IMPL_H */

