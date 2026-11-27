#include "stick.h"

#include <algorithm>
#include <cmath>

namespace App::Stick {

std::pair<float, float> Apply(float x, float y, const Shape& shape) {
    const double length = std::hypot(static_cast<double>(x), static_cast<double>(y));
    const double dead = std::clamp(shape.deadzone, 0, 50) / 100.0;
    const double outer = std::max(std::clamp(shape.outer, 50, 100) / 100.0, dead + 0.01);
    if (length <= dead || length == 0.0) {
        return {0.0f, 0.0f};
    }
    // past the dead zone, scaled from 0 at its edge to 1 at the outer one
    double push = std::clamp((length - dead) / (outer - dead), 0.0, 1.0);
    push = std::pow(push, std::clamp(shape.curve, 50, 300) / 100.0);
    const double anti = std::clamp(shape.anti_deadzone, 0, 60) / 100.0;
    push = anti + (1.0 - anti) * push;
    return {static_cast<float>(x / length * push), static_cast<float>(y / length * push)};
}

std::pair<float, float> Turn(float x, float y, double dt, const Camera& camera) {
    dt = std::clamp(dt, 0.0, 0.1);  // a long stall doesn't spin the camera around
    const double degrees = std::clamp(camera.speed, 1, 100) * 4.0 * dt;
    const double vertical = std::clamp(camera.vertical, 10, 200) / 100.0;
    const double yaw = x * degrees * (camera.invert_x ? -1.0 : 1.0);
    const double pitch = y * degrees * vertical * (camera.invert_y ? -1.0 : 1.0);
    return {static_cast<float>(yaw), static_cast<float>(pitch)};
}

} // namespace App::Stick
