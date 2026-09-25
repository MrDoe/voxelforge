// tree_gen - procedural tree generator (voxel stand for the hamlet).
//
// Generates trees as .vxw object layers from a growth simulation, not from
// prescribed shapes: no "trunk + cone" or "trunk + ball" templates. The
// branching structure EMERGES from competition for space.
//
// Algorithm (Runions, Lane & Prusinkiewicz, "Modeling Trees with a Space
// Colonization Algorithm", Eurographics Workshop on Natural Phenomena 2007):
//
//   1. Scatter attraction points through a species crown envelope (a cone for
//      conifers, an ellipsoid crown above a clear trunk for broadleaves).
//   2. Repeatedly: every attraction point looks at the closest tree node
//      within its influence radius and pulls it; each node grows one new node
//      in the average direction of its points; points closer than the kill
//      radius are consumed. Branches therefore fill the crown evenly, fork
//      where the pull divides, and never cross - the competition is the
//      model.
//   3. Radius from the PIPE MODEL: a segment's cross-section carries every
//      leaf downstream of it, so r ~ (leaf count)^(1/2.49). That is what
//      produces the natural trunk-to-twig taper.
//   4. Foliage: small leaf clusters at the terminal nodes, sized by the
//      remaining local space - so the crown reads as clusters following the
//      branch structure rather than as one blob.
//   5. Voxelisation: segments are rasterised as tapered capsules into a local
//      grid, then only SURFACE voxels are emitted (solid with an air face),
//      which is what a .vxw stores and keeps the layer small.
//
// Deterministic: a seed reproduces the forest bit for bit.
//
// Usage:
//   tree_gen [--seed N] [--out NAME] [--dry-run]
// Writes assets/<NAME>.vxw (default hamlet_trees) and registers it in
// assets/world.json.

#include "voxel/common.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/worldfile.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vf::voxel;

namespace {

// ---------------------------------------------------------------- utilities
// Deterministic 64-bit PRNG (splitmix64): same seed => same forest.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next()
    {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float f() { return float(next() >> 40) / float(1u << 24); }
    float range(float a, float b) { return a + (b - a) * f(); }
    int irange(int a, int b) { return a + int(next() % uint64_t(b - a + 1)); }
};

inline float worldOf(int cell) { return -0.5f * WORLD + (float(cell) + 0.5f) * VOXEL; }
inline int latticeOf(float world)
{
    return int(std::floor((world + 0.5f * WORLD) / VOXEL));
}

// -------------------------------------------------------------- voxel buffer
struct Grid {
    int nx = 0, ny = 0, nz = 0, x0 = 0, y0 = 0, z0 = 0; // lattice origin
    std::vector<uint8_t> mat, r, g, b, solid;

    void init(int ax0, int ay0, int az0, int anx, int any, int anz)
    {
        x0 = ax0; y0 = ay0; z0 = az0;
        nx = anx; ny = any; nz = anz;
        const size_t n = size_t(nx) * ny * nz;
        mat.assign(n, 0); r.assign(n, 0); g.assign(n, 0); b.assign(n, 0);
        solid.assign(n, 0);
    }
    size_t idx(int x, int y, int z) const
    {
        return (size_t(z) * ny + size_t(y)) * nx + size_t(x);
    }
    bool inside(int x, int y, int z) const
    {
        return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
    }
    bool isSolid(int x, int y, int z) const
    {
        return inside(x, y, z) && solid[idx(x, y, z)] != 0;
    }
    void set(int x, int y, int z, uint8_t m, int cr, int cg, int cb)
    {
        if (!inside(x, y, z))
            return;
        const size_t i = idx(x, y, z);
        // wood wins where a limb and a leaf cluster overlap
        if (solid[i] && mat[i] == 8 && m != 8)
            return;
        solid[i] = 1; mat[i] = m;
        r[i] = uint8_t(std::clamp(cr, 0, 255));
        g[i] = uint8_t(std::clamp(cg, 0, 255));
        b[i] = uint8_t(std::clamp(cb, 0, 255));
    }
};

void rasterCapsule(Grid& gr, const glm::vec3& a, const glm::vec3& b, float ra,
                   float rb, uint8_t mat, const glm::vec3& rgb)
{
    const float pad = std::max(ra, rb) + VOXEL;
    const glm::vec3 lo = glm::min(a, b) - pad, hi = glm::max(a, b) + pad;
    const int cx0 = latticeOf(lo.x), cx1 = latticeOf(hi.x);
    const int cy0 = latticeOf(lo.y), cy1 = latticeOf(hi.y);
    const int cz0 = latticeOf(lo.z), cz1 = latticeOf(hi.z);
    const glm::vec3 ab = b - a;
    const float ab2 = glm::dot(ab, ab);
    for (int z = cz0; z <= cz1; ++z)
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) {
                const glm::vec3 p(worldOf(x), worldOf(y), worldOf(z));
                const float t = ab2 > 1e-9f
                                    ? glm::clamp(glm::dot(p - a, ab) / ab2, 0.0f, 1.0f)
                                    : 0.0f;
                const float rad = glm::mix(ra, rb, t);
                if (glm::dot(p - (a + ab * t), p - (a + ab * t)) > rad * rad)
                    continue;
                gr.set(x - gr.x0, y - gr.y0, z - gr.z0, mat, int(rgb.r),
                       int(rgb.g), int(rgb.b));
            }
}

