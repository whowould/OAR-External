// Development aid: runs the scanner against the live game and prints what it sees once per second.
//   oar_probe.exe [seconds]       (needs the same elevation as the game)
#include "game/scanner.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 5;
    Settings settings;
    settings.debugActors = argc > 2;

    scene::Scanner scanner;
    scanner.setSettings(settings);
    scanner.start();

    static const char* kLink[] = { "Detached", "AccessDenied", "Incompatible", "Waiting", "Ready" };
    for (int i = 0; i < seconds; i++)
    {
        ::Sleep(1000);
        const scene::SnapshotPtr s = scanner.snapshot();
        printf("[%d] link=%s detail='%s' pid=%u build=%s world=%s\n", i, kLink[(int)s->link], s->detail.c_str(), s->pid, s->buildId.c_str(), s->worldName.c_str());
        if (s->link == scene::Link::Ready)
        {
            const auto& v = s->view;
            printf("    view valid=%d loc=(%.1f %.1f %.1f) rot=(p%.1f y%.1f r%.1f) fov=%.1f aspect=%.3f constrain=%d axis=%d\n", v.valid, v.location.x, v.location.y,
                   v.location.z, v.rotation.pitch, v.rotation.yaw, v.rotation.roll, v.fov, v.aspect, v.constrainAspect, v.axisConstraint);
            printf("    guards=%zu cameras=%zu phones=%zu terminals=%zu debugActors=%zu\n", s->guards.size(), s->cameras.size(), s->phones.size(),
                   s->terminals.size(), s->debugActors ? s->debugActors->size() : 0);
            for (const auto& g : s->guards)
                printf("      guard %p hp=%d/%d dead=%d patrol=%d invest=%d alert=%d pos=(%.0f %.0f %.0f)\n", (void*)g.id, g.health, g.maxHealth, g.dead, g.patrolling, g.investigating, g.alert, g.pos.x, g.pos.y, g.pos.z);
            for (const auto& c : s->cameras)
                printf("      camera #%d destroyed=%d emped=%d seen=%d pos=(%.0f %.0f %.0f)\n", c.number, c.destroyed, c.emped, c.spotting, c.pos.x, c.pos.y, c.pos.z);
            for (const auto& p : s->phones)
                printf("      phone active=%d emped=%d seconds=%d pos=(%.0f %.0f %.0f)\n", p.active, p.emped, p.seconds, p.pos.x, p.pos.y, p.pos.z);
            const auto& st = s->stats;
            printf("    tick=%.3fms %.0fHz discover=%.2fms reads/s=%.0f names=%zu offsets=%zu cache(hit=%llu miss=%llu) actors=%zu\n", st.tickMs, st.tickHz, st.discoverMs,
                   st.readsPerSec, st.names, st.offsetsCached, (unsigned long long)st.cacheHits, (unsigned long long)st.cacheMisses, st.actorsSeen);
        }
        for (const auto& e : scanner.takeEvents())
            printf("    event: %s %s\n", e.text.c_str(), e.highlight.c_str());
        fflush(stdout);
    }
    scanner.stop();
    return 0;
}
