// A controller's sticks the way current games read them, on its own (no SDL, no Azahar) so that it
// can be tested: a round dead zone that scales what is past it (no jump at its edge), an outer
// dead zone (a stick that never reaches its rim still gives a full push), a response curve, and
// an anti-dead zone (the least push the game is given once the stick moves, past the game's own
// dead zone). input.cpp shapes the controller's circle pad and C-stick with it, and turns the
// right stick's push into a turn of the camera, in degrees a second, where the port's camera hook
// runs (like the mouse's).
#pragma once

#include <utility>

namespace App::Stick {

struct Shape {
    int deadzone = 10;       ///< 0-50: % of the way that reads as centred
    int outer = 95;          ///< 50-100: % of the way that reads as all the way
    int curve = 100;         ///< 50-300: the response's exponent in % (100 straight, 200 squared)
    int anti_deadzone = 0;   ///< 0-60: % of the way given at once when the stick leaves its dead zone
};

/// The stick's push (x right, y up, each -1..1) shaped: the whole at most 1 long, its direction
/// kept.
std::pair<float, float> Apply(float x, float y, const Shape& shape);

struct Camera {
    int speed = 50;          ///< 1-100: at full push, 4 degrees a second per step (50: 200°/s)
    int vertical = 60;       ///< 10-200: up and down in % of left and right
    bool invert_x = false;
    bool invert_y = false;
};

/// The camera's turn for `dt` seconds of a shaped push: (yaw to the right, pitch up) in degrees.
std::pair<float, float> Turn(float x, float y, double dt, const Camera& camera);

} // namespace App::Stick
