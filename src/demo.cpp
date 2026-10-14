#include "demo.h"

#include <cmath>

namespace demo
{
static ue::Vec3 Unit(float x, float y, float z)
{
    const float l = sqrtf(x * x + y * y + z * z);
    return { x / l, y / l, z / l };
}

scene::SnapshotPtr Make(double t)
{
    auto s = std::make_shared<scene::Snapshot>();
    s->seq = (uint64_t)(t * 125.0);
    s->link = scene::Link::Ready;
    s->pid = 4242;
    s->buildId = "demo";
    s->worldName = "Demo_Bank";

    s->view.location = { 0, 0, 170 };
    s->view.rotation = { 0, (float)(8.0 * sin(t * 0.4)), 0 };
    s->view.fov = 90.0f;
    s->view.axisConstraint = 1;
    s->view.valid = true;

    const float sway = (float)(60.0 * sin(t * 0.7));
    scene::Guard walker;
    walker.id = 1;
    walker.pos = { 900, -250 + sway, 90 };
    walker.health = walker.maxHealth = 100;
    walker.patrolling = true;
    scene::Guard hunter;
    hunter.id = 2;
    hunter.pos = { 1400, 350, 90 };
    hunter.health = 55;
    hunter.maxHealth = 100;
    hunter.alert = true;
    scene::Guard corpse;
    corpse.id = 3;
    corpse.pos = { 600, 320, 25 };
    corpse.halfHeight = 25;
    corpse.dead = true;
    s->guards = { walker, hunter, corpse };

    scene::Camera a;
    a.id = 10; a.number = 1; a.pos = { 1100, 0, 300 }; a.forward = Unit(-1, 0.1f, -0.3f);
    scene::Camera b;
    b.id = 11; b.number = 2; b.pos = { 700, -500, 280 }; b.forward = Unit(0.3f, 1, -0.2f); b.emped = true;
    scene::Camera c;
    c.id = 12; c.number = 3; c.pos = { 1500, -300, 260 }; c.forward = Unit(-1, 0.4f, -0.2f); c.destroyed = true;
    scene::Camera d;
    d.id = 13; d.number = 4; d.pos = { 1000, 650, 290 }; d.forward = Unit(-0.2f, -1, -0.3f); d.spotting = true;
    s->cameras = { a, b, c, d };

    scene::Phone phone;
    phone.id = 20;
    phone.pos = { 500, 100, 20 };
    phone.active = true;
    phone.seconds = 60 - (int)fmod(t, 60.0);
    s->phones = { phone };

    scene::Terminal term;
    term.id = 30;
    term.pos = { 1300, -600, 150 };
    s->terminals = { term };

    s->stats.tickMs = 0.05f;
    s->stats.tickHz = 125.0f;
    s->stats.readsPerSec = 2300.0f;
    return s;
}
}