// A leaf cluster: an ellipsoid of foliage with per-voxel tint jitter so a
// crown is not one flat colour.
void rasterCluster(Grid& gr, const glm::vec3& c, const glm::vec3& radii, Rng& rng,
                   const glm::vec3& base, float jitter)
{
    const float pad = std::max(std::max(radii.x, radii.y), radii.z) + VOXEL;
    const int cx0 = latticeOf(c.x - pad), cx1 = latticeOf(c.x + pad);
    const int cy0 = latticeOf(c.y - pad), cy1 = latticeOf(c.y + pad);
    const int cz0 = latticeOf(c.z - pad), cz1 = latticeOf(c.z + pad);
    for (int z = cz0; z <= cz1; ++z)
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) {
                const glm::vec3 p(worldOf(x), worldOf(y), worldOf(z));
                const glm::vec3 d = (p - c) / glm::max(radii, glm::vec3(1e-3f));
                if (glm::dot(d, d) > 1.0f)
                    continue;
                const float j = (rng.f() - 0.5f) * jitter;
                gr.set(x - gr.x0, y - gr.y0, z - gr.z0, 8,
                       int(base.r * (1.0f + j)), int(base.g * (1.0f + j)),
                       int(base.b * (1.0f + j)));
            }
}

// ------------------------------------------------------------- species setup
struct Species {
    const char* name;
    float height;      // metres, ground to crown top
    float clearTrunk;  // metres of bare trunk before the crown starts
    float crownRad;    // metres, widest crown radius
    float coneShape;   // 1 = conifer cone, 0 = ellipsoid crown
    float baseRad;     // trunk radius at the ground, metres
    float influence;   // attraction influence radius, metres
    float kill;        // attraction kill radius, metres
    float step;        // growth step, metres
    int   attractors;  // points seeded in the envelope
    float leafSize;    // leaf cluster radius, metres
    glm::vec3 wood { 108, 70, 42 };
    glm::vec3 leaf { 50, 102, 40 };
};

// Scatter attraction points through the species envelope. A conifer uses a
// cone (widest low, narrowing to a spire); a broadleaf uses an ellipsoid crown
// sitting on top of the clear trunk.
void seedAttractors(const Species& S, Rng& rng, std::vector<glm::vec3>& pts)
{
    pts.clear();
    pts.reserve(size_t(S.attractors));
    const float y0 = S.clearTrunk;
    const float y1 = S.height;
    while (int(pts.size()) < S.attractors) {
        const float t = rng.f();
        const float y = y0 + (y1 - y0) * t;
        float r;
        if (S.coneShape > 0.5f) {
            const float f = 1.0f - t;                    // 1 at the base, 0 at the top
            r = S.crownRad * std::pow(std::max(f, 0.0f), 0.85f);
        } else {
            const float f = 1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f);
            r = S.crownRad * std::sqrt(std::max(f, 0.0f));
        }
        // random point in the disk, sqrt for uniform area density
        const float a = rng.range(0.0f, 6.2831853f);
        const float rr = r * std::sqrt(rng.f());
        pts.push_back(glm::vec3(std::cos(a) * rr, y, std::sin(a) * rr));
    }
}

