/**
 *  MIT License
 *
 *  Copyright (c) 2019 Yuya Kudo
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

#include <Planner/InformedRRTStar/InformedRRTStar.h>

namespace planner {
    InformedRRTStar::InformedRRTStar(const uint32_t& dim,
                                     const uint32_t& max_sampling_num,
                                     const double&   goal_sampling_rate,
                                     const double&   expand_dist,
                                     const double&   R,
                                     const double&   goal_region_radius) :
        base::PlannerBase(dim),
        max_sampling_num_(max_sampling_num),
        expand_dist_(expand_dist),
        R_(R),
        goal_region_radius_(goal_region_radius) {
        setGoalSamplingRate(goal_sampling_rate);
    }

    InformedRRTStar::~InformedRRTStar() {
    }

    void InformedRRTStar::setMaxSamplingNum(const uint32_t& max_sampling_num) {
        max_sampling_num_ = max_sampling_num;
    }

    void InformedRRTStar::setGoalSamplingRate(const double& goal_sampling_rate) {
        if(!(0.0 <= goal_sampling_rate && goal_sampling_rate <= 1.0)) {
            throw std::invalid_argument("[" + std::string(__PRETTY_FUNCTION__) + "] " +
                                        "Rate of Sampling goal state is invalid");
        }

        goal_sampling_rate_ = goal_sampling_rate;
    }

    void InformedRRTStar::setExpandDist(const double& expand_dist) {
        expand_dist_ = expand_dist;
    }

    void InformedRRTStar::setR(const double& R) {
        R_ = R;
    }

    void InformedRRTStar::setGoalRegionRadius(const double& goal_region_radius) {
        goal_region_radius_ = goal_region_radius;
    }

    bool InformedRRTStar::solve(const State& start, const State& goal) {
        // definition of random device
        std::random_device rand_dev;
        std::minstd_rand rand(static_cast<unsigned int>(rand_dev()));

        // definition of constraint of generating random value in euclidean space
        std::vector<std::uniform_real_distribution<double>> rand_restrictions;
        rand_restrictions.reserve(constraint_->space.getDim());
        for(size_t di = 1; di <= constraint_->space.getDim(); di++) {
            rand_restrictions.emplace_back(constraint_->space.getBound(di).low,
                                           constraint_->space.getBound(di).high);
        }

        // definition of random device in order to sample goal state with a certain probability
        auto sample_restriction = std::uniform_real_distribution<>(0, 1.0);

        // definition of set of node
        std::vector<std::shared_ptr<Node>> node_list;
        node_list.reserve(max_sampling_num_);
        node_list.push_back(std::make_shared<Node>(start, nullptr, 0));

        // definition of index of set of node which exist on goal region
        std::vector<size_t> goal_node_indexes;

        // definition of direct distance between start and goal
        auto min_cost = goal.distanceFrom(start);

        // definition of center state between start and goal
        auto center_v = ((start + goal) / 2).vals;
        center_v.push_back(0.0);
        auto center = Eigen::Map<Eigen::VectorXd>(&center_v[0], center_v.size());

        // definition of rotation matrix
        auto rotate_mat = calcRotationToWorldFlame(start, goal);

        // sampling on euclidean space
        for(size_t i = 0; i < max_sampling_num_; i++) {

            // get best cost in the set of node which exist on goal region
            auto best_cost = std::numeric_limits<double>::max();
            for(const auto& goal_node_index : goal_node_indexes) {
                best_cost = std::min(best_cost, node_list[goal_node_index]->cost);
            }

            // sampling node
            auto rand_node = std::make_shared<Node>(goal, nullptr, 0);
            if(goal_sampling_rate_ < sample_restriction(rand)) {
                if(best_cost == std::numeric_limits<double>::max()) {
                    for(size_t i = 0; i < constraint_->space.getDim(); i++) {
                        rand_node->state.vals[i] = rand_restrictions[i](rand);
                    }
                }
                else {
                    //--- random sampling on heuristic domain
                    // diagonal element
                    std::vector<double> diag_v(constraint_->space.getDim() + 1,
                                               std::sqrt(std::pow(best_cost, 2) - std::pow(min_cost, 2)) / 2.0);
                    diag_v[0] = best_cost / 2.0;

                    // random sampling on unit n-ball
                    auto x_ball_state = sampleUnitNBall(constraint_->space.getDim());
                    auto x_ball_v = x_ball_state.vals;
                    x_ball_v.push_back(0.0);

                    // trans sampling pt
                    auto rand =
                        rotate_mat *
                        Eigen::Map<Eigen::VectorXd>(&*diag_v.begin(), diag_v.size()).asDiagonal() *
                        Eigen::Map<Eigen::VectorXd>(&*x_ball_v.begin(), x_ball_v.size()) +
                        center;

                    auto row_i = 0;
                    for(auto& val : rand_node->state.vals) {
                        val = rand(row_i, 0);
                        row_i++;
                    }
                }

                // resample when node dose not meet constraint
                if(constraint_->checkConstraintType(rand_node->state) == ConstraintType::NOENTRY) {
                    continue;
                }
            }

            // get index of node that nearest node from sampling node
            auto nearest_node_index = getNearestNodeIndex(rand_node, node_list);

            // generate new node
            auto new_node = generateSteerNode(node_list[nearest_node_index], rand_node, expand_dist_);

            // add to list if new node meets constraint
            if(constraint_->checkCollision(node_list[nearest_node_index]->state, new_node->state)) {
                // Find nodes that exist on certain domain
                auto near_node_indexes = findNearNodes(new_node, node_list);

                // Choose parent node from near node
                new_node = chooseParentNode(new_node, node_list, near_node_indexes);

                // add node to list
                node_list.push_back(new_node);

                // redefine parent node of near node
                rewireNearNodes(node_list, near_node_indexes);

                if(new_node->state.distanceFrom(goal) < goal_region_radius_) {
                    goal_node_indexes.push_back(node_list.size() - 1);
                }
            }
        }

        result_.clear();

        // store the result
        auto best_last_index = getBestNodeIndex(goal, expand_dist_, node_list);
        if(best_last_index < 0) {
            return false;
        }
        else {
            std::shared_ptr<base::NodeBase> result_node = node_list[best_last_index];

            result_cost_ = node_list[best_last_index]->cost + result_node->state.distanceFrom(goal);
            if(result_node->state != goal) {
                result_.push_back(goal);
            }

            while(true) {
                auto result_begin_itr = result_.begin();
                result_.insert(result_begin_itr, result_node->state);

                if(result_node->parent == nullptr) {
                    break;
                }

                result_node = result_node->parent;
            }
        }

        // store the node list
        node_list_.clear();
        std::move(node_list.begin(), node_list.end(), std::back_inserter(node_list_));

        return true;
    }

    size_t InformedRRTStar::getNearestNodeIndex(const std::shared_ptr<Node>& target_node,
                                                const std::vector<std::shared_ptr<Node>>& node_list) const {
        auto min_dist_index = 0;
        auto min_dist       = std::numeric_limits<double>::max();
        for(size_t i = 0; i < node_list.size(); i++) {
            auto dist = node_list[i]->state.distanceFrom(target_node->state);
            if(dist < min_dist) {
                min_dist = dist;
                min_dist_index = i;
            }
        }

        return min_dist_index;
    }

    std::shared_ptr<InformedRRTStar::Node> InformedRRTStar::generateSteerNode(const std::shared_ptr<Node>& src_node,
                                                                              const std::shared_ptr<Node>& dst_node,
                                                                              const double& expand_dist) const {
        auto steered_node = std::make_shared<Node>(src_node->state, src_node, src_node->cost);

        if(src_node->state.distanceFrom(dst_node->state) < expand_dist) {
            steered_node->cost  += src_node->state.distanceFrom(dst_node->state);
            steered_node->state  = dst_node->state;
        }
        else {
            steered_node->cost += expand_dist;

            auto src = src_node->state;
            auto dst = dst_node->state;

            auto dim_expand_dist = expand_dist;
            for(int i = constraint_->space.getDim() - 1; 0 < i; i--) {
                auto dist_delta_dim = dst.vals.back() - src.vals.back();
                src.vals.pop_back();
                dst.vals.pop_back();
                auto dist_lower_dim = (i != 1) ? dst.distanceFrom(src) : dst.vals.front() - src.vals.front();

                auto t = std::atan2(dist_delta_dim, dist_lower_dim);

                steered_node->state.vals[i] += dim_expand_dist * std::sin(t);
                dim_expand_dist              = dim_expand_dist * std::cos(t);
            }
            steered_node->state.vals[0] += dim_expand_dist;
        }

        return steered_node;
    }

    std::vector<size_t> InformedRRTStar::findNearNodes(const std::shared_ptr<Node>&              target_node,
                                                       const std::vector<std::shared_ptr<Node>>& node_list) const {
        std::vector<size_t> near_node_indexes;

        auto num_node = node_list.size();
        if(num_node != 0) {
            auto radius = R_ * std::pow((std::log(num_node) / num_node), 1.0 / constraint_->space.getDim());
            for(size_t i = 0; i < num_node; i++) {
                auto dist = node_list[i]->state.distanceFrom(target_node->state);
                if(dist < radius) {
                    near_node_indexes.push_back(i);
                }
            }
        }

        return near_node_indexes;
    }

    std::shared_ptr<InformedRRTStar::Node> InformedRRTStar::chooseParentNode(const std::shared_ptr<Node>&              target_node,
                                                                             const std::vector<std::shared_ptr<Node>>& node_list,
                                                                             const std::vector<size_t>&                near_node_indexes) const {
        auto min_cost_parent_node = target_node->parent;
        auto min_cost             = std::numeric_limits<double>::max();
        for(const auto& near_node_index : near_node_indexes) {
            auto dist = target_node->state.distanceFrom(node_list[near_node_index]->state);
            auto cost = node_list[near_node_index]->cost + dist;
            if(cost < min_cost) {
                if(constraint_->checkCollision(target_node->state, node_list[near_node_index]->state)) {
                    min_cost_parent_node = node_list[near_node_index];
                    min_cost             = cost;
                }
            }
        }

        if(min_cost != std::numeric_limits<double>::max()) {
            target_node->parent = min_cost_parent_node;
            target_node->cost   = min_cost;
        }

        return target_node;
    }

    void InformedRRTStar::rewireNearNodes(std::vector<std::shared_ptr<Node>>& node_list,
                                          const std::vector<size_t>&          near_node_indexes) const {
        auto new_node = node_list.back();
        for(const auto& near_node_index : near_node_indexes) {
            auto near_node = node_list[near_node_index];
            auto new_cost  = new_node->cost + near_node->state.distanceFrom(new_node->state);
            if(new_cost < near_node->cost) {
                if(constraint_->checkCollision(new_node->state, near_node->state)) {
                    near_node->parent = new_node;
                    near_node->cost   = new_cost;
                }
            }
        }
    }

    int InformedRRTStar::getBestNodeIndex(const State&                              target_state,
                                          const double&                             radius,
                                          const std::vector<std::shared_ptr<Node>>& node_list) const {
        auto best_index = -1;
        auto min_cost   = std::numeric_limits<double>::max();
        for(size_t i = 0; i < node_list.size(); i++) {
            auto dist_from_target = target_state.distanceFrom(node_list[i]->state);
            if(dist_from_target < radius) {
                if(node_list[i]->cost < min_cost) {
                    best_index = i;
                    min_cost   = node_list[i]->cost;
                }
            }
        }

        return best_index;
    }

    Eigen::MatrixXd InformedRRTStar::calcRotationToWorldFlame(const State& start,
                                                              const State& goal) const {
        if(start.getDim() != goal.getDim() || start.getDim() < 2) {
            throw std::invalid_argument("[" + std::string(__PRETTY_FUNCTION__) + "] " +
                                        "State dimension is invalid");
        }

        auto a1_state = (goal - start) / goal.distanceFrom(start);
        auto a1_v = a1_state.vals;
        a1_v.push_back(0.0);

        auto M   = Eigen::Map<Eigen::VectorXd>(&*a1_v.begin(), a1_v.size()) * Eigen::MatrixXd::Identity(1, a1_v.size());
        auto svd = Eigen::JacobiSVD<Eigen::MatrixXd>(M, Eigen::ComputeFullU | Eigen::ComputeFullV);

        auto diag_v = std::vector<double>(a1_v.size(), 1.0);
        diag_v[diag_v.size() - 1] = svd.matrixV().determinant();
        diag_v[diag_v.size() - 2] = svd.matrixU().determinant();

        return svd.matrixU() * Eigen::Map<Eigen::VectorXd>(&*diag_v.begin(), diag_v.size()).asDiagonal() * svd.matrixV().transpose();
    }

    State InformedRRTStar::sampleUnitNBall(const uint32_t& dim) const {
        if(dim == 0) {
            throw std::invalid_argument("[" + std::string(__PRETTY_FUNCTION__) + "] " +
                                        "Can not sample zero-dimension ball");
        }

        std::random_device seed_gen;
        std::default_random_engine engine(seed_gen());

        std::normal_distribution<> dist_gauss(0.0, 1.0);
        std::uniform_real_distribution<double> dist_uni(0.0, 1.0);

        State x(dim);
        while(true) {
            for(auto& v : x.vals) {
                v = dist_gauss(engine);
            }

            auto r = x.norm();
            if(r != 0.0) {
                x = x / r;
                break;
            }
        }

        auto r = std::pow(dist_uni(engine), 1.0 / dim);

        return x * r;
    }
}