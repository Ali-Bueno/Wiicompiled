#ifndef MKW_ACCESSIBILITY_RACE_MOTION_PREDICTION_H
#define MKW_ACCESSIBILITY_RACE_MOTION_PREDICTION_H

#include <algorithm>
#include <cmath>

#include "accessibility/race/race_state.h"

namespace a11y::race {

// Three samples is the shortest window that rejects one bad heading sample.
inline constexpr float kYawSmoothSamples = 3.0f;

inline void UpdateYawRate(const RaceState& state, float rightX, float rightZ,
                          float& yawRate, float& lastForwardX, float& lastForwardZ,
                          bool& haveLastForward) {
    if (haveLastForward && state.frameSec > 0.0f) {
        const float alpha = 1.0f - std::exp(-1.0f / kYawSmoothSamples);
        const float sinTurn =
            std::clamp(-(lastForwardX * rightX + lastForwardZ * rightZ), -1.0f, 1.0f);
        yawRate += (std::asin(sinTurn) / state.frameSec - yawRate) * alpha;
    }
    lastForwardX = state.forwardX;
    lastForwardZ = state.forwardZ;
    haveLastForward = true;
}

// Constant-yaw motion over one anticipation interval, including its straight limit.
inline void PredictPosition(const RaceState& state, float rightX, float rightZ, float yawRate,
                            float seconds, float& x, float& z) {
    constexpr float kStraightTurnRad = 1e-3f;
    const float distance = state.speedPerSecond * seconds;
    const float turn = yawRate * seconds;
    float along = distance;
    float side = 0.0f;
    if (std::fabs(turn) >= kStraightTurnRad) {
        const float radius = distance / turn;
        along = radius * std::sin(turn);
        side = radius * (1.0f - std::cos(turn));
    }
    x = state.x + state.forwardX * along + rightX * side;
    z = state.z + state.forwardZ * along + rightZ * side;
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_MOTION_PREDICTION_H