// ------------------------------------------------------- space colonization
struct Node {
    glm::vec3 p { 0.0f };
    int parent = -1;
    int leaves = 0;   // terminal tips downstream (pipe model)
    float r = 0.0f;
};

void colonize(const Species& S, Rng& rng, std::vector<Node>& nodes,
              std::vector<glm::vec3>& att)
{
    nodes.clear();
    // Trunk: an explicit chain from the ground to the crown base, with a slight
    // deterministic lean so it is not a perfect pole. Colonization then grows
    // the crown from this tip; the pipe model below gives the trunk its radius
    // from every leaf above it, so it still tapers naturally.
    const int trunkSteps = std::max(2, int(S.clearTrunk / S.step));
    nodes.push_back({ glm::vec3(0.0f), -1, 0, S.baseRad });
    const float leanA = rng.range(0.0f, 6.2831853f);
    const float leanAmt = S.clearTrunk * rng.range(0.10f, 0.26f);
    const float bend = rng.range(0.4f, 1.5f); // S-curve phase
    for (int i = 1; i <= trunkSteps; ++i) {
        const float t = float(i) / float(trunkSteps);
        Node nd;
        // a gentle arc plus a slow S-bend: real trunks are never poles
        const float lat = leanAmt * (t * t) + leanAmt * 0.35f * std::sin(bend * t * 3.14159f);
        nd.p = glm::vec3(lat * std::sin(leanA), S.clearTrunk * t,
                         lat * std::cos(leanA));
        nd.parent = int(nodes.size()) - 1;
        nodes.push_back(nd);
    }

    const float infl2 = S.influence * S.influence;
    const float kill2 = S.kill * S.kill;
    std::vector<char> alive(att.size(), 1);

    for (int iter = 0; iter < 400; ++iter) {
        // each node averages the directions of the points that pull it
        std::vector<glm::vec3> pull(nodes.size(), glm::vec3(0.0f));
        std::vector<int> count(nodes.size(), 0);
        int live = 0;
        for (size_t i = 0; i < att.size(); ++i) {
            if (!alive[i])
                continue;
            ++live;
            int best = -1;
            float bestD = infl2;
            for (size_t n = 0; n < nodes.size(); ++n) {
                const glm::vec3 d = att[i] - nodes[n].p;
                const float dd = glm::dot(d, d);
                if (dd < bestD) {
                    bestD = dd;
                    best = int(n);
                }
            }
            if (best >= 0) {
                pull[size_t(best)] += glm::normalize(att[i] - nodes[size_t(best)].p);
                ++count[size_t(best)];
            }
        }
        if (live == 0)
            break;

        // grow every pulled node one step
        const size_t n0 = nodes.size();
        bool grew = false;
        for (size_t n = 0; n < n0; ++n) {
            if (count[n] == 0)
                continue;
            glm::vec3 dir = glm::normalize(pull[n]);
            // tropism: bias growth upward so branches reach for the light
            dir = glm::normalize(dir + glm::vec3(0.0f, 0.35f, 0.0f));
            Node nd;
            nd.p = nodes[n].p + dir * S.step;
            nd.parent = int(n);
            nodes.push_back(nd);
            grew = true;
        }
        if (!grew)
            break;

        // consume the points that have been reached
        for (size_t i = 0; i < att.size(); ++i) {
            if (!alive[i])
                continue;
            for (size_t n = n0; n < nodes.size(); ++n) {
                const glm::vec3 d = att[i] - nodes[n].p;
                if (glm::dot(d, d) < kill2) {
                    alive[i] = 0;
                    break;
                }
            }
        }
    }

    // ---- pipe model: accumulate leaf counts from the tips down, then radius
    for (Node& nd : nodes)
        nd.leaves = 0;
    for (size_t n = 1; n < nodes.size(); ++n) {
        bool tip = true;
        for (size_t m = 1; m < nodes.size(); ++m)
            if (nodes[m].parent == int(n)) {
                tip = false;
                break;
            }
        if (tip) {
            // walk to the root, counting this tip for every ancestor
            int cur = int(n);
            while (cur >= 0) {
                ++nodes[size_t(cur)].leaves;
                cur = nodes[size_t(cur)].parent;
            }
        }
    }
    int maxLeaves = 1;
    for (const Node& nd : nodes)
        maxLeaves = std::max(maxLeaves, nd.leaves);
    for (Node& nd : nodes)
        nd.r = S.baseRad * std::pow(float(nd.leaves) / float(maxLeaves), 1.0f / 2.49f);
}

