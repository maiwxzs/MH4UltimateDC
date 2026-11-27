"""port/app/stick.cpp: a controller's sticks the way current games read them. A round dead zone
that scales what is past it, an outer dead zone, a response curve and an anti-dead zone; and the
right stick's push as a turn of the camera in degrees a second."""
from __future__ import annotations

import math
import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <string>
#include "stick.h"

using namespace App::Stick;

// shape DEAD OUTER CURVE ANTI X Y: "x y"
// turn SPEED VERTICAL INVX INVY DT X Y: "yaw pitch"
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "shape") {
        Shape s{std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5])};
        const auto [x, y] = Apply(std::atof(argv[6]), std::atof(argv[7]), s);
        std::printf("%.6f %.6f\n", x, y);
        return 0;
    }
    Camera c{std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]) != 0, std::atoi(argv[5]) != 0};
    const auto [yaw, pitch] = Turn(std::atof(argv[7]), std::atof(argv[8]), std::atof(argv[6]), c);
    std::printf("%.6f %.6f\n", yaw, pitch);
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("stick")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "stick.cpp"), "-o", str(exe)],
                   check=True)
    return exe


def run(driver, *args):
    out = subprocess.run([str(driver), *map(str, args)], capture_output=True, text=True, check=True).stdout
    return tuple(float(v) for v in out.split())


def shape(driver, x, y, dead=10, outer=95, curve=100, anti=0):
    return run(driver, "shape", dead, outer, curve, anti, x, y)


def turn(driver, x, y, speed=50, vertical=60, inv_x=0, inv_y=0, dt=1 / 60):
    return run(driver, "turn", speed, vertical, inv_x, inv_y, dt, x, y)


def test_inside_the_dead_zone_reads_centred(driver):
    assert shape(driver, 0.05, 0.05) == (0.0, 0.0)
    assert shape(driver, 0.0, 0.0) == (0.0, 0.0)


def test_no_jump_at_the_dead_zones_edge(driver):
    # just past it the push is tiny, not the 10 % a plain cut-off gives
    x, _ = shape(driver, 0.11, 0.0)
    assert 0.0 < x < 0.02


def test_outer_dead_zone_gives_a_full_push(driver):
    assert shape(driver, 0.95, 0.0)[0] == pytest.approx(1.0)
    assert shape(driver, 1.0, 0.0)[0] == pytest.approx(1.0)
    # a diagonal on a square stick's corner is no longer than the stick
    x, y = shape(driver, 1.0, 1.0)
    assert math.hypot(x, y) == pytest.approx(1.0)


def test_round_dead_zone_keeps_the_direction(driver):
    x, y = shape(driver, 0.3, 0.4)
    assert y / x == pytest.approx(0.4 / 0.3)


def test_curve_makes_small_pushes_finer(driver):
    straight = shape(driver, 0.5, 0.0, curve=100)[0]
    squared = shape(driver, 0.5, 0.0, curve=200)[0]
    assert squared == pytest.approx(straight ** 2)
    assert squared < straight


def test_anti_dead_zone_skips_the_games_own(driver):
    x, _ = shape(driver, 0.11, 0.0, anti=25)
    assert 0.25 <= x < 0.27
    assert shape(driver, 0.95, 0.0, anti=25)[0] == pytest.approx(1.0)


def test_camera_turns_by_degrees_a_second(driver):
    # speed 50: 200 degrees a second at full push
    yaw, pitch = turn(driver, 1.0, 0.0, dt=0.05)
    assert yaw == pytest.approx(10.0)
    assert pitch == 0.0
    yaw, pitch = turn(driver, 0.0, 1.0, dt=0.05, vertical=60)
    assert pitch == pytest.approx(6.0)
    # half the push, half the turn (the curve is the stick's shape's, not the camera's)
    assert turn(driver, 0.5, 0.0, dt=0.05)[0] == pytest.approx(5.0)


def test_camera_inverts_and_ignores_long_stalls(driver):
    assert turn(driver, 1.0, 1.0, inv_x=1, inv_y=1, dt=0.05) == pytest.approx((-10.0, -6.0))
    # a stall of a second turns no more than a tenth of a second does
    assert turn(driver, 1.0, 0.0, dt=1.0)[0] == pytest.approx(20.0)
