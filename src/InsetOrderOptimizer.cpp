// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "ExtruderTrain.h"
#include "FffGcodeWriter.h"
#include "InsetOrderOptimizer.h"
#include "LayerPlan.h"
#include "utils/views/convert.h"
#include "utils/views/dfs.h"
#include <spdlog/spdlog.h>

#include <iterator>
#include <tuple>

#include <range/v3/algorithm/max.hpp>
#include <range/v3/algorithm/sort.hpp>
#include <range/v3/range/operations.hpp>
#include <range/v3/to_container.hpp>
#include <range/v3/view/addressof.hpp>
#include <range/v3/view/any_view.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/remove_if.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take_exactly.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/view/zip.hpp>

namespace rg = ranges;
namespace rv = ranges::views;

namespace cura
{

InsetOrderOptimizer::InsetOrderOptimizer(const FffGcodeWriter& gcode_writer,
                                         const SliceDataStorage& storage,
                                         LayerPlan& gcode_layer,
                                         const Settings& settings,
                                         const int extruder_nr,
                                         const GCodePathConfig& inset_0_non_bridge_config,
                                         const GCodePathConfig& inset_X_non_bridge_config,
                                         const GCodePathConfig& inset_0_bridge_config,
                                         const GCodePathConfig& inset_X_bridge_config,
                                         const bool retract_before_outer_wall,
                                         const coord_t wall_0_wipe_dist,
                                         const coord_t wall_x_wipe_dist,
                                         const size_t wall_0_extruder_nr,
                                         const size_t wall_x_extruder_nr,
                                         const ZSeamConfig& z_seam_config,
                                         const std::vector<VariableWidthLines>& paths)
    : gcode_writer(gcode_writer)
    , storage(storage)
    , gcode_layer(gcode_layer)
    , settings(settings)
    , extruder_nr(extruder_nr)
    , inset_0_non_bridge_config(inset_0_non_bridge_config)
    , inset_X_non_bridge_config(inset_X_non_bridge_config)
    , inset_0_bridge_config(inset_0_bridge_config)
    , inset_X_bridge_config(inset_X_bridge_config)
    , retract_before_outer_wall(retract_before_outer_wall)
    , wall_0_wipe_dist(wall_0_wipe_dist)
    , wall_x_wipe_dist(wall_x_wipe_dist)
    , wall_0_extruder_nr(wall_0_extruder_nr)
    , wall_x_extruder_nr(wall_x_extruder_nr)
    , z_seam_config(z_seam_config)
    , paths(paths)
    , layer_nr(gcode_layer.getLayerNr())
{
}

bool InsetOrderOptimizer::addToLayer()
{
    // Settings & configs:
    const auto pack_by_inset = ! settings.get<bool>("optimize_wall_printing_order");
    const auto inset_direction = settings.get<InsetDirection>("inset_direction");
    const auto alternate_walls = settings.get<bool>("material_alternate_walls");

    const bool outer_to_inner = inset_direction == InsetDirection::OUTSIDE_IN;
    const bool use_one_extruder = wall_0_extruder_nr == wall_x_extruder_nr;
    const bool current_extruder_is_wall_x = wall_x_extruder_nr == extruder_nr;

    const bool reverse = shouldReversePath(use_one_extruder, current_extruder_is_wall_x, outer_to_inner);
    auto walls_to_be_added = getWallsToBeAdded(reverse, use_one_extruder);

    const auto order = pack_by_inset ? getInsetOrder(walls_to_be_added, outer_to_inner) : getRegionOrder(walls_to_be_added, outer_to_inner);

    constexpr Ratio flow = 1.0_r;

    bool added_something = false;

    constexpr bool detect_loops = false;
    constexpr Polygons* combing_boundary = nullptr;
    // When we alternate walls, also alternate the direction at which the first wall starts in.
    // On even layers we start with normal direction, on odd layers with inverted direction.
    constexpr bool reverse_all_paths = false;
    PathOrderOptimizer<const ExtrusionLine*> order_optimizer(gcode_layer.getLastPlannedPositionOrStartingPosition(), z_seam_config, detect_loops, combing_boundary, reverse_all_paths, order);

    for (const auto& line : walls_to_be_added)
    {
        if (line.is_closed)
        {
            order_optimizer.addPolygon(&line);
        }
        else
        {
            order_optimizer.addPolyline(&line);
        }
    }


    order_optimizer.optimize();

    cura::Point p_end{ 0, 0 };
    for (const PathOrderPath<const ExtrusionLine*>& path : order_optimizer.paths)
    {
        if (path.vertices->empty())
            continue;

        const bool is_outer_wall = path.vertices->inset_idx == 0; // or thin wall 'gap filler'
        const bool is_gap_filler = path.vertices->is_odd;
        const GCodePathConfig& non_bridge_config = is_outer_wall ? inset_0_non_bridge_config : inset_X_non_bridge_config;
        const GCodePathConfig& bridge_config = is_outer_wall ? inset_0_bridge_config : inset_X_bridge_config;
        const coord_t wipe_dist = is_outer_wall && ! is_gap_filler ? wall_0_wipe_dist : wall_x_wipe_dist;
        const bool retract_before = is_outer_wall ? retract_before_outer_wall : false;

        const bool revert_inset = alternate_walls && (path.vertices->inset_idx % 2);
        const bool revert_layer = alternate_walls && (layer_nr % 2);
        const bool backwards = path.backwards != (revert_inset != revert_layer);
        const size_t start_index = (backwards != path.backwards) ? path.vertices->size() - (path.start_vertex + 1) : path.start_vertex;

        p_end = path.backwards ? path.vertices->back().p : path.vertices->front().p;
        const cura::Point p_start = path.backwards ? path.vertices->front().p : path.vertices->back().p;
        const bool linked_path = p_start != p_end;

        gcode_writer.setExtruder_addPrime(storage, gcode_layer, extruder_nr);
        gcode_layer.setIsInside(true); // Going to print walls, which are always inside.
        gcode_layer.addWall(*path.vertices, start_index, settings, non_bridge_config, bridge_config, wipe_dist, flow, retract_before, path.is_closed, backwards, linked_path);
        added_something = true;
    }
    return added_something;
}

InsetOrderOptimizer::value_type InsetOrderOptimizer::getRegionOrder(const auto& input, const bool outer_to_inner)
{
    if (input.empty()) // Early out
    {
        return {};
    }

    // Cache the polygons and get the signed area of each extrusion line and store them mapped against the pointers for those lines
    struct LineLoc
    {
        const ExtrusionLine* line;
        Polygon poly;
        double area;
    };
    auto poly_views = input | views::convert<Polygon>(&ExtrusionLine::toPolygon);
    auto pointer_view = input | rv::addressof;
    auto locator_view = rv::zip(pointer_view, poly_views)
                      | rv::transform(
                            [](const auto& locator)
                            {
                                const auto poly = std::get<1>(locator);
                                const auto line = std::get<0>(locator);
                                return LineLoc {
                                    .line = line,
                                    .poly = poly,
                                    .area = line->is_closed ? poly.area() : 0.0,
                                };
                            })
                      | rg::to_vector;

    // sort polygons on increasing area
    rg::sort( locator_view, [](const auto& lhs, const auto& rhs) { return std::abs(lhs) < std::abs(rhs); }, &LineLoc::area);

    std::unordered_multimap<const LineLoc*, const LineLoc*> graph;
    std::unordered_set<LineLoc*> roots{ &rg::front(locator_view) };
    for (const auto& locator : locator_view | rv::addressof | rv::drop(1))
    {
        std::vector<LineLoc*> erase;
        for (const auto& root : roots)
        {
            if (root->poly.inside(locator->poly))
            {
                // we need a bi-directional graph as we are performing a dfs from the root down
                // and from each of the hole (which are leaves in the graph) up the tree
                graph.emplace(locator, root);
                graph.emplace(root, locator);
                erase.emplace_back(root);
            }
        }
        for (const auto& node : erase)
        {
            roots.erase(node);
        }
        roots.emplace(locator);
    }

    std::unordered_set<std::pair<const ExtrusionLine*, const ExtrusionLine*>> order;

    for (const LineLoc* root : roots)
    {
        std::map<const LineLoc*, unsigned int> min_dist;
        std::map<const LineLoc*, const LineLoc*> min_node;
        std::vector<const LineLoc*> hole_roots;

        // Responsible for the following initialization
        // - initialize all reachable nodes to root
        // - mark all reachable nodes with their distance from the root
        // - find hole roots, these are the innermost polygons enclosing a hole
        {
            const std::function<unsigned int(const LineLoc*, const unsigned int)> initialize_nodes =
                [graph, root, &hole_roots, &min_node, &min_dist]
                (const auto current_node, const auto dist)
                {
                    min_node[current_node] = root;
                    min_dist[current_node] = dist;

                    // find hole roots (defined by a possitive area in clipper1), these are leaves of the tree structure
                    // as odd walls are also leaves we filter them out by adding a non-zero area check
                    if (current_node != root && graph.count(current_node) == 1 && current_node->line->is_closed && current_node->area > 0)
                    {
                        hole_roots.push_back(current_node);
                    }

                    return dist + 1;
                };

            unsigned int initial_dist = 0;
            auto visited = std::unordered_set<const LineLoc*>();
            actions::dfs(root, graph, initial_dist, initialize_nodes, visited);
        };

        // For each hole root perform a dfs, and keep track of distance from hole root
        // if the distance to a node is smaller than a distance calculated from another root update
        // min_dist and min_node
        {
            for (auto& hole_root : hole_roots)
            {
                const std::function<unsigned int(const LineLoc*, const unsigned int)> update_nodes =
                    [hole_root, &min_dist, &min_node]
                    (const auto& current_node, auto dist)
                    {
                        if (dist < min_dist[current_node])
                        {
                            min_dist[current_node] = dist;
                            min_node[current_node] = hole_root;
                        }
                        return dist + 1;
                    };

                unsigned int initial_dist = 0;
                auto visited = std::unordered_set<const LineLoc*>();
                actions::dfs(hole_root, graph, initial_dist, update_nodes, visited);
            }
        };

        // perform a dfs from the root and all hole roots $r$ and set the order constraints for each polyline for which
        // the distance is closest to root $r$
        {
            const LineLoc* prev_node = nullptr;

            const LineLoc* root_ = root;
            const std::function<std::nullptr_t(const LineLoc*, std::nullptr_t)> set_order_constraints =
                [&order, &min_node, &root_, &prev_node, graph]
                (const auto& current_node, auto _prev_state)
                {
                   if (min_node[current_node] == root_)
                   {
                       if (prev_node != nullptr)
                       {
                           order.emplace(prev_node->line, current_node->line);
                       }
                       prev_node = current_node;
                   }

                   return nullptr;
                };

            auto visited = std::unordered_set<const LineLoc*>();
            actions::dfs(root, graph, nullptr, set_order_constraints, visited);

            for (auto& hole_root : hole_roots)
            {
                root_ = hole_root;
                auto visited = std::unordered_set<const LineLoc*>();
                actions::dfs(hole_root, graph, nullptr, set_order_constraints, visited);
            }
        }
    }

    // flip the key values if we want to print from inner to outer walls
    return outer_to_inner ? order : rv::zip(order | rv::values, order | rv::keys) | rg::to<value_type>;
}

InsetOrderOptimizer::value_type InsetOrderOptimizer::getInsetOrder(const auto& input, const bool outer_to_inner)
{
    value_type order;

    std::vector<std::vector<const ExtrusionLine*>> walls_by_inset;
    std::vector<std::vector<const ExtrusionLine*>> fillers_by_inset;

    for (const auto& line : input)
    {
        if (line.is_odd)
        {
            if (line.inset_idx >= fillers_by_inset.size())
            {
                fillers_by_inset.resize(line.inset_idx + 1);
            }
            fillers_by_inset[line.inset_idx].emplace_back(&line);
        }
        else
        {
            if (line.inset_idx >= walls_by_inset.size())
            {
                walls_by_inset.resize(line.inset_idx + 1);
            }
            walls_by_inset[line.inset_idx].emplace_back(&line);
        }
    }
    for (size_t inset_idx = 0; inset_idx + 1 < walls_by_inset.size(); inset_idx++)
    {
        for (const ExtrusionLine* line : walls_by_inset[inset_idx])
        {
            for (const ExtrusionLine* inner_line : walls_by_inset[inset_idx + 1])
            {
                const ExtrusionLine* before = inner_line;
                const ExtrusionLine* after = line;
                if (outer_to_inner)
                {
                    std::swap(before, after);
                }
                order.emplace(before, after);
            }
        }
    }
    for (size_t inset_idx = 1; inset_idx < fillers_by_inset.size(); inset_idx++)
    {
        for (const ExtrusionLine* line : fillers_by_inset[inset_idx])
        {
            if (inset_idx - 1 >= walls_by_inset.size())
                continue;
            for (const ExtrusionLine* enclosing_wall : walls_by_inset[inset_idx - 1])
            {
                order.emplace(enclosing_wall, line);
            }
        }
    }

    return order;
}

constexpr bool InsetOrderOptimizer::shouldReversePath(const bool use_one_extruder, const bool current_extruder_is_wall_x, const bool outer_to_inner)
{
    if (use_one_extruder && current_extruder_is_wall_x)
    {
        return ! outer_to_inner;
    }
    return current_extruder_is_wall_x;
}

std::vector<ExtrusionLine> InsetOrderOptimizer::getWallsToBeAdded(const bool reverse, const bool use_one_extruder)
{
    rg::any_view<VariableWidthLines> view;
    if (reverse)
    {
        if (use_one_extruder)
        {
            view = paths | rv::reverse;
        }
        else
        {
            view = paths | rv::reverse | rv::drop_last(1);
        }
    }
    else
    {
        if (use_one_extruder)
        {
            view = paths | rv::all;
        }
        else
        {
            view = paths | rv::take_exactly(1);
        }
    }
    return view | rv::join | rv::remove_if(rg::empty) | rg::to_vector;
}
} // namespace cura