// ---------------------------------------------------------------- one tree
// Generates one tree at (wx, wz) on the terrain, appending surface voxels.
std::vector<VoxelRecord> buildTree(const Species& S0, float wx, float wz,
                                   uint64_t seed)
{
    Rng rng(seed);
    // Per-tree variation: a stand of identical clones reads as a repeated
    // asset, so every dimension is jittered around the species norm.
    Species S = S0;
    S.height *= rng.range(0.85f, 1.18f);
    S.clearTrunk *= rng.range(0.85f, 1.2f);
    S.crownRad *= rng.range(0.82f, 1.2f);
    S.baseRad *= rng.range(0.85f, 1.15f);
    S.leafSize *= rng.range(0.85f, 1.15f);
    S.leaf *= rng.range(0.85f, 1.12f);
    S.leaf.g *= rng.range(0.9f, 1.12f);
    std::vector<glm::vec3> att;
    seedAttractors(S, rng, att);
    std::vector<Node> nodes;
    colonize(S, rng, nodes, att);

    const float ground = sharedHeightmap().sample(wx, wz);
    const float H = S.height;
    const int span = int(S.crownRad / VOXEL) + 8;
    Grid gr;
    gr.init(latticeOf(wx) - span, latticeOf(ground) - 2, latticeOf(wz) - span,
            span * 2 + 1, int(H / VOXEL) + 8, span * 2 + 1);
    const glm::vec3 org(wx, ground, wz);

    // limbs
    for (size_t n = 1; n < nodes.size(); ++n) {
        const Node& nd = nodes[n];
        if (nd.parent < 0)
            continue;
        const Node& pa = nodes[size_t(nd.parent)];
        rasterCapsule(gr, org + pa.p, org + nd.p, std::max(pa.r, 0.06f),
                      std::max(nd.r, 0.05f), 6, S.wood);
    }

    // foliage: clusters at the tips, sized by the local branch order
    for (size_t n = 1; n < nodes.size(); ++n) {
        bool tip = true;
        for (size_t m = 1; m < nodes.size(); ++m)
            if (nodes[m].parent == int(n)) {
                tip = false;
                break;
            }
        const Node& nd = nodes[n];
        // Leaves follow the branch structure: every tip gets a cluster, and so
        // does every node in the outer part of the crown, so the canopy reads
        // as a continuous mass rather than as isolated puffs on stick ends.
        const float hFrac = nd.p.y / std::max(S.height, 1e-3f);
        const bool outer = hFrac > 0.45f;
        if (!tip && !outer)
            continue;
        // clusters are centred ON the branch, offset only slightly, so they
        // never float free of it
        const float sz = S.leafSize * (tip ? rng.range(0.8f, 1.2f)
                                           : rng.range(0.55f, 0.85f));
        const glm::vec3 c = org + nd.p + glm::vec3(rng.range(-0.05f, 0.05f),
                                                   rng.range(-0.02f, 0.12f),
                                                   rng.range(-0.05f, 0.05f));
        rasterCluster(gr, c, glm::vec3(sz, sz * 0.8f, sz), rng, S.leaf, 0.28f);
        if (tip) {
            const int extra = 2;
            for (int e = 0; e < extra; ++e) {
                const float sz2 = sz * rng.range(0.6f, 0.9f);
                const glm::vec3 off(rng.range(-sz * 0.7f, sz * 0.7f),
                                    rng.range(-sz * 0.3f, sz * 0.6f),
                                    rng.range(-sz * 0.7f, sz * 0.7f));
                rasterCluster(gr, c + off, glm::vec3(sz2, sz2 * 0.8f, sz2), rng,
                              S.leaf, 0.28f);
            }
        }
    }

    // ---- emit surface voxels only (solid with at least one air face)
    std::vector<VoxelRecord> out;
    for (int z = 0; z < gr.nz; ++z)
        for (int y = 0; y < gr.ny; ++y)
            for (int x = 0; x < gr.nx; ++x) {
                const size_t i = gr.idx(x, y, z);
                if (!gr.solid[i])
                    continue;
                // Emit the FULL solid volume, not just the shell. The loader
                // flood-fills an object component to a solid, but that needs a
                // watertight shell: a one-voxel shell of a rasterised cylinder
                // leaks, the fill escapes, and the trunk comes back as a
                // hollow tube (probed: solid wall, air core). Emitting the
                // interior costs a few thousand records per trunk and removes
                // the whole failure mode.
                const int lx = gr.x0 + x, ly = gr.y0 + y, lz = gr.z0 + z;
                if (lx < 0 || ly < 0 || lz < 0 || lx >= 1024 || ly >= 1024 || lz >= 1024)
                    continue;
                // Trunks/limbs carry the bark material (17) as a per-cell
                // texture override, so material 6 keeps the plank atlas slot
                // for the buildings.
                const uint8_t tex = gr.mat[i] == 6 ? uint8_t(17) : uint8_t(0);
                out.push_back(makeVoxelRecord(uint16_t(lx), uint16_t(ly), uint16_t(lz),
                                              gr.mat[i], gr.r[i], gr.g[i], gr.b[i],
                                              0, 0, tex));
            }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    uint64_t seed = 20260919ull;
    std::string outName = "hamlet_trees";
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            outName = argv[++i];
        else if (!std::strcmp(argv[i], "--dry-run"))
            dryRun = true;
        else {
            std::printf("usage: tree_gen [--seed N] [--out NAME] [--dry-run]\n");
            return 2;
        }
    }

    // Species mix: tall narrow conifers and shorter broad crowns, matching the
    // reference stand. Sites are the existing hamlet tree clusters.
    Species conifer;
    conifer.name = "conifer";
    conifer.height = 13.0f; conifer.clearTrunk = 2.2f; conifer.crownRad = 2.6f;
    conifer.coneShape = 1.0f; conifer.baseRad = 0.42f;
    conifer.influence = 2.2f; conifer.kill = 0.62f; conifer.step = 0.26f;
    conifer.attractors = 1900; conifer.leafSize = 0.34f;
    conifer.leaf = glm::vec3(44, 92, 38);

    Species broadleaf;
    broadleaf.name = "broadleaf";
    broadleaf.height = 9.5f; broadleaf.clearTrunk = 2.8f; broadleaf.crownRad = 3.4f;
    broadleaf.coneShape = 0.0f; broadleaf.baseRad = 0.52f;
    broadleaf.influence = 2.4f; broadleaf.kill = 0.66f; broadleaf.step = 0.28f;
    broadleaf.attractors = 2200; broadleaf.leafSize = 0.42f;
    broadleaf.leaf = glm::vec3(56, 108, 44);

    struct Site { float x, z; const Species* sp; };
    const Site sites[] = {
        { 17.45f, 25.45f, &conifer },
        { 21.95f, 21.95f, &broadleaf },
        { 27.45f, 23.95f, &conifer },
        { 24.45f, 16.95f, &broadleaf },
        { 29.45f, 18.45f, &conifer },
    };

    std::vector<VoxelRecord> all;
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        const Site& s = sites[i];
        std::vector<VoxelRecord> t =
            buildTree(*s.sp, s.x, s.z, seed + 0x1000ull * (i + 1));
        std::printf("  %-9s at (%.2f, %.2f): %zu surface voxels\n", s.sp->name,
                    s.x, s.z, t.size());
        all.insert(all.end(), t.begin(), t.end());
    }

    if (dryRun) {
        std::printf("dry run: %zu voxels total\n", all.size());
        return 0;
    }
    EditableWorld ew(std::string(VOXELFORGE_ASSET_DIR), "tree_gen_scratch.vxw",
                     "tree_gen_scratch", "object");
    if (!ew.writeObjectLayer(outName, all)) {
        std::fprintf(stderr, "tree_gen: failed to write layer '%s'\n", outName.c_str());
        return 1;
    }
    std::printf("tree_gen: wrote %zu voxels to %s.vxw (seed %llu)\n", all.size(),
                outName.c_str(), (unsigned long long)seed);
    return 0;
}
