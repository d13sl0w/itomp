#include <ros/ros.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit_msgs/PlanningScene.h>
#include <itomp_cio_planner/optimization/new_eval_manager.h>
#include <itomp_cio_planner/trajectory/trajectory_factory.h>
#include <itomp_cio_planner/cost/trajectory_cost_manager.h>
#include <itomp_cio_planner/model/itomp_planning_group.h>
#include <itomp_cio_planner/model/rbdl_model_util.h>
#include <itomp_cio_planner/contact/ground_manager.h>
#include <itomp_cio_planner/contact/contact_util.h>
#include <itomp_cio_planner/visualization/new_viz_manager.h>
#include <itomp_cio_planner/util/min_jerk_trajectory.h>
#include <itomp_cio_planner/util/planning_parameters.h>
#include <itomp_cio_planner/util/vector_util.h>
#include <itomp_cio_planner/util/multivariate_gaussian.h>
#include <itomp_cio_planner/util/exponential_map.h>
#include <visualization_msgs/MarkerArray.h>

using namespace std;
using namespace Eigen;

namespace itomp_cio_planner
{

const NewEvalManager* NewEvalManager::ref_evaluation_manager_ = NULL;

NewEvalManager::NewEvalManager() :
    last_trajectory_feasible_(false),
    parameter_modified_(true),
    best_cost_(std::numeric_limits<double>::max())
{
    if (ref_evaluation_manager_ == NULL)
        ref_evaluation_manager_ = this;
}

NewEvalManager::NewEvalManager(const NewEvalManager& manager)
    : robot_model_(manager.robot_model_),
      planning_scene_(manager.planning_scene_),
      planning_group_(manager.planning_group_),
      planning_start_time_(manager.planning_start_time_),
      trajectory_start_time_(manager.trajectory_start_time_),
      last_trajectory_feasible_(manager.last_trajectory_feasible_),
      parameter_modified_(manager.parameter_modified_),
      best_cost_(manager.best_cost_),
      rbdl_models_(manager.rbdl_models_),
      joint_torques_(manager.joint_torques_),
      external_forces_(manager.external_forces_),
      contact_variables_(manager.contact_variables_),
      evaluation_cost_matrix_(manager.evaluation_cost_matrix_)
{
    full_trajectory_.reset(manager.full_trajectory_->createClone());
    full_trajectory_const_ = full_trajectory_;
    parameter_trajectory_.reset(TrajectoryFactory::getInstance()->CreateParameterTrajectory(full_trajectory_, planning_group_));
    parameter_trajectory_const_ = parameter_trajectory_;
    itomp_trajectory_.reset(new ItompTrajectory(*manager.getTrajectory()));
    itomp_trajectory_const_ = itomp_trajectory_;

    robot_state_.resize(itomp_trajectory_->getNumPoints());
    for (int i = 0; i < itomp_trajectory_->getNumPoints(); ++i)
        robot_state_[i].reset(new robot_state::RobotState(*manager.robot_state_[i]));

    const collision_detection::WorldPtr world(new collision_detection::World(*planning_scene_->getWorld()));
    collision_world_derivatives_.reset(new CollisionWorldFCLDerivatives(
                                           dynamic_cast<const collision_detection::CollisionWorldFCL&>(*planning_scene_->getCollisionWorld()), world));
    collision_robot_derivatives_.reset(new CollisionRobotFCLDerivatives(
                                           dynamic_cast<const collision_detection::CollisionRobotFCL&>(*planning_scene_->getCollisionRobotUnpadded())));
    collision_robot_derivatives_->constructInternalFCLObject(planning_scene_->getCurrentState());
}

NewEvalManager::~NewEvalManager()
{
}

NewEvalManager& NewEvalManager::operator=(const NewEvalManager& manager)
{
    robot_model_ = manager.robot_model_;
    planning_scene_ = manager.planning_scene_;
    planning_group_ = manager.planning_group_;
    planning_start_time_ = manager.planning_start_time_;
    trajectory_start_time_ = manager.trajectory_start_time_;
    last_trajectory_feasible_ = manager.last_trajectory_feasible_;
    parameter_modified_ = manager.parameter_modified_;
    best_cost_ = manager.best_cost_;
    rbdl_models_ = manager.rbdl_models_;
    joint_torques_ = manager.joint_torques_;
    external_forces_ = manager.external_forces_;
    contact_variables_ = manager.contact_variables_;
    evaluation_cost_matrix_ = manager.evaluation_cost_matrix_;

    // allocate
    full_trajectory_.reset(manager.full_trajectory_->createClone());
    full_trajectory_const_ = full_trajectory_;
    parameter_trajectory_.reset(TrajectoryFactory::getInstance()->CreateParameterTrajectory(full_trajectory_, planning_group_));
    parameter_trajectory_const_ = parameter_trajectory_;
    itomp_trajectory_.reset(new ItompTrajectory(*manager.getTrajectory()));
    itomp_trajectory_const_ = itomp_trajectory_;

    robot_state_.resize(itomp_trajectory_->getNumPoints());
    for (int i = 0; i < itomp_trajectory_->getNumPoints(); ++i)
        robot_state_[i].reset(new robot_state::RobotState(*manager.robot_state_[i]));

    const collision_detection::WorldPtr world(new collision_detection::World(*planning_scene_->getWorld()));
    collision_world_derivatives_.reset(new CollisionWorldFCLDerivatives(
                                           dynamic_cast<const collision_detection::CollisionWorldFCL&>(*planning_scene_->getCollisionWorld()), world));
    collision_robot_derivatives_.reset(new CollisionRobotFCLDerivatives(
                                           dynamic_cast<const collision_detection::CollisionRobotFCL&>(*planning_scene_->getCollisionRobotUnpadded())));
    collision_robot_derivatives_->constructInternalFCLObject(planning_scene_->getCurrentState());

    return *this;
}

void NewEvalManager::initialize(const FullTrajectoryPtr& full_trajectory,
                                const ItompTrajectoryPtr& itomp_trajectory,
                                const ItompRobotModelConstPtr& robot_model,
                                const planning_scene::PlanningSceneConstPtr& planning_scene,
                                const ItompPlanningGroupConstPtr& planning_group,
                                double planning_start_time, double trajectory_start_time,
                                const moveit_msgs::Constraints& path_constraints)
{
    full_trajectory_const_ = full_trajectory_ = full_trajectory;
    itomp_trajectory_const_ = itomp_trajectory_ = itomp_trajectory;

	robot_model_ = robot_model;
	planning_scene_ = planning_scene;
	planning_group_ = planning_group;

	planning_start_time_ = planning_start_time;
	trajectory_start_time_ = trajectory_start_time;

	TrajectoryCostManager::getInstance()->buildActiveCostFunctions(this);
    evaluation_cost_matrix_.setZero(full_trajectory_->getNumPoints(), TrajectoryCostManager::getInstance()->getNumActiveCostFunctions());

    int num_joints = full_trajectory_->getComponentSize(FullTrajectory::TRAJECTORY_COMPONENT_JOINT);
    rbdl_models_.resize(full_trajectory_->getNumPoints(), robot_model_->getRBDLRobotModel());
    joint_torques_.resize(full_trajectory_->getNumPoints(), Eigen::VectorXd(num_joints));
	external_forces_.resize(full_trajectory_->getNumPoints(),
                            std::vector<RigidBodyDynamics::Math::SpatialVector>(robot_model_->getRBDLRobotModel().mBodies.size(), RigidBodyDynamics::Math::SpatialVectorZero));

	robot_state_.resize(full_trajectory_->getNumPoints());
	for (int i = 0; i < full_trajectory_->getNumPoints(); ++i)
        robot_state_[i].reset(new robot_state::RobotState(robot_model_->getMoveitRobotModel()));

	initializeContactVariables();

    parameter_trajectory_.reset(TrajectoryFactory::getInstance()->CreateParameterTrajectory(full_trajectory_, planning_group));
    parameter_trajectory_const_ = parameter_trajectory_;

    itomp_trajectory_->computeParameterToTrajectoryIndexMap(robot_model, planning_group);
    itomp_trajectory_->interpolateKeyframes(planning_group);

    const collision_detection::WorldPtr world(new collision_detection::World(*planning_scene_->getWorld()));
    collision_world_derivatives_.reset(new CollisionWorldFCLDerivatives(
                                           dynamic_cast<const collision_detection::CollisionWorldFCL&>(*planning_scene_->getCollisionWorld()), world));
    collision_robot_derivatives_.reset(new CollisionRobotFCLDerivatives(
                                           dynamic_cast<const collision_detection::CollisionRobotFCL&>(*planning_scene_->getCollisionRobotUnpadded())));
    collision_robot_derivatives_->constructInternalFCLObject(planning_scene_->getCurrentState());
}

double NewEvalManager::evaluate()
{
	if (parameter_modified_)
	{
        full_trajectory_->updateFromParameterTrajectory(parameter_trajectory_, planning_group_);
		parameter_modified_ = false;
	}

    performFullForwardKinematicsAndDynamics(0, full_trajectory_->getNumPoints());

    std::vector<TrajectoryCostPtr>& cost_functions = TrajectoryCostManager::getInstance()->getCostFunctionVector();
	for (int c = 0; c < cost_functions.size(); ++c)
	{
		cost_functions[c]->preEvaluate(this);
	}

    last_trajectory_feasible_ = evaluatePointRange(0, full_trajectory_->getNumPoints(), evaluation_cost_matrix_);

	for (int c = 0; c < cost_functions.size(); ++c)
	{
		cost_functions[c]->postEvaluate(this);
	}

	return getTrajectoryCost();
}

void NewEvalManager::computeDerivatives(
	const std::vector<Eigen::MatrixXd>& parameters, int type, int point,
	double* out, double eps, double* d_p, double* d_m, std::vector<std::vector<double> >* cost_der)
{
	//debug_ = true;

	setParameters(parameters);
    full_trajectory_->updateFromParameterTrajectory(parameter_trajectory_, planning_group_);
    parameter_modified_ = false;

    int num_cost_functions = TrajectoryCostManager::getInstance()->getNumActiveCostFunctions();

	for (int i = 0; i < parameter_trajectory_->getNumElements(); ++i)
	{
		const double value = parameters[type](point, i);
		int begin, end;

		evaluateParameterPoint(value + eps, type, point, i, begin, end, true);
        const double delta_plus = (evaluation_cost_matrix_.block(begin, 0, end - begin, num_cost_functions).sum());

		if (cost_der)
		{
			for (int j = 0; j < num_cost_functions; ++j)
			{
				double dp = evaluation_cost_matrix_.block(begin, j, end - begin, 1).sum();
				(*cost_der)[j][i] = dp;
			}
		}

		evaluateParameterPoint(value - eps, type, point, i, begin, end, false);
        const double delta_minus = (evaluation_cost_matrix_.block(begin, 0, end - begin, num_cost_functions).sum());


		*(out + i) = (delta_plus - delta_minus) / (2 * eps);

		*(d_p + i) = delta_plus;
		*(d_m + i) = delta_minus;

		if (cost_der)
		{
			for (int j = 0; j < num_cost_functions; ++j)
			{
				double dp = (*cost_der)[j][i];
				double dm = evaluation_cost_matrix_.block(begin, j, end - begin, 1).sum();
				(*cost_der)[j][i] = (dp - dm) / (2 * eps);
			}
		}

		full_trajectory_->restoreBackupTrajectories();
	}

	//debug_ = false;
}

void NewEvalManager::computeDerivatives(int parameter_index, const ItompTrajectory::ParameterVector& parameters,
                                        double* derivative_out, double eps)
{
    int num_cost_functions = TrajectoryCostManager::getInstance()->getNumActiveCostFunctions();

    unsigned int point_begin, point_end;
    const double value = parameters(parameter_index, 0);

    evaluateParameterPointItomp(value + eps, parameter_index, point_begin, point_end, true);
    const double delta_plus = (evaluation_cost_matrix_.block(point_begin, 0, point_end - point_begin, num_cost_functions).sum());

    evaluateParameterPointItomp(value - eps, parameter_index, point_begin, point_end, false);
    const double delta_minus = (evaluation_cost_matrix_.block(point_begin, 0, point_end - point_begin, num_cost_functions).sum());

    *(derivative_out + parameter_index) = (delta_plus - delta_minus) / (2 * eps);

    itomp_trajectory_->restoreTrajectory();
}

void NewEvalManager::computeCostDerivatives(int parameter_index, const ItompTrajectory::ParameterVector& parameters,
        double* derivative_out, std::vector<double*>& cost_derivative_out, double eps)
{
    int num_cost_functions = TrajectoryCostManager::getInstance()->getNumActiveCostFunctions();

    unsigned int point_begin, point_end;
    const double value = parameters(parameter_index, 0);

    std::vector<double> cost_delta_plus(num_cost_functions);
    std::vector<double> cost_delta_minus(num_cost_functions);

    evaluateParameterPointItomp(value + eps, parameter_index, point_begin, point_end, true);
    const double delta_plus = (evaluation_cost_matrix_.block(point_begin, 0, point_end - point_begin, num_cost_functions).sum());
    for (int i = 0; i < num_cost_functions; ++i)
        cost_delta_plus[i] = (evaluation_cost_matrix_.block(point_begin, i, point_end - point_begin, 1).sum());

    evaluateParameterPointItomp(value - eps, parameter_index, point_begin, point_end, false);
    const double delta_minus = (evaluation_cost_matrix_.block(point_begin, 0, point_end - point_begin, num_cost_functions).sum());
    for (int i = 0; i < num_cost_functions; ++i)
        cost_delta_minus[i] = (evaluation_cost_matrix_.block(point_begin, i, point_end - point_begin, 1).sum());

    *(derivative_out + parameter_index) = (delta_plus - delta_minus) / (2 * eps);
    for (int i = 0; i < num_cost_functions; ++i)
        *(cost_derivative_out[i] + parameter_index) = (cost_delta_plus[i] - cost_delta_minus[i]) / (2 * eps);

    itomp_trajectory_->restoreTrajectory();
}

void NewEvalManager::evaluateParameterPoint(double value, int type, int point,
		int element, int& full_point_begin, int& full_point_end, bool first)
{
	full_trajectory_->directChangeForDerivatives(value, planning_group_, type,
			point, element, full_point_begin, full_point_end, first);

	performPartialForwardKinematicsAndDynamics(full_point_begin, full_point_end,
			element);

	evaluatePointRange(full_point_begin, full_point_end,
					   evaluation_cost_matrix_, type, element);
}

void NewEvalManager::evaluateParameterPointItomp(double value, int parameter_index,
        unsigned int& point_begin, unsigned int& point_end, bool first)
{
    itomp_trajectory_->directChangeForDerivativeComputation(parameter_index, value, point_begin, point_end, first);

    const ItompTrajectoryIndex& index = itomp_trajectory_->getTrajectoryIndex(parameter_index);

    if (index.point == point_end)
        ++point_end;

    performPartialForwardKinematicsAndDynamics(point_begin, point_end, index);

    evaluatePointRange(point_begin, point_end, evaluation_cost_matrix_, index);
}

bool NewEvalManager::evaluatePointRange(int point_begin, int point_end,
                                        Eigen::MatrixXd& cost_matrix,
                                        const ItompTrajectoryIndex& index)
{
    bool is_feasible = true;

    const std::vector<TrajectoryCostPtr>& cost_functions =
        TrajectoryCostManager::getInstance()->getCostFunctionVector();

    // cost weight changed
    if (cost_functions.size() != cost_matrix.cols())
        cost_matrix = Eigen::MatrixXd::Zero(cost_matrix.rows(),	cost_functions.size());

    for (int c = 0; c < cost_functions.size(); ++c)
    {
        if (cost_functions[c]->isInvariant(this, index))
        {
            for (int i = point_begin; i < point_end; ++i)
                cost_matrix(i, c) = 0.0;
        }
        else
        {
            for (int i = point_begin; i < point_end; ++i)
            {
                double cost = 0.0;

                is_feasible &= cost_functions[c]->evaluate(this, i, cost);

                cost_matrix(i, c) = cost_functions[c]->getWeight() * cost;
            }
        }
    }
    is_feasible = false;
    return is_feasible;
}

bool NewEvalManager::evaluatePointRange(int point_begin, int point_end,
										Eigen::MatrixXd& cost_matrix, int type, int element)
{
	bool is_feasible = true;

	const std::vector<TrajectoryCostPtr>& cost_functions =
		TrajectoryCostManager::getInstance()->getCostFunctionVector();

	// cost weight changed
	if (cost_functions.size() != cost_matrix.cols())
        cost_matrix = Eigen::MatrixXd::Zero(cost_matrix.rows(),	cost_functions.size());

	for (int c = 0; c < cost_functions.size(); ++c)
	{
        /*
		if (cost_functions[c]->isInvariant(this, type, element))
		{
			for (int i = point_begin; i < point_end; ++i)
				cost_matrix(i, c) = 0.0;
		}
		else
        */
		{
			for (int i = point_begin; i < point_end; ++i)
			{
				double cost = 0.0;

				is_feasible &= cost_functions[c]->evaluate(this, i, cost);

				cost_matrix(i, c) = cost_functions[c]->getWeight() * cost;
			}
		}
	}
	is_feasible = false;
	return is_feasible;
}

void NewEvalManager::render()
{
	bool is_best = (getTrajectoryCost() <= best_cost_);
	if (PlanningParameters::getInstance()->getAnimatePath())
    {
        NewVizManager::getInstance()->animatePath(itomp_trajectory_, robot_state_[0], is_best);

        if (is_best)
            NewVizManager::getInstance()->displayTrajectory(itomp_trajectory_);
    }

	if (PlanningParameters::getInstance()->getAnimateEndeffector())
	{
        NewVizManager::getInstance()->animateEndeffectors(itomp_trajectory_, rbdl_models_, is_best);
        NewVizManager::getInstance()->animateContacts(itomp_trajectory_, contact_variables_, rbdl_models_, is_best);
	}
}

void NewEvalManager::performFullForwardKinematicsAndDynamics(int point_begin, int point_end)
{
	TIME_PROFILER_START_TIMER(FK);

	int num_contacts = planning_group_->getNumContacts();

	for (int point = point_begin; point < point_end; ++point)
	{
        const Eigen::VectorXd& q = itomp_trajectory_->getElementTrajectory(ItompTrajectory::COMPONENT_TYPE_POSITION,
                                   ItompTrajectory::SUB_COMPONENT_TYPE_JOINT)->getTrajectoryPoint(point);

        const Eigen::VectorXd& q_dot = itomp_trajectory_->getElementTrajectory(ItompTrajectory::COMPONENT_TYPE_VELOCITY,
                                       ItompTrajectory::SUB_COMPONENT_TYPE_JOINT)->getTrajectoryPoint(point);

        const Eigen::VectorXd& q_ddot = itomp_trajectory_->getElementTrajectory(ItompTrajectory::COMPONENT_TYPE_ACCELERATION,
                                        ItompTrajectory::SUB_COMPONENT_TYPE_JOINT)->getTrajectoryPoint(point);

		// compute contact variables
        itomp_trajectory_->getContactVariables(point, contact_variables_[point]);
		for (int i = 0; i < num_contacts; ++i)
		{
            const Eigen::Vector3d contact_position = contact_variables_[point][i].getPosition();
            const Eigen::Vector3d contact_orientation = contact_variables_[point][i].getOrientation();

			Eigen::Vector3d contact_normal, proj_position, proj_orientation;
            GroundManager::getInstance()->getNearestGroundPosition(contact_position, contact_orientation,
                    proj_position, proj_orientation, contact_normal);

            int rbdl_endeffector_id = planning_group_->contact_points_[i].getRBDLBodyId();

            contact_variables_[point][i].ComputeProjectedPointPositions(proj_position, proj_orientation,
                    rbdl_models_[point], planning_group_->contact_points_[i]);
		}

		// compute external forces
		for (int i = 0; i < num_contacts; ++i)
		{
			for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
			{
                int rbdl_point_id = planning_group_->contact_points_[i].getContactPointRBDLIds(c);

                Eigen::Vector3d point_position = contact_variables_[point][i].projected_point_positions_[c];

                Eigen::Vector3d contact_force = contact_variables_[point][i].getPointForce(c);

                Eigen::Vector3d contact_torque = point_position.cross(contact_force);

                RigidBodyDynamics::Math::SpatialVector& ext_force = external_forces_[point][rbdl_point_id];
				for (int j = 0; j < 3; ++j)
				{
					ext_force(j) = contact_torque(j);
					ext_force(j + 3) = contact_force(j);
				}
			}
		}

        updateFullKinematicsAndDynamics(rbdl_models_[point], q, q_dot, q_ddot, joint_torques_[point], &external_forces_[point]);
	}

	TIME_PROFILER_END_TIMER(FK);
}
void NewEvalManager::performPartialForwardKinematicsAndDynamics(int point_begin,
		int point_end, int parameter_element)
{
	TIME_PROFILER_START_TIMER(FK);

	if (!full_trajectory_->hasVelocity()
			|| !full_trajectory_->hasAcceleration())
		return;

	for (int point = point_begin; point < point_end; ++point)
	{
		rbdl_models_[point] = ref_evaluation_manager_->rbdl_models_[point];
	}

	bool dynamics_only = (parameter_element
						  >= parameter_trajectory_->getNumJoints());
	int num_contacts = planning_group_->getNumContacts();

	for (int point = point_begin; point < point_end; ++point)
	{
		const Eigen::VectorXd& q = full_trajectory_->getComponentTrajectory(
									   FullTrajectory::TRAJECTORY_COMPONENT_JOINT,
									   Trajectory::TRAJECTORY_TYPE_POSITION).row(point);

		const Eigen::VectorXd& q_dot = full_trajectory_->getComponentTrajectory(
										   FullTrajectory::TRAJECTORY_COMPONENT_JOINT,
										   Trajectory::TRAJECTORY_TYPE_VELOCITY).row(point);
		const Eigen::VectorXd& q_ddot =
			full_trajectory_->getComponentTrajectory(
				FullTrajectory::TRAJECTORY_COMPONENT_JOINT,
				Trajectory::TRAJECTORY_TYPE_ACCELERATION).row(point);

		if (dynamics_only)
		{
			// compute contact variables
			full_trajectory_->getContactVariables(point,
												  contact_variables_[point]);
			for (int i = 0; i < num_contacts; ++i)
			{
				const Eigen::Vector3d contact_position =
					contact_variables_[point][i].getPosition();
				const Eigen::Vector3d contact_orientation =
					contact_variables_[point][i].getOrientation();

				Eigen::Vector3d contact_normal, proj_position, proj_orientation;
				GroundManager::getInstance()->getNearestGroundPosition(
					contact_position, contact_orientation, proj_position,
					proj_orientation, contact_normal);

				int rbdl_endeffector_id =
					planning_group_->contact_points_[i].getRBDLBodyId();

				contact_variables_[point][i].ComputeProjectedPointPositions(
					proj_position, proj_orientation, rbdl_models_[point],
					planning_group_->contact_points_[i]);
			}

			// compute external forces
			for (int i = 0; i < num_contacts; ++i)
			{
				for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
				{
					int rbdl_point_id =
						planning_group_->contact_points_[i].getContactPointRBDLIds(
							c);

					Eigen::Vector3d point_position =
						contact_variables_[point][i].projected_point_positions_[c];

					Eigen::Vector3d contact_force =
						contact_variables_[point][i].getPointForce(c);

					Eigen::Vector3d contact_torque = point_position.cross(
														 contact_force);

					RigidBodyDynamics::Math::SpatialVector& ext_force =
						external_forces_[point][rbdl_point_id];
					for (int j = 0; j < 3; ++j)
					{
						ext_force(j) = contact_torque(j);
						ext_force(j + 3) = contact_force(j);
					}
				}
			}

			updatePartialDynamics(rbdl_models_[point], q, q_dot, q_ddot,
                                  joint_torques_[point], &external_forces_[point]);
		}
		else
		{
			contact_variables_[point] =
				ref_evaluation_manager_->contact_variables_[point];
            joint_torques_[point] = ref_evaluation_manager_->joint_torques_[point];
			external_forces_[point] =
				ref_evaluation_manager_->external_forces_[point];

			updatePartialKinematicsAndDynamics(rbdl_models_[point], q, q_dot,
                                               q_ddot, joint_torques_[point], &external_forces_[point],
											   planning_group_->group_joints_[parameter_element].rbdl_affected_body_ids_);

		}
	}

	TIME_PROFILER_END_TIMER(FK);
}

void NewEvalManager::performPartialForwardKinematicsAndDynamics(int point_begin, int point_end, const ItompTrajectoryIndex& index)
{
    TIME_PROFILER_START_TIMER(FK);

    bool dynamics_only = (index.sub_component != ItompTrajectory::SUB_COMPONENT_TYPE_JOINT);
    int num_contacts = planning_group_->getNumContacts();

    // copy only variables will be updated
    for (int point = point_begin; point < point_end; ++point)
    {
        rbdl_models_[point].f = ref_evaluation_manager_->rbdl_models_[point].f;

        if (!dynamics_only)
        {
            rbdl_models_[point].X_lambda = ref_evaluation_manager_->rbdl_models_[point].X_lambda;
            rbdl_models_[point].X_base = ref_evaluation_manager_->rbdl_models_[point].X_base;
            rbdl_models_[point].v = ref_evaluation_manager_->rbdl_models_[point].v;
            rbdl_models_[point].a = ref_evaluation_manager_->rbdl_models_[point].a;
            rbdl_models_[point].c = ref_evaluation_manager_->rbdl_models_[point].c;
        }
    }

    const ElementTrajectoryPtr& pos_trajectory = itomp_trajectory_->getElementTrajectory(ItompTrajectory::COMPONENT_TYPE_POSITION,
            ItompTrajectory::SUB_COMPONENT_TYPE_JOINT);
    const ElementTrajectoryPtr& vel_trajectory = itomp_trajectory_->getElementTrajectory(ItompTrajectory::COMPONENT_TYPE_VELOCITY,
            ItompTrajectory::SUB_COMPONENT_TYPE_JOINT);
    const ElementTrajectoryPtr& acc_trajectory = itomp_trajectory_->getElementTrajectory(ItompTrajectory::COMPONENT_TYPE_ACCELERATION,
            ItompTrajectory::SUB_COMPONENT_TYPE_JOINT);

    for (int point = point_begin; point < point_end; ++point)
    {
        const Eigen::VectorXd& q = pos_trajectory->getTrajectoryPoint(point);
        const Eigen::VectorXd& q_dot = vel_trajectory->getTrajectoryPoint(point);
        const Eigen::VectorXd& q_ddot = acc_trajectory->getTrajectoryPoint(point);

        if (dynamics_only)
        {
            // compute contact variables
            itomp_trajectory_->getContactVariables(point, contact_variables_[point]);
            for (int i = 0; i < num_contacts; ++i)
            {
                const Eigen::Vector3d contact_position = contact_variables_[point][i].getPosition();
                const Eigen::Vector3d contact_orientation = contact_variables_[point][i].getOrientation();

                Eigen::Vector3d contact_normal, proj_position, proj_orientation;
                GroundManager::getInstance()->getNearestGroundPosition(contact_position, contact_orientation,
                        proj_position, proj_orientation,
                        contact_normal);

                int rbdl_endeffector_id = planning_group_->contact_points_[i].getRBDLBodyId();

                contact_variables_[point][i].ComputeProjectedPointPositions(proj_position, proj_orientation,
                        rbdl_models_[point], planning_group_->contact_points_[i]);
            }

            // compute external forces
            for (int i = 0; i < num_contacts; ++i)
            {
                for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
                {
                    int rbdl_point_id = planning_group_->contact_points_[i].getContactPointRBDLIds(c);

                    Eigen::Vector3d point_position = contact_variables_[point][i].projected_point_positions_[c];

                    Eigen::Vector3d contact_force = contact_variables_[point][i].getPointForce(c);

                    Eigen::Vector3d contact_torque = point_position.cross(contact_force);

                    RigidBodyDynamics::Math::SpatialVector& ext_force = external_forces_[point][rbdl_point_id];
                    for (int j = 0; j < 3; ++j)
                    {
                        ext_force(j) = contact_torque(j);
                        ext_force(j + 3) = contact_force(j);
                    }
                }
            }

            updatePartialDynamics(rbdl_models_[point], q, q_dot, q_ddot, joint_torques_[point], &external_forces_[point]);
        }
        else
        {
            contact_variables_[point] = ref_evaluation_manager_->contact_variables_[point];
            joint_torques_[point] = ref_evaluation_manager_->joint_torques_[point];
            external_forces_[point] = ref_evaluation_manager_->external_forces_[point];

            updatePartialKinematicsAndDynamics(rbdl_models_[point], q, q_dot,
                                               q_ddot, joint_torques_[point], &external_forces_[point],
                                               planning_group_->group_joints_[itomp_trajectory_->getParameterJointIndex(index.element)].rbdl_affected_body_ids_);

        }
    }

    TIME_PROFILER_END_TIMER(FK);
}

void NewEvalManager::getParameters(
	std::vector<Eigen::MatrixXd>& parameters) const
{
	parameters[Trajectory::TRAJECTORY_TYPE_POSITION] =
		parameter_trajectory_->getTrajectory(
			Trajectory::TRAJECTORY_TYPE_POSITION);

	if (parameter_trajectory_->hasVelocity())
		parameters[Trajectory::TRAJECTORY_TYPE_VELOCITY] =
			parameter_trajectory_->getTrajectory(
				Trajectory::TRAJECTORY_TYPE_VELOCITY);
}

void NewEvalManager::setParameters(
	const std::vector<Eigen::MatrixXd>& parameters)
{
	parameter_trajectory_->getTrajectory(Trajectory::TRAJECTORY_TYPE_POSITION) =
		parameters[Trajectory::TRAJECTORY_TYPE_POSITION];

	if (parameter_trajectory_->hasVelocity())
		parameter_trajectory_->getTrajectory(
			Trajectory::TRAJECTORY_TYPE_VELOCITY) =
				parameters[Trajectory::TRAJECTORY_TYPE_VELOCITY];

	setParameterModified();
}

void NewEvalManager::getParameters(ItompTrajectory::ParameterVector& parameters) const
{
    itomp_trajectory_->getParameters(parameters);
}

void NewEvalManager::setParameters(const ItompTrajectory::ParameterVector& parameters)
{
    itomp_trajectory_->setParameters(parameters, planning_group_);
}

void NewEvalManager::printTrajectoryCost(int iteration, bool details)
{
    cout.precision(std::numeric_limits<double>::digits10);

	double cost = evaluation_cost_matrix_.sum();

    double old_best = best_cost_;

	bool is_best = cost < best_cost_;
	if (is_best)
		best_cost_ = cost;

	const std::vector<TrajectoryCostPtr>& cost_functions =
		TrajectoryCostManager::getInstance()->getCostFunctionVector();



	if (!details || !is_best)
	{
	}
	else
	{

        cout << "[" << iteration << "] Trajectory cost : " << fixed << old_best << " -> " << fixed << best_cost_ << std::endl;

        static std::vector<double> cost_vec[3];

        for (int c = 0; c < cost_functions.size(); ++c)
		{
            double sub_cost = evaluation_cost_matrix_.col(c).sum();
            cout << cost_functions[c]->getName() << " : " << fixed << sub_cost << std::endl;

            cost_vec[c].push_back(sub_cost);
        }

        if (cost_vec[0].size() == 15000)
        {
            for (int i = 0; i < 15000; ++i)
            {
                cout << i << " : ";
                for (int c = 0; c < cost_functions.size(); ++c)
                {
                    cout << cost_vec[c][i] << " ";
                }
                cout << endl;
            }
        }




        for (int c = 0; c < cost_functions.size(); ++c)
        {
            cout << cost_functions[c]->getName() << " : ";
            for (int i = 0; i < itomp_trajectory_->getNumPoints(); ++i)
            {
                double cost = evaluation_cost_matrix_(i, c);
                std::cout << fixed << cost << " ";
            }
            std::cout << std::endl;
		}

        /*
        for (int i = 0; i < 4; ++i)
        {
            std::vector<ContactVariables> cv(4);
            itomp_trajectory_->getContactVariables(1, cv);
            for (int j = 0; j < 4; ++j)
            {
                double c = getContactActiveValue(i, j, cv);

                std::cout << "Contact Force " << i << ":" << j << " " << c << " " << cv[i].getPointForce(j).transpose() << std::endl;
            }
        }
        */

        /*
        for (int p = 0; p < itomp_trajectory_->getNumPoints(); ++p)
        {
            std::cout << "Torques " << p << " : ";
            for (int i = 0; i < 6; ++i)
            {
                std::cout << fixed << joint_torques_[p](i) << " ";
            }
            std::cout << std::endl;
        }
        */


	}
}

void NewEvalManager::initializeContactVariables()
{
	int num_contacts = planning_group_->getNumContacts();
    ROS_ASSERT(num_contacts == PlanningParameters::getInstance()->getNumContacts());

	// allocate
	contact_variables_.resize(full_trajectory_->getNumPoints());
	for (int i = 0; i < contact_variables_.size(); ++i)
	{
		contact_variables_[i].resize(num_contacts);
	}

    if (!full_trajectory_->hasVelocity() || !full_trajectory_->hasAcceleration())
		return;

    for (int point = 0; point < full_trajectory_->getNumPoints(); ++point)
	{
        const Eigen::VectorXd& q = full_trajectory_->getComponentTrajectory(FullTrajectory::TRAJECTORY_COMPONENT_JOINT, Trajectory::TRAJECTORY_TYPE_POSITION).row(point);
        const Eigen::VectorXd& q_dot = full_trajectory_->getComponentTrajectory(FullTrajectory::TRAJECTORY_COMPONENT_JOINT, Trajectory::TRAJECTORY_TYPE_VELOCITY).row(point);
        const Eigen::VectorXd& q_ddot =	full_trajectory_->getComponentTrajectory(FullTrajectory::TRAJECTORY_COMPONENT_JOINT, Trajectory::TRAJECTORY_TYPE_ACCELERATION).row(point);

		Eigen::VectorXd tau(q.rows());

        updateFullKinematicsAndDynamics(rbdl_models_[point], q, q_dot, q_ddot, tau, NULL);

		/*
		 for (int i = 0; i < rbdl_models_[point].f.size(); ++i)
		 cout << i << " : " << rbdl_models_[point].f[i].transpose() << endl;
		 cout << tau.transpose() << endl;
		 */

		std::vector<RigidBodyDynamics::Math::SpatialVector> ext_forces;
        ext_forces.resize(rbdl_models_[point].mBodies.size(), RigidBodyDynamics::Math::SpatialVectorZero);

		for (int i = 0; i < num_contacts; ++i)
		{
            int rbdl_body_id = planning_group_->contact_points_[i].getRBDLBodyId();

			contact_variables_[point][i].setVariable(0.0);
            contact_variables_[point][i].setPosition(rbdl_models_[point].X_base[rbdl_body_id].r);
            contact_variables_[point][i].setOrientation(exponential_map::RotationToExponentialMap(rbdl_models_[point].X_base[rbdl_body_id].E));

			for (int j = 0; j < NUM_ENDEFFECTOR_CONTACT_POINTS; ++j)
			{
				Eigen::MatrixXd jacobian(6, q.rows());
				Eigen::Vector3d contact_position;
				Eigen::Vector3d contact_force;
				Eigen::Vector3d contact_torque;

                int rbdl_body_id = planning_group_->contact_points_[i].getContactPointRBDLIds(j);
                CalcFullJacobian(rbdl_models_[point], q, rbdl_body_id, Eigen::Vector3d::Zero(), jacobian, false);

				contact_position = rbdl_models_[point].X_base[rbdl_body_id].r;
				// set forces to 0
				contact_force = 0.0 / num_contacts * tau.block(0, 0, 3, 1);
				contact_torque = contact_position.cross(contact_force);

                RigidBodyDynamics::Math::SpatialVector& ext_force = ext_forces[rbdl_body_id];
                ext_force.set(contact_torque.coeff(0), contact_torque.coeff(1), contact_torque.coeff(2),
                              contact_force.coeff(0), contact_force.coeff(1), contact_force.coeff(2));

				contact_variables_[point][i].setPointForce(j, contact_force);
			}
		}

		// to validate
        RigidBodyDynamics::InverseDynamics(rbdl_models_[point], q, q_dot, q_ddot, tau, &ext_forces);

		/*
		 for (int i = 0; i < rbdl_models_[point].f.size(); ++i)
		 cout << i << " : " << rbdl_models_[point].f[i].transpose() << endl;
		 cout << tau.transpose() << endl;
		 */

		full_trajectory_->setContactVariables(point, contact_variables_[point]);
        itomp_trajectory_->setContactVariables(point, contact_variables_[point]);
	}
	full_trajectory_->interpolateContactVariables();
    itomp_trajectory_->interpolateStartEnd(ItompTrajectory::SUB_COMPONENT_TYPE_CONTACT_POSITION);
    itomp_trajectory_->interpolateStartEnd(ItompTrajectory::SUB_COMPONENT_TYPE_CONTACT_FORCE);
}

void NewEvalManager::resetBestTrajectoryCost()
{
    best_cost_ = std::numeric_limits<double>::max();
}

}

