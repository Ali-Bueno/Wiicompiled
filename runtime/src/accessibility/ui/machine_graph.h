#ifndef MKW_ACCESSIBILITY_UI_MACHINE_GRAPH_H
#define MKW_ACCESSIBILITY_UI_MACHINE_GRAPH_H

#include <cstdint>
#include <string>
#include <vector>

namespace a11y::ui {

// The seven stat bars the vehicle screens draw for the highlighted kart: speed, weight,
// acceleration, handling, drift, off-road and mini-turbo.
//
// They are pictures, so there is no string to read anywhere. The numbers are recomputed the way
// CtrlMenuMachineGraph::OnLoad (func_807E7E20) does it - the driver's row of ticks plus the
// vehicle's, turned into the fill the bar would show - and spoken as a percentage.
//
// Empty when the stack has no stat graph on it, which is every screen but the three kart-select
// pages, and when the driver behind the bars cannot be established.
std::string DescribeVehicleStats(const std::vector<std::uint32_t>& layers, std::int32_t vehicleId);

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_MACHINE_GRAPH_H
