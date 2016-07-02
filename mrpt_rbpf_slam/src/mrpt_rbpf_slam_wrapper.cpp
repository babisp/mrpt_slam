
#include "mrpt_rbpf_slam/mrpt_rbpf_slam_wrapper.h"

PFslamWrapper::PFslamWrapper(){
    rawlog_play_=false;
    mrpt_bridge::convert(ros::Time(0), timeLastUpdate_);
}

PFslamWrapper::~PFslamWrapper(){
}

bool PFslamWrapper::is_file_exists(const std::string& name) {
    std::ifstream f(name.c_str());
    return f.good();
}

void PFslamWrapper::get_param(){
    ROS_INFO("READ PARAM FROM LAUNCH FILE");
    n_.param<double>("rawlog_play_delay", rawlog_play_delay, 0.1);
    ROS_INFO("rawlog_play_delay: %f", rawlog_play_delay);

    n_.getParam("rawlog_filename", rawlog_filename);
    ROS_INFO("rawlog_filename: %s", rawlog_filename.c_str());

    n_.getParam("ini_filename", ini_filename);
    ROS_INFO("ini_filename: %s", ini_filename.c_str());

    n_.param<std::string>("global_frame_id", global_frame_id, "map");
    ROS_INFO("global_frame_id: %s", global_frame_id.c_str());

    n_.param<std::string>("odom_frame_id", odom_frame_id, "odom");
    ROS_INFO("odom_frame_id: %s", odom_frame_id.c_str());

    n_.param<std::string>("base_frame_id", base_frame_id, "base_link");
    ROS_INFO("base_frame_id: %s", base_frame_id.c_str());

    n_.param<std::string>("sensor_source", sensor_source, "scan");
    ROS_INFO("sensor_source: %s", sensor_source.c_str());

    
}


void PFslamWrapper::init(){

     //get parameters from ini file
     if(!is_file_exists(ini_filename)){
        ROS_ERROR_STREAM("CAN'T READ INI FILE");
        return;
     }
      PFslam::read_iniFile(ini_filename);
      //read rawlog file if it  exists
    if(is_file_exists(rawlog_filename)){
        ROS_WARN_STREAM("PLAY FROM RAWLOG FILE: "<<rawlog_filename.c_str());
        PFslam::read_rawlog(data,rawlog_filename);
        rawlog_play_=true;
     }

    
      ///Create publishers///        
      //publish grid map
      pub_map_ = n_.advertise<nav_msgs::OccupancyGrid>("map", 1, true);
      pub_metadata_= n_.advertise<nav_msgs::MapMetaData>("map_metadata", 1, true);
      //robot pose
      pub_Particles_ = n_.advertise<geometry_msgs::PoseArray>("particlecloud", 1, true);
      //ro particles poses
      pub_Particles_Beacons_ = n_.advertise<geometry_msgs::PoseArray>("particlecloud_beacons", 1, true);

    
    
        
    //read sensor topics
    std::vector<std::string> lstSources;
	mrpt::system::tokenize(sensor_source," ,\t\n",lstSources);
	ROS_ASSERT_MSG(!lstSources.empty(), "*Fatal*: At least one sensor source must be provided in ~sensor_sources (e.g. \"scan\" or \"beacon\")");
	

    ///Create subscribers///	
	sensorSub_.resize(lstSources.size());
	for (size_t i=0;i<lstSources.size();i++) {
		if(lstSources[i].find("scan") != std::string::npos) {
			sensorSub_[i]  = n_.subscribe(lstSources[i],  1, &PFslamWrapper::laserCallback, this);
		}
		else {
            sensorSub_[i]  = n_.subscribe(lstSources[i],  1, &PFslamWrapper::callbackBeacon, this);
		}
	}

    //init slam
    mapBuilder = new mrpt::slam::CMetricMapBuilderRBPF(rbpfMappingOptions);
    init_slam();

}

