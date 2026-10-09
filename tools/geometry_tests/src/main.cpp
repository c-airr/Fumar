// Tests for the modelling operations in fumar_geometry.
//
// Each test builds a small mesh, performs one operation, and checks two kinds
// of thing: the counts the operation should have produced, and that the mesh
// is still SOUND - every face wound consistently with its neighbours, no edge
// shared by three faces, no vertex left over. The second kind is the one that
// matters; see the CMakeLists beside this file for why.
//
// Closed meshes are also checked against Euler's formula, V - E + F = 2 for
// anything shaped like a sphere. An operation that miscounts its new edges or
// leaves a hole breaks that immediately, whatever the mesh looks like.

#include "fumar/geometry/editable_mesh.hpp"
#include "fumar/geometry/operations.hpp"
#include "fumar/geometry/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace fumar;
using namespace fumar::geometry;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    std::printf(condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) {
        ++failures;
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

usize edgeCount(const EditableMesh& mesh) { return mesh.buildEdges().edges.size(); }

/// validate() with its findings printed, so a failure says what went wrong.
bool sound(const EditableMesh& mesh) {
    const std::vector<std::string> problems = validate(mesh);
    for (const std::string& problem : problems) {
        std::printf("        %s\n", problem.c_str());
    }
    return problems.empty();
}

void checkCounts(const EditableMesh& mesh, usize v, usize e, usize f, const std::string& what) {
    const usize edges = edgeCount(mesh);
    const bool ok = mesh.vertexCount() == v && edges == e && mesh.faceCount() == f;
    check(ok, what + " has " + std::to_string(v) + "/" + std::to_string(e) + "/" + std::to_string(f) +
                  " V/E/F (got " + std::to_string(mesh.vertexCount()) + "/" + std::to_string(edges) + "/" +
                  std::to_string(mesh.faceCount()) + ")");
}

bool closedEuler(const EditableMesh& mesh) {
    const auto v = static_cast<i64>(mesh.vertexCount());
    const auto e = static_cast<i64>(edgeCount(mesh));
    const auto f = static_cast<i64>(mesh.faceCount());
    return v - e + f == 2;
}

/// Every face of a closed convex-ish shape points away from its middle.
bool facesPointOutward(const EditableMesh& mesh) {
    const Vec3 middle = mesh.centre();
    for (const Face& face : mesh.faces) {
        if (dot(faceNormal(mesh, face), faceCentroid(mesh, face) - middle) <= 0.0f) {
            return false;
        }
    }
    return true;
}

/// The face of a cube whose normal points along `direction`.
u32 faceFacing(const EditableMesh& mesh, Vec3 direction) {
    for (u32 f = 0; f < mesh.faces.size(); ++f) {
        if (dot(faceNormal(mesh, mesh.faces[f]), direction) > 0.99f) {
            return f;
        }
    }
    return 0;
}

/// Moves every vertex the selection covers.
void translate(EditableMesh& mesh, const Selection& selection, SelectMode mode, Vec3 offset) {
    for (const u32 v : coveredVertices(mesh, selection, mode)) {
        mesh.positions[v] += offset;
    }
}

bool approx(f32 a, f32 b) { return std::abs(a - b) < 1e-4f; }

// --- tests ---------------------------------------------------------------

void validationCatchesDamage() {
    section("validation itself");

    // A checker that passes everything is worse than none. These meshes are
    // broken on purpose, and must be reported as broken.
    EditableMesh flipped = makeCube();
    std::reverse(flipped.faces[0].verts.begin(), flipped.faces[0].verts.end());
    std::reverse(flipped.faces[0].uvs.begin(), flipped.faces[0].uvs.end());
    check(!validate(flipped).empty(), "one face turned inside out is caught");

    EditableMesh stray = makeCube();
    stray.positions.push_back(Vec3{5.0f, 5.0f, 5.0f});
    check(!validate(stray).empty(), "a vertex nothing uses is caught");

    EditableMesh fin = makeCube();
    fin.positions.push_back(Vec3{0.0f, 2.0f, 0.0f});
    fin.faces.push_back(Face{.verts = {6, 7, 8}, .uvs = {{0, 0}, {1, 0}, {0, 1}}});
    check(!validate(fin).empty(), "a third face on one edge is caught");
}

void primitives() {
    section("primitives");

    const EditableMesh cube = makeCube();
    checkCounts(cube, 8, 12, 6, "cube");
    check(sound(cube), "cube is sound");
    check(closedEuler(cube), "cube satisfies V - E + F = 2");
    check(facesPointOutward(cube), "cube faces point outward");

    const EditableMesh plane = makePlane(2.0f);
    checkCounts(plane, 4, 4, 1, "plane");
    check(sound(plane), "plane is sound");
    check(faceNormal(plane, plane.faces[0]).y > 0.99f, "plane faces up");

    const EditableMesh cylinder = makeCylinder(0.5f, 2.0f, 16);
    checkCounts(cylinder, 32, 48, 18, "16-sided cylinder");
    check(sound(cylinder), "cylinder is sound");
    check(closedEuler(cylinder), "cylinder satisfies V - E + F = 2");
    check(facesPointOutward(cylinder), "cylinder faces point outward");

    const EditableMesh sphere = makeUvSphere(1.0f, 12, 6);
    // 12 * 5 ring vertices plus two poles; 12 * 6 faces.
    check(sphere.vertexCount() == 62 && sphere.faceCount() == 72, "sphere has 62 vertices and 72 faces");
    check(sound(sphere), "sphere is sound");
    check(closedEuler(sphere), "sphere satisfies V - E + F = 2");
    check(facesPointOutward(sphere), "sphere faces point outward");
}

void roundTrip() {
    section("triangulate and back");

    const EditableMesh cube = makeCube();
    const TriangleMesh triangles = cube.triangulate();
    check(triangles.indices.size() == 36, "a cube is 12 triangles");
    check(triangles.positions.size() == 24, "with a vertex per corner per face, for flat normals");

    const EditableMesh exact = fromTriangles(triangles.positions, triangles.uvs, triangles.indices,
                                             triangles.polygonSizes);
    checkCounts(exact, 8, 12, 6, "cube rebuilt with polygon sizes");
    check(sound(exact), "and it is sound");

    const EditableMesh joined = fromTriangles(triangles.positions, triangles.uvs, triangles.indices);
    checkCounts(joined, 8, 12, 6, "cube rebuilt from bare triangles, pairs joined into quads");
    check(sound(joined), "and it is sound");

    const EditableMesh cylinder = makeCylinder(0.5f, 1.0f, 8);
    const TriangleMesh cylinderTriangles = cylinder.triangulate();
    const EditableMesh cylinderBack = fromTriangles(cylinderTriangles.positions, cylinderTriangles.uvs,
                                                    cylinderTriangles.indices, cylinderTriangles.polygonSizes);
    check(cylinderBack.faceCount() == 10 && cylinderBack.faces[8].size() == 8,
          "cylinder caps come back as single octagons");

    // Smoothing by angle: a box keeps its corners, a cylinder's side is smooth
    // while its rim stays sharp, and a sphere's normals point straight out.
    bool cubeFlat = true;
    for (usize i = 0; i < triangles.normals.size(); ++i) {
        const Vec3 n = triangles.normals[i];
        cubeFlat = cubeFlat && approx(std::abs(n.x) + std::abs(n.y) + std::abs(n.z), 1.0f);
    }
    check(cubeFlat, "a cube's corners stay sharp: every normal is a face normal");

    const TriangleMesh smoothSphere = makeUvSphere(1.0f, 24, 12).triangulate();
    bool radial = true;
    for (usize i = 0; i < smoothSphere.positions.size(); ++i) {
        radial = radial && dot(smoothSphere.normals[i], normalize(smoothSphere.positions[i])) > 0.995f;
    }
    check(radial, "a sphere's normals point straight out from its centre");

    const TriangleMesh roundSide = makeCylinder(0.5f, 1.0f, 32).triangulate();
    bool sideSmooth = true;
    bool capsFlat = true;
    for (usize i = 0; i < roundSide.normals.size(); ++i) {
        const Vec3 n = roundSide.normals[i];
        if (std::abs(n.y) > 0.5f) {
            capsFlat = capsFlat && approx(std::abs(n.y), 1.0f);
        } else {
            const Vec3 out = normalize(Vec3{roundSide.positions[i].x, 0.0f, roundSide.positions[i].z});
            sideSmooth = sideSmooth && dot(n, out) > 0.999f;
        }
    }
    check(sideSmooth && capsFlat, "a cylinder is smooth round its side and sharp at its rims");

    // A tetrahedron has no two triangles in one plane, so nothing is joined.
    const std::vector<Vec3> tetra{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const std::vector<u32> tetraIndices{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    const EditableMesh tetrahedron = fromTriangles(tetra, {}, tetraIndices);
    checkCounts(tetrahedron, 4, 6, 4, "tetrahedron stays four triangles");
    check(sound(tetrahedron), "and it is sound");
}

void extrudeFaces() {
    section("extrude faces");

    EditableMesh cube = makeCube();
    Selection top;
    top.faces.insert(faceFacing(cube, {0, 1, 0}));
    const Selection raised = extrude(cube, top, SelectMode::Face);
    translate(cube, raised, SelectMode::Face, {0.0f, 1.0f, 0.0f});

    checkCounts(cube, 12, 20, 10, "cube with its top extruded");
    check(sound(cube), "is sound");
    check(closedEuler(cube), "is still closed");
    check(raised.faces == top.faces, "the extruded face stays selected");
    check(facesPointOutward(cube), "every face still points outward");

    // Two neighbouring faces at once: one block, not two columns. No wall
    // between them, six walls round the outside.
    EditableMesh region = makeCube();
    Selection pair;
    pair.faces.insert(faceFacing(region, {0, 1, 0}));
    pair.faces.insert(faceFacing(region, {0, 0, 1}));
    const Selection grown = extrude(region, pair, SelectMode::Face);
    translate(region, grown, SelectMode::Face, {0.0f, 0.5f, 0.5f});
    checkCounts(region, 14, 24, 12, "cube with two adjacent faces extruded as a region");
    check(sound(region), "is sound");
    check(closedEuler(region), "is still closed");

    // An open plane grows walls on every side.
    EditableMesh plane = makePlane(1.0f);
    Selection only;
    only.faces.insert(0);
    extrude(plane, only, SelectMode::Face);
    checkCounts(plane, 8, 12, 5, "plane extruded into an open box");
    check(sound(plane), "is sound");
}

void extrudeEdgesAndVertices() {
    section("extrude edges and vertices");

    EditableMesh plane = makePlane(1.0f);
    Selection edge;
    edge.edges.insert(EdgeKey{0, 1});
    const Selection grown = extrude(plane, edge, SelectMode::Edge);
    translate(plane, grown, SelectMode::Edge, {0.0f, 0.0f, 1.0f});
    checkCounts(plane, 6, 7, 2, "plane with a border edge extruded");
    check(sound(plane), "is sound, the new quad wound to match its neighbour");
    check(grown.edges.size() == 1, "the new edge is selected");

    EditableMesh vertex = makePlane(1.0f);
    Selection corner;
    corner.vertices.insert(2);
    const Selection pulled = extrude(vertex, corner, SelectMode::Vertex);
    translate(vertex, pulled, SelectMode::Vertex, {0.0f, 1.0f, 0.0f});
    checkCounts(vertex, 5, 5, 1, "plane with a corner extruded into a loose edge");
    check(vertex.looseEdges.size() == 1, "the edge is loose");
    check(sound(vertex), "is sound");
}

void insetFaces() {
    section("inset");

    EditableMesh cube = makeCube();
    Selection top;
    top.faces.insert(faceFacing(cube, {0, 1, 0}));
    const Selection inner = inset(cube, top, 0.1f);
    checkCounts(cube, 12, 20, 10, "cube with its top inset");
    check(sound(cube), "is sound");
    check(closedEuler(cube), "is still closed");

    bool insideByThickness = true;
    for (const u32 v : cube.faces[*inner.faces.begin()].verts) {
        insideByThickness = insideByThickness && approx(std::abs(cube.positions[v].x), 0.4f) &&
                            approx(std::abs(cube.positions[v].z), 0.4f) && approx(cube.positions[v].y, 0.5f);
    }
    check(insideByThickness, "the inner face is exactly 0.1 in from every edge");
}

void loopCuts() {
    section("loop cut");

    // Vertices 4 and 6 are the front-left vertical edge. The loop runs round
    // the four sides; the top and bottom have no vertical edges and stay whole.
    EditableMesh cube = makeCube();
    const Selection loop = loopCut(cube, EdgeKey{4, 6});
    checkCounts(cube, 12, 20, 10, "cube cut round its middle");
    check(sound(cube), "is sound");
    check(closedEuler(cube), "is still closed");
    check(loop.edges.size() == 4, "the four new edges are selected");

    bool atMiddle = true;
    for (const EdgeKey key : loop.edges) {
        atMiddle = atMiddle && approx(cube.positions[key.a].y, 0.0f) && approx(cube.positions[key.b].y, 0.0f);
    }
    check(atMiddle, "and they run round the middle");

    // Starting on a rim edge of a cylinder: the side quad there is cut, and the
    // loop stops at the two caps, which gain a corner each instead of a crack.
    EditableMesh cylinder = makeCylinder(0.5f, 1.0f, 8);
    loopCut(cylinder, EdgeKey{0, 2});
    checkCounts(cylinder, 18, 27, 11, "cylinder cut from a rim edge");
    check(sound(cylinder), "is sound, the caps closed round the new corners");
    check(closedEuler(cylinder), "is still closed");

    EditableMesh plane = makePlane(1.0f);
    loopCut(plane, EdgeKey{0, 1});
    checkCounts(plane, 6, 7, 2, "a lone quad cut in two");
    check(sound(plane), "is sound");

    EditableMesh nothing = makeCube();
    check(loopCut(nothing, EdgeKey{0, 7}).empty(), "an edge that does not exist cuts nothing");
}

void deletion() {
    section("delete");

    EditableMesh openBox = makeCube();
    Selection top;
    top.faces.insert(faceFacing(openBox, {0, 1, 0}));
    deleteSelection(openBox, top, SelectMode::Face);
    checkCounts(openBox, 8, 12, 5, "cube without its top");
    check(sound(openBox), "is sound");

    EditableMesh corner = makeCube();
    Selection vertex;
    vertex.vertices.insert(7);
    deleteSelection(corner, vertex, SelectMode::Vertex);
    checkCounts(corner, 7, 9, 3, "cube without a corner");
    check(corner.looseEdges.empty(), "every remaining edge still has a face");
    check(sound(corner), "is sound");

    EditableMesh lone = makePlane(1.0f);
    Selection edge;
    edge.edges.insert(EdgeKey{0, 1});
    deleteSelection(lone, edge, SelectMode::Edge);
    checkCounts(lone, 4, 3, 0, "a quad without one edge");
    check(lone.looseEdges.size() == 3, "leaves its other three edges loose");
    check(sound(lone), "is sound");
}

void merging() {
    section("merge");

    EditableMesh cube = makeCube();
    Selection top;
    top.faces.insert(faceFacing(cube, {0, 1, 0}));
    const Selection point = mergeAtCentre(cube, top, SelectMode::Face);
    checkCounts(cube, 5, 8, 5, "cube with its top merged into a pyramid");
    check(sound(cube), "is sound");
    check(closedEuler(cube), "is still closed");
    check(point.vertices.size() == 1 && approx(cube.positions[*point.vertices.begin()].y, 0.5f) &&
              approx(cube.positions[*point.vertices.begin()].x, 0.0f),
          "the apex sits at the middle of the old top");
}

void filling() {
    section("fill");

    EditableMesh fromEdges = makeCube();
    Selection top;
    top.faces.insert(faceFacing(fromEdges, {0, 1, 0}));
    const Face lid = fromEdges.faces[*top.faces.begin()];
    deleteSelection(fromEdges, top, SelectMode::Face);

    Selection rim;
    for (usize i = 0; i < lid.size(); ++i) {
        rim.edges.insert(EdgeKey{lid.verts[i], lid.verts[(i + 1) % lid.size()]});
    }
    const Selection filled = fill(fromEdges, rim, SelectMode::Edge);
    checkCounts(fromEdges, 8, 12, 6, "open box closed again from its rim edges");
    check(sound(fromEdges), "is sound, the lid wound to match the walls");
    check(!filled.faces.empty() && faceNormal(fromEdges, fromEdges.faces[*filled.faces.begin()]).y > 0.99f,
          "and the lid faces up");

    EditableMesh fromVertices = makeCube();
    deleteSelection(fromVertices, top, SelectMode::Face);
    Selection corners;
    corners.vertices.insert(lid.verts.begin(), lid.verts.end());
    fill(fromVertices, corners, SelectMode::Vertex);
    checkCounts(fromVertices, 8, 12, 6, "open box closed again from four corners");
    check(sound(fromVertices), "is sound");

    EditableMesh twoEdges;
    twoEdges.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}};
    twoEdges.looseEdges = {EdgeKey{0, 1}, EdgeKey{2, 3}};
    Selection pair;
    pair.edges = {EdgeKey{0, 1}, EdgeKey{2, 3}};
    fill(twoEdges, pair, SelectMode::Edge);
    checkCounts(twoEdges, 4, 4, 1, "two loose edges bridged by a quad");
    check(twoEdges.looseEdges.empty(), "which uses up the loose edges");
    check(sound(twoEdges), "is sound");
    check(twoEdges.buildEdges().find(EdgeKey{1, 3}) != nullptr && twoEdges.buildEdges().find(EdgeKey{1, 2}) == nullptr,
          "and not twisted: the new sides join the near ends, not the far ones");

    EditableMesh whole = makeCube();
    Selection existing;
    existing.vertices = {6, 7, 3, 2};
    check(fill(whole, existing, SelectMode::Vertex).empty(), "filling an existing face adds nothing");
}

void subdivision() {
    section("subdivide");

    EditableMesh one = makeCube();
    Selection top;
    top.faces.insert(faceFacing(one, {0, 1, 0}));
    const Selection quarters = subdivide(one, top);
    checkCounts(one, 13, 20, 9, "cube with its top subdivided");
    check(quarters.faces.size() == 4, "into four selected quads");
    check(sound(one), "is sound, the side faces given the new midpoints");
    check(closedEuler(one), "is still closed");

    EditableMesh all = makeCube();
    subdivide(all, selectAll(all, SelectMode::Face));
    checkCounts(all, 26, 48, 24, "cube subdivided everywhere");
    check(sound(all), "is sound");
    check(closedEuler(all), "is still closed");
}

void flipping() {
    section("flip normals");

    EditableMesh cube = makeCube();
    flipNormals(cube, selectAll(cube, SelectMode::Face));
    check(sound(cube), "a cube flipped everywhere is still consistent");

    bool inward = true;
    for (const Face& face : cube.faces) {
        inward = inward && dot(faceNormal(cube, face), faceCentroid(cube, face)) < 0.0f;
    }
    check(inward, "and every face points inward");
}

void chains() {
    section("operations one after another");

    // The sequence the editor's manual test performs: extrude the top up,
    // inset it, cut a loop round the side, then subdivide everything.
    EditableMesh mesh = makeCube();
    Selection top;
    top.faces.insert(faceFacing(mesh, {0, 1, 0}));
    const Selection raised = extrude(mesh, top, SelectMode::Face);
    translate(mesh, raised, SelectMode::Face, {0.0f, 1.0f, 0.0f});
    const Selection inner = inset(mesh, raised, 0.15f);
    const Selection pushed = extrude(mesh, inner, SelectMode::Face);
    translate(mesh, pushed, SelectMode::Face, {0.0f, 0.5f, 0.0f});
    loopCut(mesh, EdgeKey{4, 6});
    subdivide(mesh, selectAll(mesh, SelectMode::Face));

    check(sound(mesh), "extrude, inset, extrude, loop cut, subdivide: still sound");
    check(closedEuler(mesh), "and still closed");

    const TriangleMesh triangles = mesh.triangulate();
    const EditableMesh back =
        fromTriangles(triangles.positions, triangles.uvs, triangles.indices, triangles.polygonSizes);
    check(back.vertexCount() == mesh.vertexCount() && back.faceCount() == mesh.faceCount(),
          "and it survives the trip through triangles");
}

} // namespace

int main() {
    std::printf("fumar geometry tests\n");

    validationCatchesDamage();
    primitives();
    roundTrip();
    extrudeFaces();
    extrudeEdgesAndVertices();
    insetFaces();
    loopCuts();
    deletion();
    merging();
    filling();
    subdivision();
    flipping();
    chains();

    std::printf(failures == 0 ? "\nall good\n" : "\n%d check(s) failed\n", failures);
    return failures;
}
