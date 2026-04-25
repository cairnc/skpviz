// Generate a synthetic LayersTraceFileProto for smoke-testing layerviewer.
// Not shipped in the normal build — wired as a separate executable.

#include <cstdio>
#include <fstream>

#include <perfetto/trace/perfetto_trace.pb.h>

using perfetto::protos::ColorProto;
using perfetto::protos::DisplayProto;
using perfetto::protos::FloatRectProto;
using perfetto::protos::LayerProto;
using perfetto::protos::LayersSnapshotProto;
using perfetto::protos::LayersTraceFileProto;
using perfetto::protos::RectProto;
using perfetto::protos::SizeProto;

static LayerProto *AddLayer(LayersSnapshotProto &snap, int id, const char *name,
                            float l, float t, float r, float b, float cr,
                            float cg, float cb, float ca, int z,
                            int parent = -1) {
    LayerProto *lp = snap.mutable_layers()->add_layers();
    lp->set_id(id);
    lp->set_name(name);
    lp->set_type("BufferStateLayer");
    lp->set_z(z);
    lp->set_layer_stack(0);
    if (parent >= 0)
        lp->set_parent(parent);
    FloatRectProto *sb = lp->mutable_screen_bounds();
    sb->set_left(l);
    sb->set_top(t);
    sb->set_right(r);
    sb->set_bottom(b);
    ColorProto *c = lp->mutable_color();
    c->set_r(cr);
    c->set_g(cg);
    c->set_b(cb);
    c->set_a(ca);
    SizeProto *sz = lp->mutable_size();
    sz->set_w((int)(r - l));
    sz->set_h((int)(b - t));
    return lp;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s out.winscope\n", argv[0]);
        return 2;
    }
    LayersTraceFileProto file;
    file.set_magic_number(0x45434152'5452594cULL);

    for (int f = 0; f < 5; ++f) {
        LayersSnapshotProto *snap = file.add_entry();
        snap->set_elapsed_realtime_nanos(int64_t(f) * 16'666'667);
        snap->set_where(f == 0 ? "bufferLatched" : "visibleRegionsDirty");

        DisplayProto *d = snap->add_displays();
        d->set_id(1);
        d->set_name("Built-In Screen");
        d->set_layer_stack(0);
        SizeProto *ds = d->mutable_size();
        ds->set_w(1080);
        ds->set_h(2400);

        // A root container, then a wallpaper, a statusbar, and an app window.
        AddLayer(*snap, 1, "ContainerLayer#1", 0, 0, 1080, 2400, 0, 0, 0, 0, 0);
        AddLayer(*snap, 2, "Wallpaper#2", 0, 0, 1080, 2400, 0.12f, 0.2f, 0.35f,
                 1.f, 1, 1);
        AddLayer(*snap, 3, "StatusBar#3", 0, 0, 1080, 90, 0.0f, 0.0f, 0.0f,
                 0.7f, 200, 1);
        // App shifts 10px right per frame to show motion over time.
        float dx = f * 10.f;
        AddLayer(*snap, 4, "CalendarApp#4", 40 + dx, 120, 1040 + dx, 2280, 0.9f,
                 0.85f, 0.3f, 1.f, 100, 1);
        AddLayer(*snap, 5, "Dialog#5", 200 + dx, 900, 880 + dx, 1400, 0.25f,
                 0.55f, 0.9f, 1.f, 150, 4);
    }

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    if (!file.SerializeToOstream(&out)) {
        std::fprintf(stderr, "serialize failed\n");
        return 1;
    }
    std::fprintf(stderr, "wrote %d entries to %s\n", file.entry_size(),
                 argv[1]);
    return 0;
}
