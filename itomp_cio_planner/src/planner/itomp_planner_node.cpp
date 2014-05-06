#include <itomp_cio_planner/planner/itomp_planner_node.h>
#include <itomp_cio_planner/model/itomp_planning_group.h>

#include <itomp_cio_planner/util/planning_parameters.h>
#include <itomp_cio_planner/visualization/visualization_manager.h>
#include <kdl/jntarray.hpp>
#include <angles/angles.h>
#include <visualization_msgs/MarkerArray.h>
#
#include <boost/random/uniform_real.hpp>
#include <boost/random/variate_generator.hpp>

#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_state/robot_state.h>

using namespace std;

namespace itomp_cio_planner
{

ItompPlannerNode::ItompPlannerNode(const robot_model::RobotModelConstPtr& model) :
    trajectory_(NULL), last_planning_time_(0), last_min_cost_trajectory_(0)
{
  complete_initial_robot_state_.reset(new robot_state::RobotState(model));
}

bool ItompPlannerNode::init()
{
  PlanningParameters::getInstance()->initFromNodeHandle();

  int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();

  robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
  robot_model::RobotModelPtr kinematic_model = robot_model_loader.getModel();

  // initialize
  robot_model_.resize(num_trajectories);
  string reference_frame = kinematic_model->getModelFrame();
  // build the robot model
  for (int i = 0; i < num_trajectories; ++i)
  {
    if (!robot_model_[i].init(kinematic_model, robot_model_loader.getRobotDescription()))
      return false;
  }

  VisualizationManager::getInstance()->initialize(robot_model_[0]);

  double trajectory_duration = PlanningParameters::getInstance()->getTrajectoryDuration();
  double trajectory_discretization = PlanningParameters::getInstance()->getTrajectoryDiscretization();
  int num_contacts = PlanningParameters::getInstance()->getNumContacts();
  trajectory_ = new ItompCIOTrajectory(&robot_model_[0], trajectory_duration, trajectory_discretization, num_contacts,
      PlanningParameters::getInstance()->getPhaseDuration());
  threadTrajectories_.resize(num_trajectories);
  for (int i = 0; i < num_trajectories; ++i)
    threadTrajectories_[i] = new ItompCIOTrajectory(&robot_model_[i], trajectory_duration, trajectory_discretization,
        num_contacts, PlanningParameters::getInstance()->getPhaseDuration());

  ROS_INFO("Initalized ITOMP planning service...");

  return true;
}

ItompPlannerNode::~ItompPlannerNode()
{
  int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
  delete trajectory_;
  for (int i = 0; i < num_trajectories; ++i)
  {
    delete threadTrajectories_[i];
    delete optimizers_[i];
  }
}

int ItompPlannerNode::run()
{
  return 0;
}

bool ItompPlannerNode::planKinematicPath(const planning_interface::MotionPlanRequest &req,
    planning_interface::MotionPlanResponse &res)
{
  // reload parameters
  PlanningParameters::getInstance()->initFromNodeHandle();

  ros::WallTime start_time = ros::WallTime::now();

  //ros::spinOnce();

  if (!preprocessRequest(req))
    return false;

  // generate planning group list
  vector<string> planningGroups;
  getPlanningGroups(planningGroups, req.group_name);

  int num_trials = PlanningParameters::getInstance()->getNumTrials();
  resetPlanningInfo(num_trials, planningGroups.size());
  for (int c = 0; c < num_trials; ++c)
  {
    printf("Trial [%d]\n", c);

    // initialize trajectory with start state
    initTrajectory(req.start_state.joint_state);
    planning_scene::PlanningScene planning_scene(robot_model_[0].getRobotModel());
    complete_initial_robot_state_ = planning_scene.getCurrentStateUpdated(req.start_state);

    sensor_msgs::JointState jointGoalState;
    getGoalState(req, jointGoalState);

    planning_start_time_ = ros::Time::now().toSec();

    // for each planning group
    for (unsigned int i = 0; i != planningGroups.size(); ++i)
    {
      const string& groupName = planningGroups[i];

      // generate multiple thread trajectories (multi-threading)
      // optimize (multi-threading)
      multiTrajectoryOptimization(groupName, jointGoalState);

      // update trajectory with the best thread trajectory
      updateTrajectoryToBestResult(groupName);

      writePlanningInfo(c, i);
    }
  }
  printPlanningInfoSummary();

  // return trajectory
  fillInResult(planningGroups, res);

  /*
   int goal_index = trajectory_->getNumPoints() - 1;
   ROS_INFO("Serviced planning request in %f wall-seconds, trajectory duration is %f",
   (ros::WallTime::now() - start_time).toSec(),
   res.trajectory.joint_trajectory.points[goal_index].time_from_start.toSec());
   */
  return true;
}

bool ItompPlannerNode::preprocessRequest(const planning_interface::MotionPlanRequest &req)
{
  ROS_INFO("Received planning request...");

  /*
   if (req.motion_plan_request.expected_path_duration.toSec() > 0)
   {
   PlanningParameters::getInstance()->setTrajectoryDuration(
   req.motion_plan_request.expected_path_duration.toSec());
   }
   */

  ROS_INFO("Trajectory Duration : %f", PlanningParameters::getInstance()->getTrajectoryDuration());

  trajectory_start_time_ = req.start_state.joint_state.header.stamp.toSec();

  // check goal constraint
  ROS_INFO("goal");
  sensor_msgs::JointState goal_joint_state = jointConstraintsToJointState(req.goal_constraints);
  if (goal_joint_state.name.size() != goal_joint_state.position.size())
  {
    ROS_ERROR("Invalid goal");
    return false;
  }
  for (unsigned int i = 0; i < goal_joint_state.name.size(); i++)
  {
    ROS_INFO("%s %f", goal_joint_state.name[i].c_str(), goal_joint_state.position[i]);
  }

  ROS_INFO_STREAM("Joint state has " << req.start_state.joint_state.name.size() << " joints");

  return true;
}

void ItompPlannerNode::initTrajectory(const sensor_msgs::JointState &joint_state)
{
  int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
  double trajectory_duration = PlanningParameters::getInstance()->getTrajectoryDuration();
  if (trajectory_->getDuration() != trajectory_duration)
  {
    double trajectory_discretization = PlanningParameters::getInstance()->getTrajectoryDiscretization();

    delete trajectory_;
    trajectory_ = new ItompCIOTrajectory(&robot_model_[0], trajectory_duration, trajectory_discretization,
        PlanningParameters::getInstance()->getNumContacts(), PlanningParameters::getInstance()->getPhaseDuration());

    for (int i = 0; i < num_trajectories; ++i)
    {
      delete threadTrajectories_[i];
      threadTrajectories_[i] = new ItompCIOTrajectory(&robot_model_[i], trajectory_duration, trajectory_discretization,
          PlanningParameters::getInstance()->getNumContacts(), PlanningParameters::getInstance()->getPhaseDuration());
    }
  }

  // set the trajectory to initial state value
  start_point_velocities_ = Eigen::MatrixXd(1, robot_model_[0].getNumKDLJoints());
  start_point_accelerations_ = Eigen::MatrixXd(1, robot_model_[0].getNumKDLJoints());

  robot_model_[0].jointStateToArray(joint_state, trajectory_->getTrajectoryPoint(0), start_point_velocities_.row(0),
      start_point_accelerations_.row(0));

  for (int i = 1; i < trajectory_->getNumPoints(); ++i)
  {
    trajectory_->getTrajectoryPoint(i) = trajectory_->getTrajectoryPoint(0);
  }

  // set the contact trajectory initial values
  Eigen::MatrixXd::RowXpr initContacts = trajectory_->getContactTrajectoryPoint(0);
  Eigen::MatrixXd::RowXpr goalContacts = trajectory_->getContactTrajectoryPoint(trajectory_->getNumContactPhases());
  for (int i = 0; i < trajectory_->getNumContacts(); ++i)
  {
    initContacts(i) = PlanningParameters::getInstance()->getContactVariableInitialValues()[i];
    goalContacts(i) = PlanningParameters::getInstance()->getContactVariableGoalValues()[i];
  }
  for (int i = 1; i < trajectory_->getNumContactPhases(); ++i)
  {
    trajectory_->getContactTrajectoryPoint(i) = initContacts;
  }
}

void ItompPlannerNode::getGoalState(const planning_interface::MotionPlanRequest &req,
    sensor_msgs::JointState& goalState)
{
  sensor_msgs::JointState goal_joint_state = jointConstraintsToJointState(req.goal_constraints);
  goalState.name.resize(req.start_state.joint_state.name.size());
  goalState.position.resize(req.start_state.joint_state.position.size());
  for (unsigned int i = 0; i < goal_joint_state.name.size(); ++i)
  {
    string name = goal_joint_state.name[i];
    int kdl_number = robot_model_[0].urdfNameToKdlNumber(name);
    if (kdl_number >= 0)
    {
      goalState.name[kdl_number] = name;
      goalState.position[kdl_number] = goal_joint_state.position[i];
    }
  }

}

void ItompPlannerNode::getPlanningGroups(std::vector<std::string>& plannningGroups, const string& groupName)
{
  plannningGroups.clear();
  if (groupName == "decomposed_body")
  {
    plannningGroups.push_back("lower_body");
    plannningGroups.push_back("torso");
    plannningGroups.push_back("head");
    plannningGroups.push_back("left_arm");
    plannningGroups.push_back("right_arm");
  }
  else
  {
    plannningGroups.push_back(groupName);
  }
}

void ItompPlannerNode::multiTrajectoryOptimization(const string& groupName,
    const sensor_msgs::JointState& jointGoalState)
{
  ros::WallTime create_time = ros::WallTime::now();

  fillGroupJointTrajectory(groupName, jointGoalState);

  int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
  optimizers_.resize(num_trajectories);

  for (int i = 0; i < num_trajectories; ++i)
  {
    const ItompPlanningGroup* group = robot_model_[i].getPlanningGroup(groupName);
    optimizers_[i] = new ItompOptimizer(i, threadTrajectories_[i], &robot_model_[i], group, planning_start_time_,
        trajectory_start_time_);
    optimizers_[i]->optimize();
  }
  last_planning_time_ = (ros::WallTime::now() - create_time).toSec();
  ROS_INFO("Optimization of group %s took %f sec", groupName.c_str(), last_planning_time_);
}

void ItompPlannerNode::updateTrajectoryToBestResult(const string& groupName)
{
  int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();

  last_min_cost_trajectory_ = 0;
  double best_cost = numeric_limits<double>::max();

  // find best cost result
  for (int i = 0; i < num_trajectories; ++i)
  {
    double threadCost = optimizers_[i]->getBestCost();
    if (threadCost < best_cost && optimizers_[i]->isSucceed())
    {
      last_min_cost_trajectory_ = i;
      best_cost = threadCost;
    }
  }

  // copy the result
  // TODO: can it be pointer swap?
  trajectory_->setTrajectory(threadTrajectories_[last_min_cost_trajectory_]->getTrajectory());
  trajectory_->setContactTrajectory(threadTrajectories_[last_min_cost_trajectory_]->getContactTrajectory());

  for (int i = 0; i < num_trajectories; ++i)
  {
    if (i != last_min_cost_trajectory_)
    {
      threadTrajectories_[i]->setTrajectory(trajectory_->getTrajectory());
      threadTrajectories_[i]->setContactTrajectory(trajectory_->getContactTrajectory());
    }
  }
}

void ItompPlannerNode::fillInResult(const std::vector<std::string>& planningGroups,
    planning_interface::MotionPlanResponse &res)
{
  const std::map<std::string, double>& joint_velocity_limits =
      PlanningParameters::getInstance()->getJointVelocityLimits();

  int num_all_joints = complete_initial_robot_state_->getVariableCount();

  res.trajectory_.reset(new robot_trajectory::RobotTrajectory(robot_model_[0].getRobotModel(), ""));

  std::vector<double> velocity_limits(num_all_joints, std::numeric_limits<double>::max());

  robot_state::RobotState ks = *complete_initial_robot_state_;
  std::vector<double> positions(num_all_joints);
  double duration = trajectory_->getDiscretization();
  for (std::size_t i = 0; i < trajectory_->getNumPoints(); ++i)
  {
    for (std::size_t j = 0; j < num_all_joints; j++)
    {
      positions[j] = (*trajectory_)(i, j);
    }

    ks.setVariablePositions(&positions[0]);
    // TODO: copy vel/acc
    ks.update();

    res.trajectory_->addSuffixWayPoint(ks, duration);
  }
  res.error_code_.val = moveit_msgs::MoveItErrorCodes::SUCCESS;

  /*
   // fill in joint names:
   res.trajectory_.joint_trajectory.joint_names.resize(num_all_joints);
   int k = 0;
   for (unsigned int i = 0; i < planningGroups.size(); ++i)
   {
   const ItompPlanningGroup* group = groups[i];
   for (int j = 0; j < group->num_joints_; ++j)
   {
   res.trajectory_.joint_trajectory.joint_names[k] = group->group_joints_[j].joint_name_;
   // try to retrieve the joint limits:
   if (joint_velocity_limits.find(res.trajectory_.joint_trajectory.joint_names[k])
   == joint_velocity_limits.end())
   {
   velocity_limits[k] = std::numeric_limits<double>::max();
   }
   else
   velocity_limits[k] = joint_velocity_limits.at(res.trajectory_.joint_trajectory.joint_names[k]);
   ++k;
   }
   }
   */

  /*
   // fill in the entire trajectory
   res.trajectory.joint_trajectory.points.resize(trajectory_->getNumPoints());
   for (int i = 0; i < trajectory_->getNumPoints(); ++i)
   {
   res.trajectory.joint_trajectory.points[i].positions.resize(num_all_joints);
   res.trajectory.joint_trajectory.points[i].velocities.resize(num_all_joints);
   res.trajectory.joint_trajectory.points[i].accelerations.resize(num_all_joints);
   int j = 0;
   for (j = 0; j < num_all_joints; j++)
   {
   int kdl_joint_index = robot_model_[0].urdfNameToKdlNumber(res.trajectory.joint_trajectory.joint_names[j]);
   res.trajectory.joint_trajectory.points[i].positions[j] = (*trajectory_)(i, kdl_joint_index);
   }

   if (i == 0)
   {
   res.trajectory.joint_trajectory.points[i].time_from_start = ros::Duration(0.0);
   for (int j = 0; j < num_all_joints; j++)
   {
   res.trajectory.joint_trajectory.points[i].velocities[j] = 0.0;
   res.trajectory.joint_trajectory.points[i].accelerations[j] = 0.0;
   }
   }
   else
   {
   double duration = trajectory_->getDiscretization();
   // check with all the joints if this duration is ok, else push it up
   for (int j = 0; j < num_all_joints; j++)
   {
   double d = fabs(
   res.trajectory.joint_trajectory.points[i].positions[j]
   - res.trajectory.joint_trajectory.points[i - 1].positions[j]) / velocity_limits[j];
   if (d > duration)
   duration = d;

   // set velocities
   res.trajectory.joint_trajectory.points[i].velocities[j] =
   (res.trajectory.joint_trajectory.points[i].positions[j]
   - res.trajectory.joint_trajectory.points[i - 1].positions[j]) / duration;

   // set accelerations
   res.trajectory.joint_trajectory.points[i].accelerations[j] =
   (res.trajectory.joint_trajectory.points[i].velocities[j]
   - res.trajectory.joint_trajectory.points[i - 1].velocities[j]) / duration;
   }
   res.trajectory.joint_trajectory.points[i].time_from_start =
   res.trajectory.joint_trajectory.points[i - 1].time_from_start + ros::Duration(duration);
   }
   }
   res.error_code.val = res.error_code.SUCCESS;
   */

  // print results
  if (PlanningParameters::getInstance()->getPrintPlanningInfo())
  {
    const std::vector<std::string>& joint_names = res.trajectory_->getFirstWayPoint().getVariableNames();
    for (int j = 0; j < num_all_joints; j++)
      printf("%s ", joint_names[j].c_str());
    printf("\n");
    for (int i = 0; i < trajectory_->getNumPoints(); ++i)
    {
      for (int j = 0; j < num_all_joints; j++)
      {
        printf("%f ", res.trajectory_->getWayPoint(i).getVariablePosition(j));
      }
      printf("\n");
    }
  }
}

void ItompPlannerNode::fillGroupJointTrajectory(const string& groupName, const sensor_msgs::JointState& jointGoalState)
{
  int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
  const ItompPlanningGroup* group = robot_model_[0].getPlanningGroup(groupName);

  // copy trajectory to thread trajectories
  for (int i = 0; i < num_trajectories; ++i)
  {
    threadTrajectories_[i]->setTrajectory(trajectory_->getTrajectory());
    threadTrajectories_[i]->setContactTrajectory(trajectory_->getContactTrajectory());
  }

  int goal_index = trajectory_->getNumPoints() - 1;
  Eigen::MatrixXd::RowXpr trajectory0GoalPoint = threadTrajectories_[0]->getTrajectoryPoint(goal_index);
  for (int i = 0; i < group->num_joints_; ++i)
  {
    string name = group->group_joints_[i].joint_name_;
    int kdl_number = robot_model_[0].urdfNameToKdlNumber(name);
    if (kdl_number >= 0)
    {
      trajectory0GoalPoint(kdl_number) = jointGoalState.position[kdl_number];
    }
  }

  for (int j = 1; j < num_trajectories; ++j)
  {
    threadTrajectories_[j]->getTrajectoryPoint(goal_index) = trajectory0GoalPoint;
  }

  vector<vector<double> > midPoints(num_trajectories);
  for (unsigned int i = 0; i < midPoints.size(); ++i)
    midPoints[i].resize(group->num_joints_);

  // create random number generator for each group joint.
  vector<boost::shared_ptr<boost::variate_generator<boost::mt19937, boost::uniform_real<double> > > > randomGenerator;
  randomGenerator.resize(group->num_joints_);
  boost::mt19937 rng;
  rng.seed(rand());
  for (int i = 0; i < group->num_joints_; i++)
  {
    double min = group->group_joints_[i].joint_limit_min_;
    double max = group->group_joints_[i].joint_limit_max_;
    randomGenerator[i].reset(
        new boost::variate_generator<boost::mt19937, boost::uniform_real<double> >(rng,
            boost::uniform_real<double>(min, max)));
  }

  // fill in mid point vector
  for (unsigned int i = 0; i < midPoints.size(); ++i)
  {
    for (unsigned int j = 0; j < midPoints[i].size(); ++j)
    {
      midPoints[i][j] = (*randomGenerator[j])();
    }
  }

  std::set<int> groupJointsKDLIndices;
  for (int i = 0; i < group->num_joints_; ++i)
  {
    groupJointsKDLIndices.insert(group->group_joints_[i].kdl_joint_index_);
  }
  threadTrajectories_[0]->fillInMinJerk(groupJointsKDLIndices, start_point_velocities_.row(0),
      start_point_accelerations_.row(0));

  for (int i = 1; i < num_trajectories; ++i)
  {
    // TODO: vel, acc
    threadTrajectories_[i]->fillInMinJerkWithMidPoint(midPoints[i], groupJointsKDLIndices, i);
  }
}

void ItompPlannerNode::printTrajectory(ItompCIOTrajectory* trajectory)
{
  for (int i = 0; i < trajectory->getNumPoints(); ++i)
  {
    for (int j = 0; j < trajectory->getNumJoints(); ++j)
    {
      printf("%f\t", (*trajectory)(i, j));
    }
    printf("\n");
  }
}

void ItompPlannerNode::resetPlanningInfo(int trials, int component)
{
  planning_info_.clear();
  planning_info_.resize(trials, std::vector<PlanningInfo>(component));
}

void ItompPlannerNode::writePlanningInfo(int trials, int component)
{
  PlanningInfo& info = planning_info_[trials][component];
  info.time = last_planning_time_;
  info.iterations = optimizers_[last_min_cost_trajectory_]->getLastIteration() + 1;
  info.cost = optimizers_[last_min_cost_trajectory_]->getBestCost();
  info.success = (optimizers_[last_min_cost_trajectory_]->isSucceed() ? 1 : 0);
}

void ItompPlannerNode::printPlanningInfoSummary()
{
  int numPlannings = planning_info_.size();
  int numComponents = planning_info_[0].size();

  vector<PlanningInfo> summary(numComponents);
  PlanningInfo sumOfSum;
  for (int j = 0; j < numComponents; ++j)
  {
    for (int i = 0; i < numPlannings; ++i)
    {
      summary[j] += planning_info_[i][j];
    }
    sumOfSum += summary[j];
  }

  // compute success rate
  // if a component fails, that trail fails.
  int numSuccess = 0;
  for (int i = 0; i < numPlannings; ++i)
  {
    bool failed = false;
    for (int j = 0; j < numComponents; ++j)
    {
      if (planning_info_[i][j].success == 0)
      {
        failed = true;
        break;
      }
    }
    if (!failed)
    {
      ++numSuccess;
    }
  }

  printf("%d Trials, %d components\n", numPlannings, numComponents);
  printf("Component Iterations Time Smoothness SuccessRate\n");
  for (int j = 0; j < numComponents; ++j)
  {
    printf("%d %f %f %f %f\n", j, ((double) summary[j].iterations) / numPlannings,
        ((double) summary[j].time) / numPlannings, ((double) summary[j].cost) / numPlannings,
        ((double) summary[j].success) / numPlannings);
  }
  printf("Sum %f %f %f %f\n", ((double) sumOfSum.iterations) / numPlannings, ((double) sumOfSum.time) / numPlannings,
      ((double) sumOfSum.cost) / numPlannings, ((double) numSuccess) / numPlannings);
  printf("\n");

  printf("plannings info\n");
  printf("Component Iterations Time Smoothness SuccessRate\n");
  for (int i = 0; i < numPlannings; ++i)
  {
    double iterationsSum = 0, timeSum = 0, costSum = 0;
    for (int j = 0; j < numComponents; ++j)
    {
      iterationsSum += planning_info_[i][j].iterations;
      timeSum += planning_info_[i][j].time;
      costSum += planning_info_[i][j].cost;
    }
    printf("[%d] %f %f %f \n", i, iterationsSum, timeSum, costSum);

  }
}

} // namespace