void PFslamWrapper::odometryForCallback (CObservationOdometryPtr  &_odometry, const std_msgs::Header &_msg_header) {
   
    mrpt::poses::CPose3D poseOdom;
     if(this->waitForTransform(poseOdom, odom_frame_id, base_frame_id, _msg_header.stamp, ros::Duration(1))){
		_odometry = CObservationOdometry::Create();
        _odometry->sensorLabel = odom_frame_id;
        _odometry->hasEncodersInfo = false;
        _odometry->hasVelocities = false;
        _odometry->odometry.x() = poseOdom.x();
        _odometry->odometry.y() = poseOdom.y();
        _odometry->odometry.phi() = poseOdom.yaw();
    }
}

bool PFslamWrapper::waitForTransform(mrpt::poses::CPose3D &des, const std::string& target_frame, const std::string& source_frame, const ros::Time& time, const ros::Duration& timeout, const ros::Duration& polling_sleep_duration){
    tf::StampedTransform transform;
    try
    {
        listenerTF_.waitForTransform(target_frame, source_frame,  time, polling_sleep_duration);
        listenerTF_.lookupTransform(target_frame, source_frame,  time, transform);
    }
    catch(tf::TransformException)
    {
        ROS_INFO("Failed to get transform target_frame (%s) to source_frame (%s)", target_frame.c_str(), source_frame.c_str());
        return false;
    }
    mrpt_bridge::convert(transform, des);
    return true;
}

void PFslamWrapper::laserCallback(const sensor_msgs::LaserScan &_msg) {
#if MRPT_VERSION>=0x130
	using namespace mrpt::maps;
	using namespace mrpt::obs;
#else
	using namespace mrpt::slam;
#endif
	CObservation2DRangeScanPtr laser = CObservation2DRangeScan::Create();

    if(laser_poses_.find(_msg.header.frame_id) == laser_poses_.end()) { 
        updateSensorPose (_msg.header.frame_id);    
    } else {        
        mrpt::poses::CPose3D pose = laser_poses_[_msg.header.frame_id];
        mrpt_bridge::convert(_msg, laser_poses_[_msg.header.frame_id],  *laser);

	sf = CSensoryFrame::Create();
	CObservationOdometryPtr odometry;
	odometryForCallback(odometry, _msg.header);

	CObservationPtr obs = CObservationPtr(laser);
        sf->insert(obs);
        observation(sf, odometry);
        timeLastUpdate_ = sf->getObservationByIndex(0)->timestamp;

        tictac.Tic();
        mapBuilder->processActionObservation( *action,*sf);
        t_exec = tictac.Tac();
      	printf("Map building executed in %.03fms\n", 1000.0f*t_exec );
        publishMapPose();

    }
}

void PFslamWrapper::callbackBeacon (const mrpt_msgs::ObservationRangeBeacon &_msg) {
#if MRPT_VERSION>=0x130
	using namespace mrpt::maps;
	using namespace mrpt::obs;
#else
	using namespace mrpt::slam;
#endif

    
	CObservationBeaconRangesPtr beacon = CObservationBeaconRanges::Create();
    if(beacon_poses_.find(_msg.header.frame_id) == beacon_poses_.end()) {
		updateSensorPose (_msg.header.frame_id);
    } else {
        
        mrpt_bridge::convert(_msg, beacon_poses_[_msg.header.frame_id],  *beacon);

		sf = CSensoryFrame::Create();
	CObservationOdometryPtr odometry;
	odometryForCallback(odometry, _msg.header);
	
	CObservationPtr obs = CObservationPtr(beacon);
        sf->insert(obs);
        observation(sf, odometry);
       timeLastUpdate_ = sf->getObservationByIndex(0)->timestamp;

        tictac.Tic();
        mapBuilder->processActionObservation( * action,*sf);
        t_exec = tictac.Tac();
        printf("Map building executed in %.03fms\n", 1000.0f*t_exec );

        publishMapPose();
    }
}


