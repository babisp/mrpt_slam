/* +---------------------------------------------------------------------------+
	 |                     Mobile Robot Programming Toolkit (MRPT)               |
	 |                          http://www.mrpt.org/                             |
	 |                                                                           | | Copyright (c) 2005-2016, Individual contributors, see AUTHORS file        |
	 | See: http://www.mrpt.org/Authors - All rights reserved.                   |
	 | Released under BSD License. See details in http://www.mrpt.org/License    |
	 +---------------------------------------------------------------------------+ */

#ifndef CGRAPHSLAMENGINE_MR_IMPL_H
#define CGRAPHSLAMENGINE_MR_IMPL_H

namespace mrpt { namespace graphslam {

template<class GRAPH_T>
CGraphSlamEngine_MR<GRAPH_T>::CGraphSlamEngine_MR(
		ros::NodeHandle* nh,
		const std::string& config_file,
		const std::string& rawlog_fname/* ="" */,
		const std::string& fname_GT /* ="" */,
		mrpt::graphslam::CWindowManager* win_manager /* = NULL */,
		mrpt::graphslam::deciders::CNodeRegistrationDecider<GRAPH_T>* node_reg /* = NULL */,
		mrpt::graphslam::deciders::CEdgeRegistrationDecider<GRAPH_T>* edge_reg /* = NULL */,
		mrpt::graphslam::optimizers::CGraphSlamOptimizer<GRAPH_T>* optimizer /* = NULL */):
	parent_t::CGraphSlamEngine_ROS(
			nh,
			config_file,
			rawlog_fname,
			fname_GT,
			win_manager,
			node_reg,
			edge_reg,
			optimizer),
	m_conn_manager(
			dynamic_cast<mrpt::utils::COutputLogger*>(this), nh),
	m_nh(nh),
	m_graph_nodes_last_size(0),
	m_registered_multiple_nodes(false),
	m_intra_group_node_count_thresh_minadv(25),
	cm_graph_async_spinner(/* threads_num: */ 1, &this->custom_service_queue),
	m_pause_exec_on_mr_registration(false)
{
	this->initClass();
}

template<class GRAPH_T>
CGraphSlamEngine_MR<GRAPH_T>::~CGraphSlamEngine_MR() {
	MRPT_LOG_DEBUG_STREAM <<
		"In Destructor: Deleting CGraphSlamEngine_MR instance...";
	cm_graph_async_spinner.stop();

	for (typename neighbors_t::iterator
			neighbors_it = m_neighbors.begin();
			neighbors_it != m_neighbors.end();
			++neighbors_it) {
		delete *neighbors_it;
	}

}

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::addNodeBatchesFromAllNeighbors() {
	MRPT_START;
	using namespace mrpt::graphslam::detail;

	bool ret_val = false;
	for (const auto& neighbor : m_neighbors) {
		if (!isEssentiallyZero(
					neighbor->tf_self_to_neighbor_first_integrated_pose) && // intra-graph TF found
				neighbor->hasNewData() &&
				neighbor->hasNewNodesBatch(m_nodes_integration_batch_size)) {
			bool loc_ret_val = this->addNodeBatchFromNeighbor(neighbor);
			if (loc_ret_val) { ret_val = true; }
		}
	}

	return ret_val;
	MRPT_END;
} // end of addNodeBatchesFromAllNeighbors

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::
addNodeBatchFromNeighbor(TNeighborAgentProps* neighbor) {
	MRPT_START;
	ASSERT_(neighbor);
	using namespace mrpt::utils;
	using namespace mrpt::poses;
	using namespace mrpt::math;
	using namespace std;


	vector_uint nodeIDs;
	std::map<mrpt::utils::TNodeID, node_props_t> nodes_params;
	neighbor->getCachedNodes(&nodeIDs, &nodes_params, /*only unused=*/ true);

	//
	// get the condensed measurements graph of the new nodeIDs
	//
	mrpt_msgs::GetCMGraph cm_graph_srv;
	//mrpt_msgs::GetCMGraphRequest::_nodeIDs_type& cm_graph_nodes =
	//cm_graph_srv.request.nodeIDs;
	for (const auto n : nodeIDs) {
		cm_graph_srv.request.nodeIDs.push_back(n);
	}

	MRPT_LOG_DEBUG_STREAM << "Asking for the graph.";
	bool res = neighbor->cm_graph_srvclient.call(cm_graph_srv); // blocking
	if (!res) {
		MRPT_LOG_ERROR_STREAM << "Service call for CM_Graph failed.";
		return false; // skip this if failed to fetch other's graph
	}
	MRPT_LOG_DEBUG_STREAM << "Fetched graph successfully.";
	MRPT_LOG_DEBUG_STREAM << cm_graph_srv.response.cm_graph;

	GRAPH_T other_graph;
	mrpt_bridge::convert(cm_graph_srv.response.cm_graph, other_graph);

	//
	// merge batch of nodes in own graph
	//
	MRPT_LOG_WARN_STREAM
		<< "Merging new batch from \"" << neighbor->getAgentNs() << "\"..." << endl
		<< "Batch: " << getSTLContainerAsString(cm_graph_srv.request.nodeIDs);
	hypots_t graph_conns;
	// build a hypothesis connecting the new batch with the last integrated
	// pose of the neighbor
	{
		pair<TNodeID, node_props_t> node_props_to_connect = *nodes_params.begin();

		hypot_t graph_conn;
		constraint_t c;
		c.mean = node_props_to_connect.second.pose -
			neighbor->last_integrated_pair_neighbor_frame.second;
		c.cov_inv.unit();
		graph_conn.setEdge(c);

		graph_conn.from = INVALID_NODEID;
		// get the nodeID of the last integrated neighbor node after remapping
		// in own graph numbering
		for (const auto n : this->m_graph.nodes) {
			if (n.second.agent_ID_str == neighbor->getAgentNs() &&
					n.second.nodeID_loc == neighbor->last_integrated_pair_neighbor_frame.first) {
				graph_conn.from = n.first;
				break;
			}
		}
		ASSERT_(graph_conn.from != INVALID_NODEID);
		graph_conn.to = node_props_to_connect.first; // other

		MRPT_LOG_DEBUG_STREAM << "Hypothesis for adding the batch of nodes: " << graph_conn;
		graph_conns.push_back(graph_conn);
	}

	std::map<TNodeID, TNodeID> old_to_new_mappings;
	this->m_graph.mergeGraph(
			other_graph, graph_conns,
			/*hypots_from_other_to_self=*/ false,
			&old_to_new_mappings);

	//
	// Mark Nodes/LaserScans as integrated
	//
	MRPT_LOG_WARN_STREAM << "Marking used nodes as integrated - Integrating LSs";
	nodes_to_scans2D_t new_nodeIDs_to_scans_pairs;
	for (typename vector_uint::const_iterator
			n_cit = nodeIDs.begin();
			n_cit != nodeIDs.end();
			++n_cit) {

		// Mark the neighbor's used nodes as integrated
		neighbor->nodeID_to_is_integrated.at(*n_cit) = true;

		// Integrate LaserScans at the newly integrated nodeIDs in own
		// nodes_to_laser_scans2D map
		new_nodeIDs_to_scans_pairs.insert(make_pair(
					old_to_new_mappings.at(*n_cit),
					nodes_params.at(*n_cit).scan));
		MRPT_LOG_INFO_STREAM << "Adding nodeID-LS of nodeID: "
			<< "\t[old:] " << *n_cit << endl
			<< "| [new:] " << old_to_new_mappings.at(*n_cit);
	}

 	this->m_nodes_to_laser_scans2D.insert(
 			new_nodeIDs_to_scans_pairs.begin(),
 			new_nodeIDs_to_scans_pairs.end());
	edge_reg_mr_t* edge_reg_mr =
		dynamic_cast<edge_reg_mr_t*>(this->m_edge_reg);
 	edge_reg_mr->addBatchOfNodeIDsAndScans(new_nodeIDs_to_scans_pairs);

	this->execDijkstraNodesEstimation();
	neighbor->resetFlags();

	// keep track of the last integrated nodeID
	{
		TNodeID last_node = *nodeIDs.rbegin();
		neighbor->last_integrated_pair_neighbor_frame = make_pair(
				last_node,
				nodes_params.at(last_node).pose);
	}

	if (m_pause_exec_on_mr_registration) this->pauseExec();
	return true;
	MRPT_END;
} // end of addNodeBatchFromNeighbor

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::findTFsWithAllNeighbors() {
	MRPT_START;
	using namespace mrpt::utils;
	using namespace mrpt::math;

	if (this->m_graph.nodeCount() < m_intra_group_node_count_thresh) {
		return false;
	}


	// cache the gridmap so that it is computed only once during all the
	// findMatches*  calls
	this->computeMap();

	bool ret_val = false; // at *any* node registration, change to true

	// iterate over all neighbors - run map-matching proc.
	for (typename neighbors_t::iterator
			neighbors_it = m_neighbors.begin();
			neighbors_it != m_neighbors.end();
			++neighbors_it) {

		// run matching proc with neighbor only if I haven't found an initial
		// intra-graph TF
		if (!m_neighbor_to_found_initial_tf.at(*neighbors_it)) {

			bool loc_ret_val = findTFWithNeighbor(*neighbors_it);
			if (loc_ret_val) {
				MRPT_LOG_DEBUG_STREAM <<
					"Updating own cached map after successful node registration...";
				// update own map if new nodes have been added.
				this->computeMap();

				// mark current neighbor tf as found.
				m_neighbor_to_found_initial_tf.at(*neighbors_it) = true;

				ret_val = true;
			}
		}
	}

	return ret_val;
	MRPT_END;
} // end of findTFsWithAllNeighbors

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::
findTFWithNeighbor(TNeighborAgentProps* neighbor) {
	MRPT_START;
	using namespace mrpt::slam;
	using namespace mrpt::math;
	using namespace mrpt::poses;
	using namespace mrpt::utils;
	using namespace mrpt::maps;
	using namespace mrpt::graphslam::detail;

	bool ret_val = false;

	vector_uint neighbor_nodes;
	std::map<mrpt::utils::TNodeID, node_props_t> nodes_params;
	neighbor->getCachedNodes(&neighbor_nodes, &nodes_params, /*only unused=*/ false);
	if (neighbor_nodes.size() < m_intra_group_node_count_thresh ||
			!neighbor->hasNewData()) {
		return false;
	}

	//
	// run alignment procedure
	//
	COccupancyGridMap2DPtr neighbor_gridmap = neighbor->getGridMap();
	CGridMapAligner gridmap_aligner;
	gridmap_aligner.options = m_alignment_options;

	CGridMapAligner::TReturnInfo results;
	float run_time = 0;
	// TODO - make use of agents' rendez-vous in the initial estimation
	CPosePDFGaussian init_estim;
	init_estim.mean = neighbor->tf_self_to_neighbor_first_integrated_pose;
	this->logFmt(LVL_INFO,
			"Trying to align the maps, initial estimation: %s",
			init_estim.mean.asString().c_str());
	neighbor_gridmap->saveMetricMapRepresentationToFile(this->getLoggerName() + "_other");
	this->m_gridmap_cached->saveMetricMapRepresentationToFile(this->getLoggerName() + "_self");
	const CPosePDFPtr pdf_tmp = gridmap_aligner.AlignPDF(
			this->m_gridmap_cached.pointer(), neighbor_gridmap.pointer(),
			init_estim,
			&run_time, &results);
	this->logFmt(LVL_INFO, "Elapsed Time: %f", run_time);
	CPosePDFSOGPtr pdf_out = CPosePDFSOG::Create();
	pdf_out->copyFrom(*pdf_tmp);

	CPose2D pose_out; CMatrixDouble33 cov_out;
	pdf_out->getMostLikelyCovarianceAndMean(cov_out, pose_out);

	this->logFmt(LVL_INFO, "%s\n",
			getGridMapAlignmentResultsAsString(*pdf_tmp, results).c_str());

	// dismiss this?
	if (results.goodness > 0.999 ||
			isEssentiallyZero(pose_out)) {
		return false;
	}

	//
	//
	// Map-merging operation is successful. Integrate graph into own.
	//////////////////////////////////////////////////////////////////

	//
	// ask for condensed measurements graph
	//
	mrpt_msgs::GetCMGraph cm_graph_srv;
	typedef mrpt_msgs::GetCMGraphRequest::_nodeIDs_type nodeID_type;
	nodeID_type& matched_nodeIDs =
		cm_graph_srv.request.nodeIDs;

	// which nodes to ask the condensed graph for
	// I assume that no nodes of the other graph have been integrated yet.
	for (typename vector_uint::const_iterator
			n_cit = neighbor_nodes.begin();
			n_cit != neighbor_nodes.end();
			++n_cit) {
		matched_nodeIDs.push_back(*n_cit); // all used nodes
	}

	MRPT_LOG_DEBUG_STREAM << "Asking for the graph.";
	bool res = neighbor->cm_graph_srvclient.call(cm_graph_srv); // blocking
	if (!res) {
		MRPT_LOG_ERROR_STREAM << "Service call for CM_Graph failed.";
		return false; // skip this if failed to fetch other's graph
	}
	MRPT_LOG_DEBUG_STREAM << "Fetched graph successfully.";
	MRPT_LOG_DEBUG_STREAM << cm_graph_srv.response.cm_graph;

	GRAPH_T other_graph;
	mrpt_bridge::convert(cm_graph_srv.response.cm_graph, other_graph);

	//
	// merge graphs
	//
	MRPT_LOG_WARN_STREAM << "Merging graph of \"" << neighbor->getAgentNs() << "\"...";
	hypots_t graph_conns;
	// build the only hypothesis connecting graph with neighbor subgraph
	// own origin -> first valid nodeID of neighbor
	{
		hypot_t graph_conn;
		constraint_t c;
		c.mean = pose_out;
		cov_out.inv(c.cov_inv); // assign the inverse of the found covariance to c
		graph_conn.setEdge(c);
		// TODO - change this.
		graph_conn.from = 0; // self
		graph_conn.to = *neighbor_nodes.begin(); // other

		graph_conn.goodness = results.goodness;
		graph_conns.push_back(graph_conn);
	}

	std::map<TNodeID, TNodeID> old_to_new_mappings;
	this->m_graph.mergeGraph(
			other_graph, graph_conns,
			/*hypots_from_other_to_self=*/ false,
			&old_to_new_mappings);

	//
	// Mark Nodes/LaserScans as integrated
	//
	MRPT_LOG_WARN_STREAM << "Marking used nodes as integrated - Integrating LSs";
	nodes_to_scans2D_t new_nodeIDs_to_scans_pairs;
	for (typename vector_uint::const_iterator
			n_cit = neighbor_nodes.begin();
			n_cit != neighbor_nodes.end();
			++n_cit) {

		// Mark the neighbor's used nodes as integrated
		neighbor->nodeID_to_is_integrated.at(*n_cit) = true;

		// Integrate LaserScans at the newly integrated nodeIDs in own
		// nodes_to_laser_scans2D map
		new_nodeIDs_to_scans_pairs.insert(make_pair(
					old_to_new_mappings.at(*n_cit),
					nodes_params.at(*n_cit).scan));
		MRPT_LOG_INFO_STREAM << "Adding nodeID-LS of nodeID: "
			<< "\t[old:] " << *n_cit << endl
			<< "| [new:] " << old_to_new_mappings.at(*n_cit);
	}

 	this->m_nodes_to_laser_scans2D.insert(
 			new_nodeIDs_to_scans_pairs.begin(),
 			new_nodeIDs_to_scans_pairs.end());
	edge_reg_mr_t* edge_reg_mr =
		dynamic_cast<edge_reg_mr_t*>(this->m_edge_reg);
 	edge_reg_mr->addBatchOfNodeIDsAndScans(new_nodeIDs_to_scans_pairs);

	// Call for a Full graph visualization update and Dijkstra update -
	// CGraphSlamOptimizer
	//MRPT_LOG_WARN_STREAM << "Optimizing graph..." << endl;
	//this->m_optimizer->optimizeGraph();
	MRPT_LOG_WARN_STREAM << "Executing Dijkstra..." << endl;
	this->execDijkstraNodesEstimation();

	neighbor->resetFlags();

	// update the initial tf estimation
	neighbor->tf_self_to_neighbor_first_integrated_pose = pose_out;

	// keep track of the last integrated nodeID
	{
		TNodeID last_node = *neighbor_nodes.rbegin();
		neighbor->last_integrated_pair_neighbor_frame = make_pair(
				last_node,
				nodes_params.at(last_node).pose);
	}

	MRPT_LOG_WARN_STREAM << "Nodes of neighbor [" << neighbor->getAgentNs()
		<< "] have been integrated successfully";

	if (m_pause_exec_on_mr_registration) this->pauseExec();
	ret_val = true;

	return ret_val;
	MRPT_END;
} // end if findTFWithNeighbor

/*

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::findMatchesWithNeighbors() {
	MRPT_START;
	using namespace mrpt::utils;
	using namespace mrpt::math;

	bool ret_val = false; // at *any* node registration, change to true

	// we are dealing with a CLoopCloserERD type in the mr-case.
	loop_closer_t* edge_reg =
		dynamic_cast<loop_closer_t*>(this->m_edge_reg);
	ASSERTMSG_(edge_reg,
			"Only the CLoopCloserERD classes and its derivatives can be used.");

	// groupA => own nodes
	// Utilize all own partitions
	// Each partition will be taken as a group in itself => groupA
	partitions_t own_partitions;
	edge_reg->getCurrPartitions(&own_partitions);

	int partition_id = 0;
	for(partitions_t::iterator
			partitions_it = own_partitions.begin();
			partitions_it != own_partitions.end();
			++partitions_it, ++partition_id) {
		MRPT_LOG_DEBUG_STREAM << "Matching proc. for partition ID: " << partition_id;

 		// groupA => own nodes
		vector_uint& groupA = *partitions_it;
		// Avoid multiple integration (propagation) of information - see Lazaro
		// IV.b
		// remove all the nodeIDs that correspond to nodes registered by other
		// agents.
		//MRPT_LOG_WARN_STREAM << "groupA (prior to removal of mr-registered.): "
		//<< getSTLContainerAsString(groupA);
		groupA.erase(
				std::remove_if(
					groupA.begin(), groupA.end(),
					[this](const unsigned int& i){return !this->isOwnNodeID(i);}), groupA.end());
		//MRPT_LOG_WARN_STREAM << "groupA (After to removal of mr-registered.): "
		//<< getSTLContainerAsString(groupA);

		// continue only if the nodes in group exceed the indicated threshold
		if (groupA.size() < m_intra_group_node_count_thresh) { return ret_val; }

		// get the registered nodes of all the other nearby agents
		for (typename neighbors_t::iterator
				neighbors_it = m_neighbors.begin();
				neighbors_it != m_neighbors.end();
				++neighbors_it) {

			TNeighborAgentProps& curr_neigh = **neighbors_it;

			// groupB => neighbor's cached registered nodes
			// Ask only of the nodes that haven't been already integrated in the
			// graph
			vector_uint groupB;

			// build the additional parameters for passing poses, scans of groupB
			typename loop_closer_t::TGenerateHypotsPoolAdParams gen_hypots_ad_params;
			// TODO - bug here.
			MRPT_LOG_DEBUG_STREAM << "Fetching list of cached nodes of TNeighborAgent: "
				<< curr_neigh.getAgentNs();
			curr_neigh.getCachedNodes(
					&groupB,
					&gen_hypots_ad_params.groupB_params,
					true);
			MRPT_LOG_DEBUG_STREAM << "Fetched list.";

			if (groupB.size() < m_intra_group_node_count_thresh ||
					!curr_neigh.hasNewData()) { continue; }

			//
			// run matching proc between groupA and groupB
			//
			MRPT_LOG_WARN_STREAM << "Running matching proc...";
			hypotsp_t hypots_pool;
			edge_reg->generateHypotsPool(
					groupA, groupB, &hypots_pool, &gen_hypots_ad_params);
			MRPT_LOG_WARN_STREAM << "Generated Hypotheses pool successfully ==> Hypotheses #"
			  << hypots_pool.size();

			// Generating Consistencies Matrix
			// fill the vector of optimal paths for all combination of nodes in groupB.
			paths_t groupB_opt_paths;
			MRPT_LOG_WARN_STREAM << "Filling optimal paths for groupB";
			curr_neigh.fillOptPaths(
					set<TNodeID>(groupB.begin(), groupB.end()), &groupB_opt_paths);
			MRPT_LOG_WARN_STREAM << "Filled optimal paths for groupB";

			// generate Pair-wise consistencies.
			MRPT_LOG_WARN_STREAM << "Calling generatePWConsistenciesMatrix";
			mrpt::math::CMatrixDouble consist_matrix(
					hypots_pool.size(), hypots_pool.size());
			edge_reg->generatePWConsistenciesMatrix(
					groupA, groupB, hypots_pool,
					&consist_matrix,
					NULL,
					&groupB_opt_paths);
			MRPT_LOG_WARN_STREAM << "Returned from generatePWConsistenciesMatrix";

			// evaluate the Pair-wise consistencies
			MRPT_LOG_WARN_STREAM << "Calling evalPWConsistenciesMatrix";
			hypotsp_t valid_hypots;
			edge_reg->evalPWConsistenciesMatrix(
					consist_matrix,
					hypots_pool,
					&valid_hypots);
			MRPT_LOG_WARN_STREAM << "Returned from evalPWConsistenciesMatrix";


			MRPT_LOG_WARN_STREAM << "Evaluated groups ==> Valid Hypotheses #"
			    << valid_hypots.size();

			// continue only if matches are found.
			if (!valid_hypots.size() || valid_hypots.size() < m_valid_hypotheses_min_thresh) {
				continue;
			}

			// put constraints on the goodness values as well
			// TODO

			MRPT_LOG_ERROR_STREAM << "Matches have been found!";

			// TODO - make sure X time has passed after the previous successful
			// registration with current neighbor

			//
			// Ask for the condensed graph of the matched nodes
			//
			mrpt_msgs::GetCMGraph cm_graph_srv;
			typedef mrpt_msgs::GetCMGraphRequest::_nodeIDs_type nodeID_type;
			nodeID_type& matched_nodeIDs =
				cm_graph_srv.request.nodeIDs;

			for (typename hypotsp_t::const_iterator
					h_cit = valid_hypots.begin();
					h_cit != valid_hypots.end();
					++h_cit) {
				// by default nodeIDs of groupB are the source of each hypothesis.
				matched_nodeIDs.push_back((*h_cit)->from);
			}

			MRPT_LOG_DEBUG_STREAM << "Graph nodeIDs prior to merging:\n\t"
				<< getSTLContainerAsString(this->m_graph.getAllNodes());

			MRPT_LOG_DEBUG_STREAM << "Asking for the graph.";
			bool res = curr_neigh.cm_graph_srvclient.call(cm_graph_srv); // blocking
			if (!res) {
				MRPT_LOG_ERROR_STREAM << "Service call for CM_Graph failed.";
				continue; // skip this if failed to fetch other's graph
			}
			MRPT_LOG_DEBUG_STREAM << "Fetched graph";
			MRPT_LOG_DEBUG_STREAM << cm_graph_srv.response.cm_graph;

			//
			// merge the two graphs
			//
			GRAPH_T other_graph;
			mrpt_bridge::convert(cm_graph_srv.response.cm_graph, other_graph);
			MRPT_LOG_DEBUG_STREAM << "Merging graphs.";
			hypots_t valid_hypots_objects(valid_hypots.size());
			std::transform(
					std::begin(valid_hypots),
					std::end(valid_hypots),
					std::begin(valid_hypots_objects),[](hypot_t* hypot){return *hypot;});

			MRPT_LOG_WARN_STREAM << "VALID HYPOTHESES: " << endl
				<< ">>>>>>>>>>>>>>>>>>>>" << endl
				<< getSTLContainerAsString(valid_hypots_objects) << endl
				<< "<<<<<<<<<<<<<<<<<<<<" << endl;

			// keep map of renumbered nodeIDs
			std::map<TNodeID, TNodeID> old_to_new_nodeID_mappings;
			this->m_graph.mergeGraph(other_graph, valid_hypots_objects,
					true,
					&old_to_new_nodeID_mappings);
			MRPT_LOG_DEBUG_STREAM << "Merged graphs.";

			MRPT_LOG_DEBUG_STREAM << "Graph nodeIDs after merging:\n\t"
				<< getSTLContainerAsString(this->m_graph.getAllNodes());

			MRPT_LOG_DEBUG_STREAM << "Updating graph visuals." << endl;
			this->m_optimizer->updateVisuals(); // TODO - remove it!
			MRPT_LOG_DEBUG_STREAM << "Updated." << endl;

			//
			// postprocessing
			//
			nodes_to_scans2D_t new_nodeIDs_to_scans_pairs;
			for (typename hypotsp_t::const_iterator h_cit = valid_hypots.begin();
					h_cit != valid_hypots.end();
					++h_cit) {

				// Mark the neighbor's used nodes as integrated, so that they are not to
				// be returned again..
				// This may mark them twice, but it doesn't affect anything.
				//MRPT_LOG_DEBUG_STREAM << "Marking nodeID \""
					//<< (*h_cit)->from << "\" as integrated.";
				curr_neigh.nodeID_to_is_integrated.at((*h_cit)->from) = true;

				// Integrate LaserScans at the newly integrated nodeIDs in own
				// nodes_to_laser_scans2D map
				//MRPT_LOG_DEBUG_STREAM << "Adding nodeID-LS of nodeID: "
					//<< "\t[old:] " << (*h_cit)->from << endl
					//<< "| [new:] " << old_to_new_nodeID_mappings.at((*h_cit)->from);
 				new_nodeIDs_to_scans_pairs.insert(make_pair(
 							old_to_new_nodeID_mappings.at((*h_cit)->from),
 							gen_hypots_ad_params.groupB_params.at((*h_cit)->from).scan));
			}

 			this->m_nodes_to_laser_scans2D.insert(
 					new_nodeIDs_to_scans_pairs.begin(),
 					new_nodeIDs_to_scans_pairs.end());
 			edge_reg->addBatchOfNodeIDsAndScans(new_nodeIDs_to_scans_pairs);

			if (m_pause_exec_on_mr_registration) this->pauseExec();

			ret_val = true;

			// Call for a Full graph visualization update and Dijkstra update -
			// CGraphSlamOptimizer
			// TODO

			// delete all hypotheses - generated in the heap...
			//MRPT_LOG_DEBUG_STREAM << "Deleting the generated hypotheses pool..." ;
			for (typename hypotsp_t::iterator
					it = hypots_pool.begin(); it != hypots_pool.end(); ++it) {
				delete *it;
			}
			//MRPT_LOG_DEBUG_STREAM << "Deleted.";
		} // end - for all nearby agents
	} // end - for all partitions

	return ret_val;
	MRPT_END;
}

*/

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::_execGraphSlamStep(
		mrpt::obs::CActionCollectionPtr& action,
		mrpt::obs::CSensoryFramePtr& observations,
		mrpt::obs::CObservationPtr& observation,
		size_t& rawlog_entry) {
	MRPT_START;
	using namespace mrpt::graphslam::deciders;
	using namespace mrpt;

	// call parent method
	bool continue_exec = parent_t::_execGraphSlamStep(
			action, observations, observation, rawlog_entry);

	// find matches between own nodes and those of the neighbors
  bool did_register_from_map_merge;
  if (!m_conservative_find_initial_tfs_to_neighbors) {
  	did_register_from_map_merge = this->findTFsWithAllNeighbors();
  }
  else { // intra-graph TF is available - add new nodes
  	THROW_EXCEPTION("Conservative intra-graph TF computation is not yet implemented.");
  }

  bool did_register_from_batches = this->addNodeBatchesFromAllNeighbors();
  m_registered_multiple_nodes = (did_register_from_map_merge || did_register_from_batches);

  if (m_registered_multiple_nodes) {
  	if (this->m_enable_visuals) {
  		this->updateAllVisuals();
  	}
  }

  return continue_exec;
  MRPT_END;
} // end of _execGraphSlamStep

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::initClass() {
	using namespace mrpt::graphslam;
	using namespace mrpt::utils;

	// initialization of topic namespace names
	// TODO - put these into seperate method
	m_mr_ns = "mr_info";

	// initialization of topic names / services
	m_list_neighbors_topic   = m_mr_ns + "/" + "neighbors";
	m_last_regd_id_scan_topic = m_mr_ns + "/" + "last_nodeID_laser_scan";
	m_last_regd_nodes_topic = m_mr_ns + "/" + "last_regd_nodes";
	m_cm_graph_service = m_mr_ns + "/" + "get_cm_graph";

	this->m_class_name = "CGraphSlamEngine_MR_" + m_conn_manager.getTrimmedNs();
	this->setLoggerName(this->m_class_name);
	this->setupComm();

	// Make sure that master_discovery and master_sync are up before trying to
	// connect to other agents
	std::vector<string> nodes_up;
	ros::master::getNodes(nodes_up);
	// If both master_dicovery and master_sync are running, then the
	// /master_sync/get_sync_info service should be available
	MRPT_LOG_INFO_STREAM << "Waiting for master_discovery, master_sync nodes to come up....";
	ros::service::waitForService("/master_sync/get_sync_info"); // block until it is
	MRPT_LOG_INFO_STREAM << "master_discovery, master_sync are available.";

	this->m_node_reg->setClassName(
	 	  this->m_node_reg->getClassName() + "_" + m_conn_manager.getTrimmedNs());
	this->m_edge_reg->setClassName(
	 	  this->m_edge_reg->getClassName() + "_" + m_conn_manager.getTrimmedNs());
	this->m_optimizer->setClassName(
	 	  this->m_optimizer->getClassName() + "_" + m_conn_manager.getTrimmedNs());

	// in case of Multi-robot specific deciders/optimizers (they
	// inherit from the CDeciderOrOptimizer_ROS interface) set the
	// CConnectionManager*
	// NOTE: It's not certain, even though we are running multi-robot graphSLAM
	// that all these classes do inherit from the
	// CRegistrationDeciderOrOptimizer_MR base class. They might be more generic
	// and not care about the multi-robot nature of the algorithm (e.g.
	// optimization scheme)
	{ // NRD
	 	CRegistrationDeciderOrOptimizer_MR<GRAPH_T>* dec_opt_mr =
	 	 	dynamic_cast<CRegistrationDeciderOrOptimizer_MR<GRAPH_T>*>(this->m_node_reg);

	 	if (dec_opt_mr) {
	 	 	dec_opt_mr->setCConnectionManagerPtr(&m_conn_manager);
	 	}
	}
	{ // ERD
		CRegistrationDeciderOrOptimizer_MR<GRAPH_T>* dec_opt_mr =
	 	 	dynamic_cast<CRegistrationDeciderOrOptimizer_MR<GRAPH_T>*>(this->m_edge_reg);
		ASSERT_(dec_opt_mr);
	 	dec_opt_mr->setCConnectionManagerPtr(&m_conn_manager);
	 	dec_opt_mr->setCGraphSlamEnginePtr(this);
	}
	{ // GSO
		CRegistrationDeciderOrOptimizer_MR<GRAPH_T>* dec_opt_mr =
	 	 	dynamic_cast<CRegistrationDeciderOrOptimizer_MR<GRAPH_T>*>(this->m_optimizer);
		if (dec_opt_mr) {
	 		dec_opt_mr->setCConnectionManagerPtr(&m_conn_manager);
		}
	}
	// display messages with the names of the deciders, optimizer and agent string ID
	if (this->m_enable_visuals) {
		// NRD
		this->m_win_manager->assignTextMessageParameters(
				/* offset_y* = */ &m_offset_y_nrd,
				/* text_index* = */ &m_text_index_nrd);
		this->m_win_manager->addTextMessage(this->m_offset_x_left, -m_offset_y_nrd,
				mrpt::format("NRD: %s", this->m_node_reg->getClassName().c_str()),
				TColorf(0,0,0),
				/* unique_index = */ m_text_index_nrd);

		// ERD
		this->m_win_manager->assignTextMessageParameters(
				/* offset_y* = */ &m_offset_y_erd,
				/* text_index* = */ &m_text_index_erd);
		this->m_win_manager->addTextMessage(
				this->m_offset_x_left, -m_offset_y_erd,
				mrpt::format("ERD: %s", this->m_edge_reg->getClassName().c_str()),
				TColorf(0,0,0),
				/* unique_index = */ m_text_index_erd);

		// GSO
		this->m_win_manager->assignTextMessageParameters(
				/* offset_y* = */ &m_offset_y_gso,
				/* text_index* = */ &m_text_index_gso);
		this->m_win_manager->addTextMessage(
				this->m_offset_x_left, -m_offset_y_gso,
				mrpt::format("GSO: %s", this->m_optimizer->getClassName().c_str()),
				TColorf(0,0,0),
				/* unique_index = */ m_text_index_gso);

		// Agent Namespace
		this->m_win_manager->assignTextMessageParameters(
				/* offset_y* = */ &m_offset_y_namespace,
				/* text_index* = */ &m_text_index_namespace);
		this->m_win_manager->addTextMessage(
				this->m_offset_x_left, -m_offset_y_namespace,
				mrpt::format("Agent ID: %s", m_conn_manager.getTrimmedNs().c_str()),
				TColorf(0,0,0),
				/* unique_index = */ m_text_index_namespace);

	}
	this->readParams();

	this->m_optimized_map_color = neighbor_colors_manager.getNextTColor();

	// start the spinner for asynchronously servicing cm_graph requests
	cm_graph_async_spinner.start();
} // end of initClass

template<class GRAPH_T>
mrpt::poses::CPose3D CGraphSlamEngine_MR<GRAPH_T>::
getLSPoseForGridMapVisualization(
		const mrpt::utils::TNodeID nodeID) const {
	MRPT_START;
	using namespace mrpt::graphs::detail;

	// if this is my own node ID return the corresponding CPose2D, otherwise
	// return the pose connecting own graph with the neighbor's graph

	const TMRSlamNodeAnnotations* node_annots = dynamic_cast<const TMRSlamNodeAnnotations*>(
			&this->m_graph.nodes.at(nodeID));

	// Is the current ndoeID registered by a neighboring agent or is it my own?
	TNeighborAgentProps* neighbor = NULL;
	this->getNeighborByAgentID(node_annots->agent_ID_str, neighbor);

	CPose3D laser_pose;
	if (!neighbor) { // own node
		laser_pose = parent_t::getLSPoseForGridMapVisualization(nodeID);
	}
	else { // not by me - return TF to neighbor graph
		laser_pose = CPose3D(neighbor->tf_self_to_neighbor_first_integrated_pose);
	}
	return laser_pose;

	MRPT_END;
}

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::
getNeighborByAgentID(const std::string& agent_ID_str,
		TNeighborAgentProps*& neighbor) const {
	MRPT_START;

	bool ret= false;
	for (auto neighbors_it = m_neighbors.begin();
			neighbors_it != m_neighbors.end();
			++neighbors_it) {

		if ((*neighbors_it)->getAgentNs() == agent_ID_str) {
			ret = true;
			neighbor = *neighbors_it;
			break;
		}
	}

	return ret;
	MRPT_END;
}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::usePublishersBroadcasters() {
	MRPT_START;
	using namespace mrpt_bridge;
	using namespace mrpt_msgs;
	using namespace std;
	using namespace mrpt::math;
	using ::operator==;

	// call the parent class
	parent_t::usePublishersBroadcasters();

	// update list of neighbors that the current agent can communicate with.
	m_conn_manager.getNearbySlamAgents(
				&m_nearby_slam_agents,
				/*ignore_self = */ true);
	m_list_neighbors_pub.publish(m_nearby_slam_agents);

	// Initialize TNeighborAgentProps
	// for each *new* GraphSlamAgent we should add a TNeighborAgentProps instance,
	// and initialize its subscribers so that we fetch every new LaserScan and
	// modified nodes list it publishes
	{
		for (GraphSlamAgents::_list_type::const_iterator
				it = m_nearby_slam_agents.list.begin();
				it != m_nearby_slam_agents.list.end();
				++it) {

			const GraphSlamAgent& gsa = *it;

			// Is the current GraphSlamAgent already registered?
			auto search = [gsa](const TNeighborAgentProps* neighbor) {
				return (neighbor->agent == gsa);
			};
			typename neighbors_t::iterator neighbor_it = find_if(
					m_neighbors.begin(),
					m_neighbors.end(), search);

			if (neighbor_it == m_neighbors.end()) { // current gsa not found, add it

				m_neighbors.push_back(new TNeighborAgentProps(*this, gsa));
				TNeighborAgentProps* latest_neighbor = m_neighbors.back();
				latest_neighbor->setTColor(neighbor_colors_manager.getNextTColor());
				m_neighbor_to_found_initial_tf.insert(make_pair(
							latest_neighbor, false));
				latest_neighbor->setupComm();
				MRPT_LOG_INFO_STREAM << "Initialized NeighborAgentProps instance...";
			}

		}
	}

	this->pubUpdatedNodesList();
	this->pubLastRegdIDScan();


	MRPT_END;
} // end of usePublishersBroadcasters

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::pubUpdatedNodesList() {
	MRPT_START;
	using namespace mrpt_msgs;

	// update the last X NodeIDs + positions; Do it only when a new node is inserted.
	// notified of this change anyway after a new node addition
	if (this->m_graph_nodes_last_size == this->m_graph.nodes.size()) {
		return false;
	}
	MRPT_LOG_DEBUG_STREAM << "Updating list of node poses";

	// send at most m_num_last_rgd_nodes
	typename GRAPH_T::global_poses_t poses_to_send;
	int poses_counter = 0;
	// fill the NodeIDWithPose_vec msg
	NodeIDWithPose_vec ros_nodes;

	// send up to m_num_last_regd_nodes Nodes - start from end.
	for (typename GRAPH_T::global_poses_t::const_reverse_iterator
			cit = this->m_graph.nodes.rbegin();
			(poses_counter <= m_num_last_regd_nodes &&
			 cit != this->m_graph.nodes.rend());
			++cit) {

		// Do not resend nodes of others, registered in own graph.
		if (cit->second.agent_ID_str != m_conn_manager.getTrimmedNs()) {
			continue; // skip this.
		}

		NodeIDWithPose curr_node_w_pose;
		// send basics - NodeID, Pose
		curr_node_w_pose.nodeID = cit->first;
		mrpt_bridge::convert(cit->second, curr_node_w_pose.pose);

		// send mr-fields
		curr_node_w_pose.str_ID.data = cit->second.agent_ID_str;
		curr_node_w_pose.nodeID_loc = cit->second.nodeID_loc;

		ros_nodes.vec.push_back(curr_node_w_pose);

		poses_counter++;
	}

	m_last_regd_nodes_pub.publish(ros_nodes);

	// update the last known size
	m_graph_nodes_last_size = this->m_graph.nodeCount();

	bool res = false;
	if (poses_counter) {
		res = true;
	}

	return res;
	MRPT_END;
} // end of pubUpdatedNodesList

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::pubLastRegdIDScan() {
	MRPT_START;
	using namespace mrpt_msgs;
	using namespace mrpt_bridge;

	// Update the last registered scan + associated nodeID
	// Last registered scan always corresponds to the *last* element of the of the
	// published NodeIDWithPose_vec that is published above.
	//
	// - Check if map is empty
	// - Have I already published the last laser scan?
	if (!this->m_nodes_to_laser_scans2D.empty() &&
			m_nodes_to_laser_scans2D_last_size < this->m_nodes_to_laser_scans2D.size()) {
		// last registered scan
		MRPT_NodeIDWithLaserScan mrpt_last_regd_id_scan =
			*(this->m_nodes_to_laser_scans2D.rbegin());


		// if this is a mr-registered nodeID+LS, skip it.
		if (!this->isOwnNodeID(mrpt_last_regd_id_scan.first)) {
			return false;
		}

		//MRPT_LOG_DEBUG_STREAM
			//<< "Publishing LaserScan of nodeID \""
			//<< mrpt_last_regd_id_scan.first << "\"";
		ASSERT_(mrpt_last_regd_id_scan.second);

		// convert to ROS msg
		mrpt_msgs::NodeIDWithLaserScan ros_last_regd_id_scan;
		convert(*(mrpt_last_regd_id_scan.second), ros_last_regd_id_scan.scan);
		ros_last_regd_id_scan.nodeID = mrpt_last_regd_id_scan.first;

		m_last_regd_id_scan_pub.publish(ros_last_regd_id_scan);

		// update the last known size.
		m_nodes_to_laser_scans2D_last_size =
			this->m_nodes_to_laser_scans2D.size();

		return true;
	}
	else {
		return false;
	}


	MRPT_END;
} // end of pubLastRegdIDScan


template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::isOwnNodeID(
		const mrpt::utils::TNodeID nodeID,
		const global_pose_t* pose_out/*=NULL*/) const {
	MRPT_START;
	using namespace mrpt::graphs::detail;

	// make sure give node is in the graph
	typename GRAPH_T::global_poses_t::const_iterator global_pose_search =
		this->m_graph.nodes.find(nodeID);
	ASSERTMSG_(global_pose_search != this->m_graph.nodes.end(),
			mrpt::format(
				"isOwnNodeID called for nodeID \"%lu\" which is not found in the graph",
				static_cast<unsigned long>(nodeID)));

	// fill pointer to global_pose_t
	if (pose_out) {
		pose_out = &global_pose_search->second;
	}

	bool res = false;
	const TMRSlamNodeAnnotations* node_annots = dynamic_cast<const TMRSlamNodeAnnotations*>(
			&global_pose_search->second);
	if ((node_annots && node_annots->agent_ID_str == m_conn_manager.getTrimmedNs()) ||
			(!node_annots)) {
		res = true;
	}

	return res;

	MRPT_END;
} // end of isOwnNodeID

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::readParams() {
	this->readROSParameters();
	this->readGridMapAlignmentParams();
}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::readGridMapAlignmentParams() {
	using namespace mrpt::slam;

	std::string alignment_opts_ns = "alignment_opts";

	m_nh->param<float>(alignment_opts_ns + "/" +  "min_ICP_goodness",
			m_alignment_options.min_ICP_goodness, 0.60);
	m_nh->param<double>(alignment_opts_ns + "/" + "maxKLd_for_merge",
			m_alignment_options.maxKLd_for_merge, 0.90);
	m_nh->param<float>(alignment_opts_ns + "/" +  "ransac_minSetSizeRatio",
			m_alignment_options.ransac_minSetSizeRatio, 0.40);
	m_nh->param<int>(alignment_opts_ns + "/" +  "nodes_integration_batch_size",
			m_nodes_integration_batch_size, 4);

	// GridMap Alignment options to be used in merging.
	m_alignment_options.methodSelection = CGridMapAligner::amModifiedRANSAC;
	////options.methodSelection = CGridMapAligner::amRobustMatch; // ASSERTION ERROR

	//m_alignment_options.loadFromConfigFileName(
			//"/home/bergercookie/mrpt/share/mrpt/config_files/grid-matching/gridmatch_example.ini",
			//"grid-match");

}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::readROSParameters() {
	// call parent method first in case the parent wants to ask for any ROS
	// parameters
	parent_t::readROSParameters();

	m_nh->param<bool>(m_mr_ns + "/" + "conservative_find_initial_tfs_to_neighbors",
			m_conservative_find_initial_tfs_to_neighbors, false);
	ASSERTMSG_(!m_conservative_find_initial_tfs_to_neighbors,
			"Conservative behavior is not yet implemented.");

	m_nh->param<int>(m_mr_ns + "/" + "num_last_registered_nodes",
			m_num_last_regd_nodes, 10);

	m_nh->param<int>(m_mr_ns + "/" + "intra_group_node_count_thresh",
			m_intra_group_node_count_thresh, m_intra_group_node_count_thresh_minadv);
	// warn user if they choose smaller threshold.
	if (m_intra_group_node_count_thresh < m_intra_group_node_count_thresh_minadv) {
		MRPT_LOG_ERROR_STREAM << "intra_group_node_count_thresh ["
			<< m_intra_group_node_count_thresh
			<< "is set lower than the advised minimum ["
			<< m_intra_group_node_count_thresh_minadv
			<< "]";
	}
}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::printParams() {
	parent_t::printParams();
	m_alignment_options.dumpToConsole();
}



template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::setupSubs() { }

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::setupPubs() {
	using namespace mrpt_msgs;
	using namespace sensor_msgs;

	m_list_neighbors_pub = m_nh->advertise<GraphSlamAgents>(
			m_list_neighbors_topic,
			this->m_queue_size,
			/*latch = */ true);

	// last registered laser scan - by default this corresponds to the last
	// nodeID of the vector of last registered nodes.
	m_last_regd_id_scan_pub = m_nh->advertise<NodeIDWithLaserScan>(
			m_last_regd_id_scan_topic,
			this->m_queue_size,
			/*latch = */ true);

	// last X nodeIDs + positions
	m_last_regd_nodes_pub = m_nh->advertise<NodeIDWithPose_vec>(
			m_last_regd_nodes_topic,
			this->m_queue_size,
			/*latch = */ true);



}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::setupSrvs() {
	// cm_graph service requests are handled by the custom custom_service_queue CallbackQueue
	ros::CallbackQueueInterface* global_queue = m_nh->getCallbackQueue();
	m_nh->setCallbackQueue(&this->custom_service_queue);

	m_cm_graph_srvserver = m_nh->advertiseService(
			m_cm_graph_service,
			&CGraphSlamEngine_MR<GRAPH_T>::getCMGraph, this);

	m_nh->setCallbackQueue(global_queue);
}

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::getCMGraph(
		mrpt_msgs::GetCMGraph::Request& req,
		mrpt_msgs::GetCMGraph::Response& res) {
	using namespace std;
	using namespace mrpt::utils;
	using namespace mrpt::math;

	const size_t min_nodeIDs = 2;

	set<TNodeID> nodes_set(req.nodeIDs.begin(), req.nodeIDs.end());
	MRPT_LOG_INFO_STREAM << "Called the GetCMGraph service for nodeIDs: "
		<< getSTLContainerAsString(nodes_set);

	bool ret_val = false;
	if (nodes_set.size() < 2) { }
	else {
		// fill the given Response with the ROS NetworkOfPoses
		GRAPH_T mrpt_subgraph;
		this->m_graph.extractSubGraph(nodes_set, &mrpt_subgraph,
				/*root_node = */ INVALID_NODEID,
				/*auto_expand_set=*/false);
		mrpt_bridge::convert(mrpt_subgraph, res.cm_graph);
		ret_val = true;
	}

	return ret_val;
} // end of getCMGraph

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::setObjectPropsFromNodeID(
		const mrpt::utils::TNodeID  nodeID,
		mrpt::opengl::CSetOfObjectsPtr& viz_object) {
	using namespace mrpt::utils;
	MRPT_START;

	// if I registered this - just use the standard map color
	TNeighborAgentProps* neighbor = NULL;
	std::string& agent_ID_str = this->m_graph.nodes.at(nodeID).agent_ID_str;
	bool is_not_own = getNeighborByAgentID(agent_ID_str, neighbor);

	TColor obj_color;
	if (!is_not_own) { // I registered this.
		obj_color = this->m_optimized_map_color;
	}
	else {
		ASSERT_(neighbor);
		obj_color = neighbor->color;
	}
	viz_object->setColor_u8(obj_color);

	MRPT_END;
}// end of setObjectPropsFromNodeID


template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::monitorNodeRegistration(
		bool registered/*=false*/,
		std::string class_name/*="Class"*/) {
	MRPT_START;
	using namespace mrpt::utils;

	if (m_registered_multiple_nodes) {
		MRPT_LOG_ERROR_STREAM << "m_registered_multiple_nodes = TRUE!";
		m_registered_multiple_nodes = !m_registered_multiple_nodes;
		this->m_nodeID_max = this->m_graph.nodeCount()-1;
	}
	else {
		parent_t::monitorNodeRegistration(registered, class_name);
	}

	MRPT_END;
} // end of monitorNodeRegistration

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::getRobotEstimatedTrajectory(
		typename GRAPH_T::global_poses_t* graph_poses) const {
	MRPT_START;
	ASSERT_(graph_poses);

	for (const auto& n : this->m_graph.nodes) {
		if (n.second.agent_ID_str == m_conn_manager.getTrimmedNs()) {
			graph_poses->insert(n);
		}
	}

	MRPT_END;
} // end of getRobotEstimatedTrajectory

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::
getAllOwnNodes(std::set<mrpt::utils::TNodeID>* nodes_set) const {
	ASSERT_(nodes_set);
	nodes_set->clear();

	for (const auto& n : this->m_graph.nodes) {
		if (n.second.agent_ID_str == m_conn_manager.getTrimmedNs()) {
			nodes_set->insert(n.first);
		}
	}
} // end of getAllOwnNodes

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::
getNodeIDsOfEstimatedTrajectory(
		std::set<mrpt::utils::TNodeID>* nodes_set) const { 
	ASSERT_(nodes_set);
	this->getAllOwnNodes(nodes_set);
} // end of getNodeIDsOfEstimatedTrajectory

//////////////////////////////////////////////////////////////////////////////////////

template<class GRAPH_T>
CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::TNeighborAgentProps(
		CGraphSlamEngine_MR<GRAPH_T>& engine_in,
		const mrpt_msgs::GraphSlamAgent& agent_in):
	engine(engine_in),
	agent(agent_in),
	has_setup_comm(false)
{
    using namespace mrpt::utils;

	nh = engine.m_nh;
	m_queue_size = engine.m_queue_size;
	this->resetFlags();

	// fill the full paths of the topics to subscribe
	// ASSUMPTION: agents namespaces start at the root "/"
	this->last_regd_nodes_topic = "/" + agent.topic_namespace.data + "/" +
		engine.m_last_regd_nodes_topic;
	this->last_regd_id_scan_topic = "/" + agent.topic_namespace.data + "/" +
		engine.m_last_regd_id_scan_topic;
	this->cm_graph_service = "/" + agent.topic_namespace.data + "/" +
		engine.m_cm_graph_service;

  engine.logFmt(LVL_DEBUG,
      "In constructor of TNeighborAgentProps for topic namespace: %s",
      agent.topic_namespace.data.c_str());

	// initialize the occupancy map
  gridmap_cached = mrpt::maps::COccupancyGridMap2D::Create();
}

template<class GRAPH_T>
CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::~TNeighborAgentProps() { }

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::setupComm() {
	using namespace mrpt::utils;

	this->setupSubs();
	this->setupSrvs();

	engine.logFmt(
			LVL_DEBUG,
			"TNeighborAgentProps: Successfully set up subscribers/services "
			"to agent topics for namespace [%s].",
			agent.topic_namespace.data.c_str());

	has_setup_comm = true;
}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::setupSrvs() {

	cm_graph_srvclient =
		nh->serviceClient<mrpt_msgs::GetCMGraph>(cm_graph_service);
}


template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::setupSubs() {
	using namespace mrpt_msgs;
	using namespace mrpt::utils;

	last_regd_nodes_sub = nh->subscribe<NodeIDWithPose_vec>(
			last_regd_nodes_topic,
			m_queue_size,
			&TNeighborAgentProps::fetchUpdatedNodesList, this);
	last_regd_id_scan_sub = nh->subscribe<NodeIDWithLaserScan>(
			last_regd_id_scan_topic,
			m_queue_size,
			&TNeighborAgentProps::fetchLastRegdIDScan, this);
}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
fetchUpdatedNodesList(
		const mrpt_msgs::NodeIDWithPose_vec::ConstPtr& nodes) {
	MRPT_START;
	using namespace mrpt_msgs;
	using namespace mrpt_bridge;
	using namespace std;
	using namespace mrpt::utils;

	typedef typename GRAPH_T::constraint_t::type_value pose_t;
    engine.logFmt(LVL_DEBUG, "In fetchUpdatedNodesList method.");

	for (NodeIDWithPose_vec::_vec_type::const_iterator
			n_it = nodes->vec.begin();
			n_it != nodes->vec.end();
			++n_it) {

		// insert in the set if not already there.
		TNodeID  nodeID = static_cast<TNodeID>(n_it->nodeID);
		std::pair<set<TNodeID>::iterator, bool> res =
			nodeIDs_set.insert(nodeID);
		// if I just inserted this node mark it as not used (in own graph)
		if (res.second) { // insertion took place.
			engine.logFmt(LVL_INFO, "Just fetched a new nodeID: %lu",
					static_cast<unsigned long>(nodeID));
			nodeID_to_is_integrated.insert(make_pair(n_it->nodeID, false));
		}

		// update the poses
		pose_t curr_pose;
		// note: use "operator[]" instead of "insert" so that if the key already
		// exists, the corresponding value is changed rather than ignored.
		poses[static_cast<TNodeID>(n_it->nodeID)] =
			convert(n_it->pose, curr_pose);
	}
  has_new_nodes = true;

	//// Mon Mar 6 17:11:23 EET 2017, Nikos Koukis
	// TODO When using 3 graphSLAM agents GDB Shows that it crashes on
	// engine.logFmt lines. Fix this.
	//
  //engine.logFmt(LVL_DEBUG, // THIS CRASHES - GDB WHERE
      //"NodeIDs for topic namespace: %s -> [%s]",
      //agent.topic_namespace.data.c_str(),
      //mrpt::math::getSTLContainerAsString(vector<TNodeID>(
          //nodeIDs_set.begin(), nodeIDs_set.end())).c_str());
  // print poses just for verification
  //engine.logFmt(LVL_DEBUG, "Poses for topic namespace: %s",
      //agent.topic_namespace.data.c_str());
  //for (typename GRAPH_T::global_poses_t::const_iterator
      //p_it = poses.begin();
      //p_it != poses.end();
      //++p_it) {
    //std::string p_str; p_it->second.asString(p_str);
    //engine.logFmt(LVL_DEBUG, "nodeID: %lu | pose: %s",
        //static_cast<unsigned long>(p_it->first),
        //p_str.c_str());
  //}
	// TODO - These also seem to crash sometimes
	//cout << "Agent information: " << agent << endl;
	//cout << "Nodes: " << mrpt::math::getSTLContainerAsString(vector<TNodeID>(nodeIDs_set.begin(), nodeIDs_set.end()));
  //print nodeIDs just for verification

  MRPT_END;
} // end of fetchUpdatedNodesList

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
fetchLastRegdIDScan(
		const mrpt_msgs::NodeIDWithLaserScan::ConstPtr& last_regd_id_scan) {
	MRPT_START;
	using namespace std;
	using namespace mrpt::utils;
	ASSERT_(last_regd_id_scan);
  engine.logFmt(LVL_DEBUG, "In fetchLastRegdIDScan method.");

	// make sure I haven't received any LaserScan for the current nodeID
	// TODO
	TNodeID curr_node = static_cast<TNodeID>(last_regd_id_scan->nodeID);

	// Pose may not be available due to timing in publishing of the corresponding
	// topics. Just keep it in ROS form and then convert them to MRPT form when
	// needed.
	ros_scans.push_back(*last_regd_id_scan);
	has_new_scans = true;

	MRPT_END;
} // end of fetchLastRegdIDScan

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::getCachedNodes(
		vector_uint* nodeIDs/*=NULL*/,
		std::map<
			mrpt::utils::TNodeID,
			node_props_t>* nodes_params/*=NULL*/,
		bool only_unused/*=true*/) const {
	MRPT_START;
	using namespace mrpt::obs;
	using namespace mrpt::utils;
	using namespace mrpt::math;
	using namespace std;

	// at least one of the two args should be given.
	ASSERT_(nodeIDs || nodes_params);

	// traverse all nodes
	for (std::set<TNodeID>::const_iterator
			n_it = nodeIDs_set.begin();
			n_it != nodeIDs_set.end();
			++n_it) {

		// Should I return only the unused ones?
		// If so, if the current nodeID is integrated, skip it.
		if (only_unused && nodeID_to_is_integrated.at(*n_it)) { continue; }

		// add the node properties
		if (nodes_params) {
			std::pair<TNodeID, node_props_t> params; // current pair to be inserted.
			const pose_t* p = &poses.at(*n_it);

			params.first = *n_it;
			params.second.pose = *p;
			CObservation2DRangeScanPtr mrpt_scan = CObservation2DRangeScan::Create();
      const sensor_msgs::LaserScan* ros_laser_scan =
        this->getLaserScanByNodeID(*n_it);

      // if LaserScan not found, skip nodeID altogether.
      //
      // Its natural to have more poses than LaserScans since on every node
      // registration I send a modified list of X node positions and just one
      // LaserScan
      if (!ros_laser_scan) { continue; }

			mrpt_bridge::convert(*ros_laser_scan, *p, *mrpt_scan);
			params.second.scan = mrpt_scan;

			// insert the pair
			nodes_params->insert(params);

			// debugging message
			//std::stringstream ss;
			//ss << "\ntID: " << nodes_params->rbegin()->first
				//<< " ==> Props: " << nodes_params->rbegin()->second;
			//engine.logFmt(LVL_DEBUG, "%s", ss.str().c_str());
		}

		// add the nodeID
		if (nodeIDs) {
			// do not return nodeIDs that don't have valid laserScans
			// this is also checked at the prior if
			if (!nodes_params && !this->getLaserScanByNodeID(*n_it)) {
				continue;
			}

			nodeIDs->push_back(*n_it);
		}
	}

	MRPT_END;
} // end of TNeighborAgentProps::getCachedNodes

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::resetFlags() const {
  has_new_nodes = false;
  has_new_scans = false;

}

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
hasNewNodesBatch(int new_batch_size) {
	MRPT_START;

	auto is_not_integrated = [this](const std::pair<mrpt::utils::TNodeID, bool> pair) {
		return (pair.second == false && getLaserScanByNodeID(pair.first));
	};
	int not_integrated_num = count_if(
			nodeID_to_is_integrated.begin(),
			nodeID_to_is_integrated.end(),
			is_not_integrated);

	return (not_integrated_num >= new_batch_size);

	MRPT_END;
}


template<class GRAPH_T>
const sensor_msgs::LaserScan*
CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
getLaserScanByNodeID(
		const mrpt::utils::TNodeID nodeID) const {
	MRPT_START;

	// assert that the current nodeID exists in the nodeIDs_set
	ASSERT_(nodeIDs_set.find(nodeID) != nodeIDs_set.end());

	for (std::vector<mrpt_msgs::NodeIDWithLaserScan>::const_iterator
			it = ros_scans.begin();
			it != ros_scans.end();
			++it) {
		if (it->nodeID == nodeID) {
			return &it->scan;
		}
	}

	// if out here, LaserScan doesn't exist.
  return 0;
  MRPT_END;
} // end of TNeighborAgentProps::getLaserScanByNodeID

template<class GRAPH_T>
void
CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::fillOptPaths(
		const std::set<mrpt::utils::TNodeID>& nodeIDs,
		paths_t* opt_paths) const {
	MRPT_START;
	using namespace std;
	using namespace mrpt::utils;
	typedef std::set<TNodeID>::const_iterator set_cit;

	// make sure that all given nodes belong in the nodeIDs_set.
	ASSERTMSG_(includes(nodeIDs_set.begin(), nodeIDs_set.end(),
				nodeIDs.begin(), nodeIDs.end()),
			"nodeIDs is not a strict subset of the current neighbor's nodes");
	ASSERT_(opt_paths);

	for (set_cit cit = nodeIDs.begin();
			cit != std::prev(nodeIDs.end());
			++cit) {

		for (set_cit cit2 = std::next(cit);
				cit2 != nodeIDs.end();
				++cit2) {


			constraint_t c = constraint_t(poses.at(*cit2) - poses.at(*cit));
			c.cov_inv =
				mrpt::math::CMatrixFixedNumeric<double,
					constraint_t::state_length,
					constraint_t::state_length>();
			c.cov_inv.unit();
			c.cov_inv *= 500;

			opt_paths->push_back(path_t(
						/*start=*/ *cit,
						/*end=*/ *cit2,
						/*constraint=*/ c));

			engine.logFmt(LVL_DEBUG,
					"Filling optimal path for nodes: %lu => %lu: %s",
					static_cast<unsigned long>(*cit),
					static_cast<unsigned long>(*cit2),
					opt_paths->back().curr_pose_pdf.getMeanVal().asString().c_str());

		}
	}
	MRPT_END;
} // end of TNeighborAgentProps::fillOptPaths

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
computeGridMap() const {
	MRPT_START;
	using namespace mrpt::poses;
	using namespace mrpt::utils;
	using namespace mrpt;
	using namespace mrpt::math;

	gridmap_cached->clear();

	vector_uint nodeIDs;
	std::map<TNodeID, node_props_t> nodes_params;
	// get list of nodes, laser scans
	this->getCachedNodes(&nodeIDs, &nodes_params, false);

	// iterate over poses, scans. Add them to the gridmap.
	for (typename map<TNodeID, node_props_t>::const_iterator
			it = nodes_params.begin();
			it != nodes_params.end();
			++it) {
		const CPose3D curr_pose_3d; // do not add the actual pose!
		gridmap_cached->insertObservation(it->second.scan.pointer(), &curr_pose_3d);
	}

	MRPT_END;
}

template<class GRAPH_T>
const mrpt::maps::COccupancyGridMap2DPtr&
CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
getGridMap() const { 
	MRPT_START;
	if (hasNewData()) {
		this->computeGridMap();
	}

	return gridmap_cached;
	MRPT_END;
}

template<class GRAPH_T>
void CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::
getGridMap(mrpt::maps::COccupancyGridMap2DPtr& map) const {
	MRPT_START;
	ASSERT_(map.present());

	if (hasNewData()) {
		this->computeGridMap();
	}

	map->copyMapContentFrom(*gridmap_cached);
	MRPT_END;
}

template<class GRAPH_T>
bool CGraphSlamEngine_MR<GRAPH_T>::TNeighborAgentProps::hasNewData() const {
  return has_new_nodes && has_new_scans;
}

} } // end of namespaces

#endif /* end of include guard: CGRAPHSLAMENGINE_MR_IMPL_H */