void PFslamWrapper::publishMapPose(){

        //if I received new grid maps from 2D laser scan sensors
      metric_map_ = mapBuilder->mapPDF.getCurrentMostLikelyMetricMap();
       mapBuilder->mapPDF.getEstimatedPosePDF(curPDF);
        if (metric_map_->m_gridMaps.size()){         
                                   
            //publish map
            nav_msgs::OccupancyGrid  _msg;                  
            mrpt_bridge::convert(*metric_map_->m_gridMaps[0], _msg );
            pub_map_.publish(_msg);
            pub_metadata_.publish(_msg.info);                
           
        }
   
    //if I received new beacon (range only) map 
        if (metric_map_->m_beaconMap){     
          

         	mrpt::opengl::CSetOfObjectsPtr objs;
            objs = mrpt::opengl::CSetOfObjects::Create();
            metric_map_->m_beaconMap->getAs3DObject( objs );

            geometry_msgs::PoseArray poseArrayBeacons;
		    poseArrayBeacons.header.frame_id = global_frame_id;
		    poseArrayBeacons.header.stamp = ros::Time::now();
	
		   
	        int objs_counter = 0;
		    while (objs->getByClass<mrpt::opengl::CEllipsoid>(objs_counter )) { 
			    objs_counter++;
		    }	
		    poseArrayBeacons.poses.resize(objs_counter);
		    mrpt::opengl::CEllipsoidPtr beacon_particle;

		    for (size_t i = 0; i < objs_counter; i++) {
			    beacon_particle = objs->getByClass<mrpt::opengl::CEllipsoid>(i);	
			    mrpt_bridge::convert(beacon_particle->getPose(), poseArrayBeacons.poses[i]);		
		    }
		    pub_Particles_Beacons_.publish(poseArrayBeacons);  
        }

        //publish pose
        geometry_msgs::PoseArray poseArray;
		poseArray.header.frame_id =  global_frame_id;
		poseArray.header.stamp = ros::Time::now();
		poseArray.poses.resize(curPDF.particlesCount());
		for(size_t i = 0; i < curPDF.particlesCount(); i++) {
			mrpt::poses::CPose3D p = curPDF.getParticlePose(i);
			mrpt_bridge::convert(p, poseArray.poses[i]);
		}
   
		pub_Particles_.publish(poseArray);
 


}

void PFslamWrapper::updateSensorPose (std::string _frame_id) {
    CPose3D pose;
    tf::StampedTransform transform;
    try {

        listenerTF_.lookupTransform(base_frame_id, _frame_id, ros::Time(0), transform);

        tf::Vector3 translation = transform.getOrigin();
        tf::Quaternion quat = transform.getRotation();
        pose.x() = translation.x();
        pose.y() = translation.y();
        pose.z() = translation.z();
        double roll, pitch, yaw;
        tf::Matrix3x3 Rsrc(quat);
        CMatrixDouble33 Rdes;
        for(int c = 0; c < 3; c++)
            for(int r = 0; r < 3; r++)
                Rdes(r,c) = Rsrc.getRow(r)[c];
        pose.setRotationMatrix(Rdes);
        laser_poses_[_frame_id] = pose;
       beacon_poses_[_frame_id] = pose;
    }
    catch (tf::TransformException ex) {
        ROS_ERROR("%s",ex.what());
        ros::Duration(1.0).sleep();
    }

}

bool PFslamWrapper::rawlogPlay() {
    if ( rawlog_play_==false){
        return false;    
    }else{

   
   
    for (int i=0; i<data.size(); i++){
      if(ros::ok()){
        
        tictac.Tic();
        mapBuilder->processActionObservation( data[i].first,data[i].second);
        t_exec = tictac.Tac();
      	printf("Map building executed in %.03fms\n", 1000.0f*t_exec );
       
        ros::Duration(rawlog_play_delay).sleep(); 
    
         metric_map_ = mapBuilder->mapPDF.getCurrentMostLikelyMetricMap();
         mapBuilder->mapPDF.getEstimatedPosePDF(curPDF);
        //if I received new grid maps from 2D laser scan sensors
        if (metric_map_->m_gridMaps.size()){         
              
                nav_msgs::OccupancyGrid  _msg;  
   
                //if we have new map for current sensor update it
               mrpt_bridge::convert(*metric_map_->m_gridMaps[0], _msg );
                    pub_map_.publish(_msg);
                    pub_metadata_.publish(_msg.info);          
           
        }
   
    //if I received new beacon (range only) map 
        if (metric_map_->m_beaconMap){         

         	mrpt::opengl::CSetOfObjectsPtr objs;
            objs = mrpt::opengl::CSetOfObjects::Create();
            metric_map_->m_beaconMap->getAs3DObject( objs );

            geometry_msgs::PoseArray poseArrayBeacons;
		    poseArrayBeacons.header.frame_id = global_frame_id;
		    poseArrayBeacons.header.stamp = ros::Time::now();
	
		   
	        int objs_counter = 0;
		    while (objs->getByClass<mrpt::opengl::CEllipsoid>(objs_counter )) { 
			    objs_counter++;
		    }	
		    poseArrayBeacons.poses.resize(objs_counter);
		    mrpt::opengl::CEllipsoidPtr beacon_particle;

		    for (size_t i = 0; i < objs_counter; i++) {
			    beacon_particle = objs->getByClass<mrpt::opengl::CEllipsoid>(i);	
			    mrpt_bridge::convert(beacon_particle->getPose(), poseArrayBeacons.poses[i]);		
		    }
		    pub_Particles_Beacons_.publish(poseArrayBeacons);  
        }

        //publish pose
        geometry_msgs::PoseArray poseArray;
		poseArray.header.frame_id =  global_frame_id;
		poseArray.header.stamp = ros::Time::now();
		poseArray.poses.resize(curPDF.particlesCount());
		for(size_t i = 0; i < curPDF.particlesCount(); i++) {
			mrpt::poses::CPose3D p = curPDF.getParticlePose(i);
			mrpt_bridge::convert(p, poseArray.poses[i]);
		}

		pub_Particles_.publish(poseArray);
      }
    ros::spinOnce();

    }

 }   
    return true;
}

void PFslamWrapper::publishTF() {
	// Most of this code was copy and pase from ros::amcl
	mrpt::poses::CPose3D		robotPose;
	mapBuilder->mapPDF.getEstimatedPosePDF(curPDF);
  
	curPDF.getMean(robotPose);
 
    tf::Stamped<tf::Pose> odom_to_map;
    tf::Transform tmp_tf;
    ros::Time stamp;
   mrpt_bridge::convert(timeLastUpdate_, stamp);
    mrpt_bridge::convert(robotPose, tmp_tf);
 
    try
    {
        tf::Stamped<tf::Pose> tmp_tf_stamped (tmp_tf.inverse(), stamp,  base_frame_id);
        listenerTF_.transformPose(odom_frame_id, tmp_tf_stamped, odom_to_map);

    }
    catch(tf::TransformException)
    {
		ROS_INFO("Failed to subtract global_frame (%s) from odom_frame (%s)", global_frame_id.c_str(), odom_frame_id.c_str());
        return;
    }

    tf::Transform latest_tf_ = tf::Transform(tf::Quaternion(odom_to_map.getRotation()),
                               tf::Point(odom_to_map.getOrigin()));

    // We want to send a transform that is good up until a
    // tolerance time so that odom can be used

    ros::Duration transform_tolerance_(0.5);
    ros::Time transform_expiration = (stamp + transform_tolerance_);
    tf::StampedTransform tmp_tf_stamped(latest_tf_.inverse(),
                                        transform_expiration,
                                        global_frame_id, odom_frame_id);
    tf_broadcaster_.sendTransform(tmp_tf_stamped);

}
