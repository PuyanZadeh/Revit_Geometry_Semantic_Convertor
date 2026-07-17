// Phase 2, Layer 1: geometric interrogation.
//
// Independent of the semantic engine's CODE (ifc_aiquery) -- no shared code,
// no call into aiquery's query pipeline, ever. This is NOT the same as being
// independent of the standardized model BUNDLE (GLB + semantic.json +
// map.json), which together are the platform's canonical representation of
// a model. Reading semantic.json directly (own JSON parsing, no aiquery.cpp
// involvement) is reading another bundle artifact, not a semantic-engine
// dependency -- see build_revit_id_lookup below for the one place this
// engine currently does so, and why.
//
// The caller supplies element IDs directly; this engine never resolves a
// clicked point or ambiguous reference.
//
// Only reads what interrogation needs from the glTF 2.0 binary (GLB)
// container: the JSON chunk's nodes/meshes/accessors/bufferViews/buffers,
// plus the BIN chunk's raw bytes. No materials, textures, animations,
// skins, cameras, or extensions -- a hand-rolled minimal reader rather than
// a vendored library, consistent with every other engine in this codebase.
#include "ifc_geom.h"
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

extern "C"
{
    // ------------------------------------------------------------------
    // Storage path resolution -- an independent copy of the same directory
    // convention ifc_aiquery uses, reimplemented locally rather than shared,
    // so this engine has no build/link dependency on the semantic engine.
    // ------------------------------------------------------------------
    static fs::path exe_dir() { return fs::canonical("/proc/self/exe").parent_path(); }
    static fs::path server_root() { return exe_dir().parent_path(); }
    static fs::path data_root() { return server_root().parent_path() / "storage"; }
    static const std::string GLTF_DIR = (data_root() / "outputs" / "gltf").string() + "/";

    static bool ends_with(const std::string &s, const std::string &suffix)
    {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Strips a known model-name suffix (".map.json" checked before the
    // shorter ".json" it would otherwise also match, then ".glb") down to
    // the bare base name shared by every file in the bundle -- "<name>.glb",
    // "<name>.json", and "<name>.map.json" all refer to the same model.
    static std::string model_base_name(const std::string &model)
    {
        if (ends_with(model, ".map.json"))
            return model.substr(0, model.size() - std::string(".map.json").size());
        if (ends_with(model, ".glb"))
            return model.substr(0, model.size() - std::string(".glb").size());
        if (ends_with(model, ".json"))
            return model.substr(0, model.size() - std::string(".json").size());
        return model;
    }

    // Accepts a raw ".glb" name as-is, or strips a ".map.json"/".json"
    // suffix and substitutes ".glb" -- the same model-name normalization
    // spirit the semantic engine uses, reimplemented independently here.
    static std::string resolve_glb_path(const std::string &model)
    {
        fs::path p(model);
        if (p.is_absolute())
            return p.string();

        return GLTF_DIR + model_base_name(model) + ".glb";
    }

    // TEMP-DEBUG(revit-intid-qc): resolves the bundle's semantic.json path
    // for the same model -- reading another artifact of the standardized
    // bundle (see file-header comment), not the semantic engine's code.
    // Used only to support the temporary Revit ElementId QC convenience
    // below; remove alongside it once the bundle is re-keyed around IFC
    // GlobalId (see build_revit_id_lookup for the full technical-debt note).
    static std::string resolve_semantic_json_path(const std::string &model)
    {
        fs::path p(model);
        if (p.is_absolute())
            return (p.parent_path() / (model_base_name(p.filename().string()) + ".json")).string();

        return GLTF_DIR + model_base_name(model) + ".json";
    }

    // ------------------------------------------------------------------
    // Minimal GLB (binary glTF) reader.
    // ------------------------------------------------------------------
    struct GlbFile
    {
        void *mapped = nullptr;
        size_t mapped_size = 0;
        int fd = -1;
        json gltf;                    // parsed JSON chunk
        const uint8_t *bin = nullptr; // pointer into `mapped` for the BIN chunk
        size_t bin_len = 0;

        GlbFile() = default;
        GlbFile(const GlbFile &) = delete;
        GlbFile &operator=(const GlbFile &) = delete;

        ~GlbFile()
        {
            if (mapped && mapped != MAP_FAILED)
                munmap(mapped, mapped_size);
            if (fd >= 0)
                close(fd);
        }
    };

    static bool load_glb(const std::string &path, GlbFile &out)
    {
        out.fd = open(path.c_str(), O_RDONLY);
        if (out.fd < 0)
            return false;

        struct stat st;
        if (fstat(out.fd, &st) != 0 || st.st_size < 12)
            return false;

        out.mapped_size = static_cast<size_t>(st.st_size);
        out.mapped = mmap(nullptr, out.mapped_size, PROT_READ, MAP_PRIVATE, out.fd, 0);
        if (out.mapped == MAP_FAILED)
            return false;

        const uint8_t *base = static_cast<const uint8_t *>(out.mapped);

        uint32_t magic = 0, version = 0, total_length = 0;
        std::memcpy(&magic, base + 0, 4);
        std::memcpy(&version, base + 4, 4);
        std::memcpy(&total_length, base + 8, 4);
        (void)version;

        static const uint32_t GLB_MAGIC = 0x46546C67; // "glTF"
        if (magic != GLB_MAGIC || total_length > out.mapped_size)
            return false;

        static const uint32_t CHUNK_JSON = 0x4E4F534A; // "JSON"
        static const uint32_t CHUNK_BIN = 0x004E4942;  // "BIN\0"

        size_t offset = 12;
        bool have_json = false;

        while (offset + 8 <= total_length)
        {
            uint32_t chunk_len = 0, chunk_type = 0;
            std::memcpy(&chunk_len, base + offset, 4);
            std::memcpy(&chunk_type, base + offset + 4, 4);
            offset += 8;

            if (offset + chunk_len > total_length)
                break;

            if (chunk_type == CHUNK_JSON)
            {
                try
                {
                    out.gltf = json::parse(reinterpret_cast<const char *>(base + offset),
                                            reinterpret_cast<const char *>(base + offset + chunk_len));
                    have_json = true;
                }
                catch (...)
                {
                    return false;
                }
            }
            else if (chunk_type == CHUNK_BIN)
            {
                out.bin = base + offset;
                out.bin_len = chunk_len;
            }

            offset += chunk_len; // chunk_len already includes spec-mandated padding
        }

        return have_json;
    }

    // ------------------------------------------------------------------
    // Node transform composition. glTF stores parent->children only, so a
    // node's own transform is found by walking DOWN from the scene's
    // declared roots, accumulating each ancestor's local transform. This is
    // written generally (works for any node depth) even though this
    // project's own exports are verified flat (every element is a 2-node
    // pair that is itself a scene root, with no node anywhere carrying a
    // matrix/TRS -- i.e. an identity transform in every real case seen so
    // far, but the composition is still done properly for correctness on
    // any valid glTF file).
    // ------------------------------------------------------------------
    struct Mat4
    {
        double m[16]; // column-major, matching glTF's convention
    };

    static Mat4 identity_mat4()
    {
        Mat4 r{};
        r.m[0] = 1;
        r.m[5] = 1;
        r.m[10] = 1;
        r.m[15] = 1;
        return r;
    }

    static Mat4 mul_mat4(const Mat4 &a, const Mat4 &b)
    {
        Mat4 r{};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
            {
                double sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += a.m[k * 4 + row] * b.m[col * 4 + k];
                r.m[col * 4 + row] = sum;
            }
        return r;
    }

    static Mat4 mat4_from_trs(const json &node)
    {
        double tx = 0, ty = 0, tz = 0;
        if (node.contains("translation") && node["translation"].is_array() && node["translation"].size() == 3)
        {
            tx = node["translation"][0].get<double>();
            ty = node["translation"][1].get<double>();
            tz = node["translation"][2].get<double>();
        }

        double qx = 0, qy = 0, qz = 0, qw = 1;
        if (node.contains("rotation") && node["rotation"].is_array() && node["rotation"].size() == 4)
        {
            qx = node["rotation"][0].get<double>();
            qy = node["rotation"][1].get<double>();
            qz = node["rotation"][2].get<double>();
            qw = node["rotation"][3].get<double>();
        }

        double sx = 1, sy = 1, sz = 1;
        if (node.contains("scale") && node["scale"].is_array() && node["scale"].size() == 3)
        {
            sx = node["scale"][0].get<double>();
            sy = node["scale"][1].get<double>();
            sz = node["scale"][2].get<double>();
        }

        double xx = qx * qx, yy = qy * qy, zz = qz * qz;
        double xy = qx * qy, xz = qx * qz, yz = qy * qz;
        double wx = qw * qx, wy = qw * qy, wz = qw * qz;

        Mat4 r{};
        r.m[0] = (1 - 2 * (yy + zz)) * sx;
        r.m[1] = (2 * (xy + wz)) * sx;
        r.m[2] = (2 * (xz - wy)) * sx;
        r.m[3] = 0;

        r.m[4] = (2 * (xy - wz)) * sy;
        r.m[5] = (1 - 2 * (xx + zz)) * sy;
        r.m[6] = (2 * (yz + wx)) * sy;
        r.m[7] = 0;

        r.m[8] = (2 * (xz + wy)) * sz;
        r.m[9] = (2 * (yz - wx)) * sz;
        r.m[10] = (1 - 2 * (xx + yy)) * sz;
        r.m[11] = 0;

        r.m[12] = tx;
        r.m[13] = ty;
        r.m[14] = tz;
        r.m[15] = 1;

        return r;
    }

    static Mat4 node_local_transform(const json &node)
    {
        if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16)
        {
            Mat4 r{};
            for (int i = 0; i < 16; ++i)
                r.m[i] = node["matrix"][i].get<double>();
            return r;
        }

        return mat4_from_trs(node);
    }

    static std::array<double, 3> transform_point(const Mat4 &m, const std::array<double, 3> &p)
    {
        double x = m.m[0] * p[0] + m.m[4] * p[1] + m.m[8] * p[2] + m.m[12];
        double y = m.m[1] * p[0] + m.m[5] * p[1] + m.m[9] * p[2] + m.m[13];
        double z = m.m[2] * p[0] + m.m[6] * p[1] + m.m[10] * p[2] + m.m[14];
        return {x, y, z};
    }

    static void accumulate_transforms(const json &gltf, int node_index, const Mat4 &parent_world,
                                       std::unordered_map<int, Mat4> &world_transforms)
    {
        const json &node = gltf["nodes"][static_cast<size_t>(node_index)];
        Mat4 world = mul_mat4(parent_world, node_local_transform(node));
        world_transforms[node_index] = world;

        if (node.contains("children"))
            for (const auto &child : node["children"])
                accumulate_transforms(gltf, child.get<int>(), world, world_transforms);
    }

    static std::unordered_map<int, Mat4> compute_world_transforms(const json &gltf)
    {
        std::unordered_map<int, Mat4> world_transforms;
        Mat4 id = identity_mat4();

        if (gltf.contains("scene") && gltf.contains("scenes"))
        {
            int scene_index = gltf["scene"].get<int>();
            const json &scene = gltf["scenes"][static_cast<size_t>(scene_index)];
            if (scene.contains("nodes"))
                for (const auto &root : scene["nodes"])
                    accumulate_transforms(gltf, root.get<int>(), id, world_transforms);
        }

        return world_transforms;
    }

    // ------------------------------------------------------------------
    // Accessor reading.
    // ------------------------------------------------------------------
    static size_t component_byte_size(int component_type)
    {
        switch (component_type)
        {
        case 5120: return 1; // BYTE
        case 5121: return 1; // UNSIGNED_BYTE
        case 5122: return 2; // SHORT
        case 5123: return 2; // UNSIGNED_SHORT
        case 5125: return 4; // UNSIGNED_INT
        case 5126: return 4; // FLOAT
        default: return 0;
        }
    }

    // POSITION accessors are always FLOAT VEC3 per the glTF 2.0 spec, so
    // restricting to that combination is spec compliance, not an
    // implementation shortcut.
    static bool read_vec3_accessor(const json &gltf, const uint8_t *bin, size_t bin_len, int accessor_index,
                                    std::vector<std::array<double, 3>> &out)
    {
        const json &accessor = gltf["accessors"][static_cast<size_t>(accessor_index)];
        if (accessor.value("type", "") != "VEC3" || accessor.value("componentType", 0) != 5126)
            return false;
        if (!accessor.contains("bufferView") || !bin)
            return false;

        const json &bv = gltf["bufferViews"][accessor["bufferView"].get<size_t>()];

        size_t accessor_offset = accessor.value("byteOffset", static_cast<size_t>(0));
        size_t bv_offset = bv.value("byteOffset", static_cast<size_t>(0));
        size_t stride = bv.value("byteStride", static_cast<size_t>(0));
        const size_t elem_size = 4 * 3; // 3 floats
        if (stride == 0)
            stride = elem_size;

        size_t count = accessor.value("count", static_cast<size_t>(0));
        size_t base_offset = bv_offset + accessor_offset;

        out.clear();
        out.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            size_t off = base_offset + i * stride;
            if (off + elem_size > bin_len)
                return false;

            float x, y, z;
            std::memcpy(&x, bin + off, 4);
            std::memcpy(&y, bin + off + 4, 4);
            std::memcpy(&z, bin + off + 8, 4);
            out.push_back({static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)});
        }

        return true;
    }

    // Handles every unsigned index component type the glTF spec allows
    // (UNSIGNED_BYTE/UNSIGNED_SHORT/UNSIGNED_INT) even though every real
    // export seen so far uses UNSIGNED_INT -- a robust reader shouldn't
    // assume one file's encoding choice is universal.
    static bool read_index_accessor(const json &gltf, const uint8_t *bin, size_t bin_len, int accessor_index,
                                     std::vector<uint32_t> &out)
    {
        const json &accessor = gltf["accessors"][static_cast<size_t>(accessor_index)];
        if (accessor.value("type", "") != "SCALAR")
            return false;

        int component_type = accessor.value("componentType", 0);
        size_t comp_size = component_byte_size(component_type);
        if (comp_size == 0 || (component_type != 5121 && component_type != 5123 && component_type != 5125))
            return false; // signed BYTE/SHORT are not valid index types per spec
        if (!accessor.contains("bufferView") || !bin)
            return false;

        const json &bv = gltf["bufferViews"][accessor["bufferView"].get<size_t>()];

        size_t accessor_offset = accessor.value("byteOffset", static_cast<size_t>(0));
        size_t bv_offset = bv.value("byteOffset", static_cast<size_t>(0));
        size_t stride = bv.value("byteStride", static_cast<size_t>(0));
        if (stride == 0)
            stride = comp_size;

        size_t count = accessor.value("count", static_cast<size_t>(0));
        size_t base_offset = bv_offset + accessor_offset;

        out.clear();
        out.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            size_t off = base_offset + i * stride;
            if (off + comp_size > bin_len)
                return false;

            uint32_t value = 0;
            if (component_type == 5121)
            {
                uint8_t v;
                std::memcpy(&v, bin + off, 1);
                value = v;
            }
            else if (component_type == 5123)
            {
                uint16_t v;
                std::memcpy(&v, bin + off, 2);
                value = v;
            }
            else
            {
                uint32_t v;
                std::memcpy(&v, bin + off, 4);
                value = v;
            }

            out.push_back(value);
        }

        return true;
    }

    // ------------------------------------------------------------------
    // Element -> node resolution. The caller-supplied element ID is matched
    // directly against each node's own "revitID" tag -- no map.json, no
    // semantic.json, no dependency on the semantic engine at all. A "group"
    // node sharing the same revitID but carrying no "mesh" key is not
    // geometry-bearing and is skipped; only actual mesh nodes contribute.
    //
    // Factual note on "revitID": despite its name, this GLB node property
    // holds a uniqueId-format string (matching semantic.json's uniqueId),
    // not a Revit ElementId/intId. It is emitted by the vendored
    // IfcOpenShell/Bonsai glTF serializer, upstream of this repository --
    // an external bundle inconsistency, not something this file defines or
    // can rename. Treat it here only as an opaque current-GLB locator; do
    // not read or document it elsewhere as if it meant Revit ID.
    // ------------------------------------------------------------------
    static std::vector<int> find_mesh_nodes_for_element(const json &gltf, const std::string &element_id)
    {
        std::vector<int> matches;
        if (!gltf.contains("nodes"))
            return matches;

        const auto &nodes = gltf["nodes"];
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const json &node = nodes[i];
            if (node.value("revitID", "") == element_id && node.contains("mesh"))
                matches.push_back(static_cast<int>(i));
        }

        return matches;
    }

    struct ElementGeometry
    {
        std::vector<std::array<double, 3>> vertices;
        std::vector<std::array<uint32_t, 3>> faces; // indices into `vertices`
        bool found = false;
    };

    // Aggregates world-space vertices/faces across every mesh node matching
    // `element_id` (currently always exactly one in this project's own
    // exports, but this does not assume that -- an element whose geometry
    // spans more than one mesh node is handled by concatenating them, with
    // each node's face indices offset to the concatenated vertex array).
    static ElementGeometry extract_element_geometry(const GlbFile &glb,
                                                     const std::unordered_map<int, Mat4> &world_transforms,
                                                     const std::string &element_id)
    {
        ElementGeometry result;
        std::vector<int> mesh_nodes = find_mesh_nodes_for_element(glb.gltf, element_id);

        for (int node_index : mesh_nodes)
        {
            const json &node = glb.gltf["nodes"][static_cast<size_t>(node_index)];
            int mesh_index = node["mesh"].get<int>();
            const json &mesh = glb.gltf["meshes"][static_cast<size_t>(mesh_index)];

            auto wt_it = world_transforms.find(node_index);
            Mat4 world = (wt_it != world_transforms.end()) ? wt_it->second : identity_mat4();

            if (!mesh.contains("primitives"))
                continue;

            for (const auto &prim : mesh["primitives"])
            {
                int mode = prim.value("mode", 4);
                if (mode != 4) // only TRIANGLES supported by this layer
                    continue;

                if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION"))
                    continue;

                int pos_accessor = prim["attributes"]["POSITION"].get<int>();

                std::vector<std::array<double, 3>> local_positions;
                if (!read_vec3_accessor(glb.gltf, glb.bin, glb.bin_len, pos_accessor, local_positions))
                    continue;

                size_t vertex_base = result.vertices.size();
                for (const auto &p : local_positions)
                    result.vertices.push_back(transform_point(world, p));

                if (prim.contains("indices"))
                {
                    std::vector<uint32_t> idx;
                    if (read_index_accessor(glb.gltf, glb.bin, glb.bin_len, prim["indices"].get<int>(), idx))
                    {
                        for (size_t i = 0; i + 2 < idx.size(); i += 3)
                            result.faces.push_back({static_cast<uint32_t>(vertex_base) + idx[i],
                                                     static_cast<uint32_t>(vertex_base) + idx[i + 1],
                                                     static_cast<uint32_t>(vertex_base) + idx[i + 2]});
                    }
                }
                else
                {
                    // Non-indexed triangle list: every 3 consecutive vertices form a face.
                    for (size_t i = 0; i + 2 < local_positions.size(); i += 3)
                        result.faces.push_back({static_cast<uint32_t>(vertex_base + i),
                                                 static_cast<uint32_t>(vertex_base + i + 1),
                                                 static_cast<uint32_t>(vertex_base + i + 2)});
                }

                result.found = true;
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    // Primitive computations.
    // ------------------------------------------------------------------
    static json point_to_json(const std::array<double, 3> &p)
    {
        return json({{"x", p[0]}, {"y", p[1]}, {"z", p[2]}});
    }

    static json compute_vertices(const ElementGeometry &geom)
    {
        json arr = json::array();
        for (const auto &v : geom.vertices)
            arr.push_back(point_to_json(v));
        return arr;
    }

    static json compute_faces(const ElementGeometry &geom)
    {
        json arr = json::array();
        for (const auto &f : geom.faces)
            arr.push_back(json::array({f[0], f[1], f[2]}));
        return arr;
    }

    // Deduplicates triangle edges as unordered vertex-index pairs, but
    // preserves FIRST-ENCOUNTER order via a vector -- an unordered_set alone
    // would leave output order dependent on hash-bucket layout, which is
    // not acceptable given the determinism requirement. Named "meshEdges"
    // deliberately: these are the triangulation's topological edges, not a
    // claim about which edges a person would call "the edges of the shape."
    static json compute_mesh_edges(const ElementGeometry &geom)
    {
        std::unordered_set<uint64_t> seen;
        json arr = json::array();

        auto add_edge = [&](uint32_t a, uint32_t b)
        {
            uint32_t lo = std::min(a, b), hi = std::max(a, b);
            uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
            if (seen.insert(key).second)
                arr.push_back(json::array({lo, hi}));
        };

        for (const auto &f : geom.faces)
        {
            add_edge(f[0], f[1]);
            add_edge(f[1], f[2]);
            add_edge(f[2], f[0]);
        }

        return arr;
    }

    // Area-weighted centroid of the triangulated SURFACE -- not a naive
    // vertex average (which would be skewed by uneven tessellation
    // density), and explicitly not a solid/volume centroid (that belongs to
    // a future area/volume layer). Returns false only when the mesh has no
    // area at all (degenerate/empty geometry).
    static bool compute_surface_centroid(const ElementGeometry &geom, std::array<double, 3> &out)
    {
        double total_area = 0.0;
        std::array<double, 3> weighted{0.0, 0.0, 0.0};

        for (const auto &f : geom.faces)
        {
            const auto &v0 = geom.vertices[f[0]];
            const auto &v1 = geom.vertices[f[1]];
            const auto &v2 = geom.vertices[f[2]];

            std::array<double, 3> e1 = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
            std::array<double, 3> e2 = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
            std::array<double, 3> cross = {e1[1] * e2[2] - e1[2] * e2[1],
                                            e1[2] * e2[0] - e1[0] * e2[2],
                                            e1[0] * e2[1] - e1[1] * e2[0]};
            double area = 0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);

            std::array<double, 3> tri_centroid = {(v0[0] + v1[0] + v2[0]) / 3.0,
                                                   (v0[1] + v1[1] + v2[1]) / 3.0,
                                                   (v0[2] + v1[2] + v2[2]) / 3.0};

            weighted[0] += area * tri_centroid[0];
            weighted[1] += area * tri_centroid[1];
            weighted[2] += area * tri_centroid[2];
            total_area += area;
        }

        if (total_area <= 0.0)
            return false;

        out = {weighted[0] / total_area, weighted[1] / total_area, weighted[2] / total_area};
        return true;
    }

    // Extents (axis-aligned min/max over all vertices) -- the basis for
    // extentsCenter. Computed from actual world-space vertices, not from
    // accessor min/max metadata (which is pre-transform, local-space only).
    static bool compute_extents(const ElementGeometry &geom, std::array<double, 3> &mn, std::array<double, 3> &mx)
    {
        if (geom.vertices.empty())
            return false;

        mn = geom.vertices[0];
        mx = geom.vertices[0];

        for (const auto &v : geom.vertices)
            for (int a = 0; a < 3; ++a)
            {
                mn[a] = std::min(mn[a], v[a]);
                mx[a] = std::max(mx[a], v[a]);
            }

        return true;
    }

    // "Lowest"/"highest" are defined along Y, per the glTF 2.0 spec's
    // mandated Y-up convention -- verified empirically for this project's
    // own exports (the scene-wide Y range matches this model's own Level
    // elevations almost exactly), not merely assumed. Ties resolve to the
    // first-encountered vertex in accessor/node order, which is fixed and
    // deterministic.
    static bool compute_lowest_point(const ElementGeometry &geom, std::array<double, 3> &out)
    {
        if (geom.vertices.empty())
            return false;

        out = geom.vertices[0];
        for (const auto &v : geom.vertices)
            if (v[1] < out[1])
                out = v;

        return true;
    }

    static bool compute_highest_point(const ElementGeometry &geom, std::array<double, 3> &out)
    {
        if (geom.vertices.empty())
            return false;

        out = geom.vertices[0];
        for (const auto &v : geom.vertices)
            if (v[1] > out[1])
                out = v;

        return true;
    }

    // ------------------------------------------------------------------
    // Phase 2, Layer 2: geometry calculations.
    //
    // Scope is deliberately minimal by design (see project memory for the
    // scoping rationale): point-to-point distance, axis-constrained
    // point-to-point difference, and surface area of a single supplied
    // element. Volume was investigated and deliberately
    // removed -- real exported elements frequently have non-manifold mesh
    // artifacts (verified empirically: duplicate coincident triangles,
    // deeper anomalies beyond that) that a closedness check can't reliably
    // resolve without an unjustified, ever-growing stack of heuristics.
    // Deferred to a future, more advanced implementation -- see project
    // memory. Nothing here decides anything the caller didn't explicitly
    // supply -- distance takes two explicit points; surface area takes an
    // explicit elementId and reads exactly the geometry Layer 1 already
    // extracts, via the same unmodified extract_element_geometry used above.
    // No Layer 1 function is changed to support this section.
    // ------------------------------------------------------------------

    // Sum of per-triangle areas (same cross-product formula
    // compute_surface_centroid already uses internally as a weight,
    // independently duplicated here rather than refactoring Layer 1 code to
    // share it). A mesh with zero triangles has a well-defined area of
    // zero -- that is a valid answer, not an error; "element not found" is
    // handled separately, before this is ever called.
    static double compute_surface_area(const ElementGeometry &geom)
    {
        double total_area = 0.0;

        for (const auto &f : geom.faces)
        {
            const auto &v0 = geom.vertices[f[0]];
            const auto &v1 = geom.vertices[f[1]];
            const auto &v2 = geom.vertices[f[2]];

            std::array<double, 3> e1 = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
            std::array<double, 3> e2 = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
            std::array<double, 3> cross = {e1[1] * e2[2] - e1[2] * e2[1],
                                            e1[2] * e2[0] - e1[0] * e2[2],
                                            e1[0] * e2[1] - e1[1] * e2[0]};

            total_area += 0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
        }

        return total_area;
    }

    // ------------------------------------------------------------------
    // Phase 2, Layer 3: geometry reasoning.
    //
    // Scope is exactly one capability by design (see project memory for the
    // full scoping rationale, including two rejected reformulations of a
    // "signed separation" capability): minimum distance between two
    // explicitly supplied full geometries. This is a search -- the closest
    // point pair is unknown in advance -- so it operates on entire meshes,
    // not on any pre-selected feature, unlike Layer 2's calculate_distance
    // which takes caller-supplied points directly. True surface-to-surface
    // distance is computed (point-to-triangle and edge-to-edge across
    // every triangle pair), never a vertex-to-vertex approximation, which
    // would silently give a wrong answer whenever the true closest
    // approach lands on a face interior or along an edge rather than at a
    // vertex. Brute-force O(nA*nB) by design -- no BVH or spatial index
    // until profiling proves one is needed. Robust to non-manifold,
    // multi-body, inconsistently wound real exported geometry, because it
    // only ever measures point-set distance -- no notion of
    // interior/exterior is involved, unlike the containment/intersection
    // capabilities deferred in project memory.
    // ------------------------------------------------------------------

    // Closest point on triangle (a,b,c) to point p, via the standard
    // barycentric-region test (Ericson, "Real-Time Collision Detection").
    // Handles vertex, edge, and face regions in one pass.
    static std::array<double, 3> closest_point_on_triangle(const std::array<double, 3> &p,
                                                            const std::array<double, 3> &a,
                                                            const std::array<double, 3> &b,
                                                            const std::array<double, 3> &c)
    {
        auto sub = [](const std::array<double, 3> &x, const std::array<double, 3> &y) -> std::array<double, 3>
        { return {x[0] - y[0], x[1] - y[1], x[2] - y[2]}; };
        auto add = [](const std::array<double, 3> &x, const std::array<double, 3> &y) -> std::array<double, 3>
        { return {x[0] + y[0], x[1] + y[1], x[2] + y[2]}; };
        auto scale = [](const std::array<double, 3> &x, double s) -> std::array<double, 3>
        { return {x[0] * s, x[1] * s, x[2] * s}; };
        auto dot = [](const std::array<double, 3> &x, const std::array<double, 3> &y) -> double
        { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };

        std::array<double, 3> ab = sub(b, a);
        std::array<double, 3> ac = sub(c, a);
        std::array<double, 3> ap = sub(p, a);

        double d1 = dot(ab, ap);
        double d2 = dot(ac, ap);
        if (d1 <= 0.0 && d2 <= 0.0)
            return a;

        std::array<double, 3> bp = sub(p, b);
        double d3 = dot(ab, bp);
        double d4 = dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3)
            return b;

        double vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        {
            double v = d1 / (d1 - d3);
            return add(a, scale(ab, v));
        }

        std::array<double, 3> cp = sub(p, c);
        double d5 = dot(ab, cp);
        double d6 = dot(ac, cp);
        if (d6 >= 0.0 && d5 <= d6)
            return c;

        double vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        {
            double w = d2 / (d2 - d6);
            return add(a, scale(ac, w));
        }

        double va = d3 * d6 - d5 * d4;
        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        {
            double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return add(b, scale(sub(c, b), w));
        }

        double denom = 1.0 / (va + vb + vc);
        double v = vb * denom;
        double w = vc * denom;
        return add(a, add(scale(ab, v), scale(ac, w)));
    }

    static double point_to_triangle_dist_sq(const std::array<double, 3> &p,
                                             const std::array<double, 3> &a,
                                             const std::array<double, 3> &b,
                                             const std::array<double, 3> &c)
    {
        std::array<double, 3> cp = closest_point_on_triangle(p, a, b, c);
        double dx = p[0] - cp[0], dy = p[1] - cp[1], dz = p[2] - cp[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // Closest distance (squared) between two line segments, via the
    // standard clamped-parametric algorithm (Ericson, "Real-Time Collision
    // Detection", ClosestPtSegmentSegment). Needed in addition to
    // point-to-triangle above -- two triangles can have their true closest
    // approach along a pair of crossing edges, with neither triangle's
    // vertices being the closest feature.
    static double segment_segment_dist_sq(const std::array<double, 3> &p1, const std::array<double, 3> &q1,
                                           const std::array<double, 3> &p2, const std::array<double, 3> &q2)
    {
        auto sub = [](const std::array<double, 3> &x, const std::array<double, 3> &y) -> std::array<double, 3>
        { return {x[0] - y[0], x[1] - y[1], x[2] - y[2]}; };
        auto dot = [](const std::array<double, 3> &x, const std::array<double, 3> &y) -> double
        { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };

        const double EPS = 1e-12;

        std::array<double, 3> d1 = sub(q1, p1);
        std::array<double, 3> d2 = sub(q2, p2);
        std::array<double, 3> r = sub(p1, p2);

        double a = dot(d1, d1);
        double e = dot(d2, d2);
        double f = dot(d2, r);

        double s, t;

        if (a <= EPS && e <= EPS)
        {
            s = 0.0;
            t = 0.0;
        }
        else if (a <= EPS)
        {
            s = 0.0;
            t = std::clamp(f / e, 0.0, 1.0);
        }
        else
        {
            double c = dot(d1, r);
            if (e <= EPS)
            {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            }
            else
            {
                double b = dot(d1, d2);
                double denom = a * e - b * b;
                s = (denom > EPS) ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
                t = (b * s + f) / e;

                if (t < 0.0)
                {
                    t = 0.0;
                    s = std::clamp(-c / a, 0.0, 1.0);
                }
                else if (t > 1.0)
                {
                    t = 1.0;
                    s = std::clamp((b - c) / a, 0.0, 1.0);
                }
            }
        }

        std::array<double, 3> c1 = {p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s};
        std::array<double, 3> c2 = {p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t};

        double dx = c1[0] - c2[0], dy = c1[1] - c2[1], dz = c1[2] - c2[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // Closest distance (squared) between two triangles: the minimum of six
    // point-to-triangle sub-cases (each triangle's three vertices against
    // the other triangle) and nine edge-to-edge sub-cases (every edge of
    // one against every edge of the other). This combination is required
    // for correctness; point-to-triangle alone misses the case where two
    // triangles' edges cross near each other without either vertex
    // projecting onto the opposing face.
    static double triangle_triangle_dist_sq(const std::array<double, 3> &a0, const std::array<double, 3> &a1,
                                             const std::array<double, 3> &a2,
                                             const std::array<double, 3> &b0, const std::array<double, 3> &b1,
                                             const std::array<double, 3> &b2)
    {
        double best = point_to_triangle_dist_sq(a0, b0, b1, b2);
        best = std::min(best, point_to_triangle_dist_sq(a1, b0, b1, b2));
        best = std::min(best, point_to_triangle_dist_sq(a2, b0, b1, b2));
        best = std::min(best, point_to_triangle_dist_sq(b0, a0, a1, a2));
        best = std::min(best, point_to_triangle_dist_sq(b1, a0, a1, a2));
        best = std::min(best, point_to_triangle_dist_sq(b2, a0, a1, a2));

        const std::array<double, 3> a_edges[3][2] = {{a0, a1}, {a1, a2}, {a2, a0}};
        const std::array<double, 3> b_edges[3][2] = {{b0, b1}, {b1, b2}, {b2, b0}};

        for (const auto &ae : a_edges)
            for (const auto &be : b_edges)
                best = std::min(best, segment_segment_dist_sq(ae[0], ae[1], be[0], be[1]));

        return best;
    }

    // True minimum surface-to-surface distance between two element
    // geometries. Brute-force over every triangle pair by design (see
    // section header). Stops early the moment an exact-zero distance is
    // found (touching or intersecting geometry), since no smaller result
    // is possible -- a correctness-neutral short circuit, not an
    // approximation. Caller is responsible for rejecting empty-face
    // geometry before calling this (see calculate_min_distance below) --
    // this function assumes both `faces` arrays are non-empty.
    static double compute_min_distance(const ElementGeometry &a, const ElementGeometry &b)
    {
        double best_sq = std::numeric_limits<double>::infinity();

        for (const auto &fa : a.faces)
        {
            const auto &a0 = a.vertices[fa[0]];
            const auto &a1 = a.vertices[fa[1]];
            const auto &a2 = a.vertices[fa[2]];

            for (const auto &fb : b.faces)
            {
                const auto &b0 = b.vertices[fb[0]];
                const auto &b1 = b.vertices[fb[1]];
                const auto &b2 = b.vertices[fb[2]];

                double d_sq = triangle_triangle_dist_sq(a0, a1, a2, b0, b1, b2);
                if (d_sq < best_sq)
                    best_sq = d_sq;

                if (best_sq <= 0.0)
                    return 0.0;
            }
        }

        return std::sqrt(best_sq);
    }

    // ------------------------------------------------------------------
    // TEMP-DEBUG(revit-intid-qc): temporary acceptance-validation support
    // for addressing an element by its Revit ElementId (intId) instead of
    // uniqueId.
    //
    // The platform's intended canonical identity is the literal IFC GlobalId
    // (semantic.json's parameters.IfcGUID). The uniqueId that the GLB/
    // map.json currently key on is only today's existing cross-file locator
    // -- a transitional stand-in, not the final canonical identity. That is
    // known technical debt: a future exporter/data-standard task must
    // re-key the bundle around IFC GlobalId. This code does not attempt
    // that; it only adds a temporary QC-convenience path on top of what
    // exists today, to be removed once that rekey happens.
    //
    // No new public field is introduced. "elementIds" remains the ONLY
    // public element-identifier field, and its type (array of strings) is
    // unchanged. The bundle itself stays authoritative: a submitted value is
    // always tried FIRST as a uniqueId, exactly as if this support did not
    // exist. Only when that direct lookup finds nothing, AND the submitted
    // string is purely numeric, is it retried as a Revit ElementId (intId),
    // resolved via semantic.json (a bundle artifact) down to the SAME
    // uniqueId the existing GLB lookup already uses. There is no
    // format-based classification up front -- the bundle's own contents
    // decide, not a naming-pattern assumption.
    //
    // The response always echoes the caller's original submitted string
    // back under "elementId", whichever path resolved it -- the internal
    // uniqueId used for the GLB lookup is never exposed. A failed intId
    // resolution surfaces the exact same generic "element not found in
    // geometry file" message used for any other unresolved identifier, so no
    // response ever reveals which path was attempted.
    // ------------------------------------------------------------------
    static bool load_semantic_json(const std::string &path, json &out)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        try
        {
            f >> out;
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

    // Walks semantic.json's {bucket: {items: [...]}} shape directly (same
    // shape aiquery.cpp also walks, reimplemented independently here -- no
    // shared code, no call into the semantic engine) to build a one-shot
    // intId -> uniqueId lookup for this request.
    static std::unordered_map<int64_t, std::string> build_revit_id_lookup(const json &semantic)
    {
        std::unordered_map<int64_t, std::string> lookup;
        if (!semantic.is_object())
            return lookup;

        for (const auto &bucket_entry : semantic.items())
        {
            const json &bucket = bucket_entry.value();
            if (!bucket.is_object() || !bucket.contains("items") || !bucket["items"].is_array())
                continue;

            for (const auto &item : bucket["items"])
            {
                if (!item.is_object() || !item.contains("intId") || !item.contains("uniqueId"))
                    continue;
                if (!item["intId"].is_number_integer() || !item["uniqueId"].is_string())
                    continue;

                lookup[item["intId"].get<int64_t>()] = item["uniqueId"].get<std::string>();
            }
        }

        return lookup;
    }

    // Temporary acceptance-validation support (see block comment above
    // load_semantic_json): true only if `s` is non-empty and every
    // character is a decimal digit -- a shape no real uniqueId can ever
    // have (Revit's UniqueId format always contains hyphens). Used only
    // as a fallback trigger after a direct uniqueId lookup has already
    // failed; the bundle itself, not this check, decides identity.
    static bool is_all_digits(const std::string &s)
    {
        if (s.empty())
            return false;

        for (char c : s)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;

        return true;
    }

    // Local, safe string -> int64_t parse. Mirrors the small helper
    // aiquery.cpp defines independently for the same purpose -- no shared
    // code between plugins, consistent with this file's existing
    // convention (see resolve_glb_path/model_base_name above).
    static bool parse_int64_local(const std::string &s, int64_t &out)
    {
        try
        {
            size_t pos = 0;
            long long v = std::stoll(s, &pos);
            if (pos != s.size())
                return false;

            out = static_cast<int64_t>(v);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Element identifier resolution -- the ONE path every geometry action
    // must go through (Revit intId -> semantic.json -> uniqueId -> map.json
    // -> GLB). Callers never deal with uniqueIds directly; this is not an
    // add-on specific to get_geometry, it belongs to the geometry engine's
    // interface as a whole. Do not re-implement this per action -- every
    // action that accepts element identifiers calls
    // resolve_and_extract_element below, sharing one RevitIdResolver per
    // request so semantic.json is read at most once even across many
    // elements/actions in a single call.
    // ------------------------------------------------------------------
    struct RevitIdResolver
    {
        std::string model;
        std::unordered_map<int64_t, std::string> lookup;
        bool loaded = false;

        // Resolves a submitted identifier via the Revit intId path only --
        // callers try the identifier directly as a uniqueId first (see
        // resolve_and_extract_element). Returns an empty string if `s`
        // isn't numeric or has no match; semantic.json is loaded lazily,
        // once, on first use.
        std::string resolve(const std::string &s)
        {
            int64_t revit_id;
            if (!is_all_digits(s) || !parse_int64_local(s, revit_id))
                return "";

            if (!loaded)
            {
                json semantic;
                if (load_semantic_json(resolve_semantic_json_path(model), semantic))
                    lookup = build_revit_id_lookup(semantic);
                loaded = true;
            }

            auto it = lookup.find(revit_id);
            return it != lookup.end() ? it->second : "";
        }
    };

    // The single element-lookup entry point every geometry action calls.
    // Bundle-authoritative order, unchanged from before the intId fallback
    // existed: the submitted identifier is always tried directly as a
    // uniqueId first; only on failure is it retried via the Revit intId
    // path. Returns geom.found == false if neither resolves.
    // `out_resolved_id`, when supplied, receives the identifier the GLB
    // lookup actually succeeded with -- the submitted id itself, unless
    // resolution went through the intId path, in which case it's the
    // resolved uniqueId. Needed by callers (build_element_response) that
    // must key a second, separate GLB lookup (find_mesh_nodes_for_element)
    // off the identifier that actually matched, not the caller's original
    // string -- the response still always echoes the caller's original
    // submitted value, never the internal uniqueId.
    static ElementGeometry resolve_and_extract_element(const GlbFile &glb,
                                                        const std::unordered_map<int, Mat4> &world_transforms,
                                                        RevitIdResolver &resolver, const std::string &submitted_id,
                                                        std::string *out_resolved_id = nullptr)
    {
        if (out_resolved_id)
            *out_resolved_id = submitted_id;

        ElementGeometry geom = extract_element_geometry(glb, world_transforms, submitted_id);
        if (geom.found)
            return geom;

        std::string resolved_id = resolver.resolve(submitted_id);
        if (resolved_id.empty())
            return geom;

        if (out_resolved_id)
            *out_resolved_id = resolved_id;

        return extract_element_geometry(glb, world_transforms, resolved_id);
    }

    // ------------------------------------------------------------------
    // Request handling.
    // ------------------------------------------------------------------
    static const std::unordered_set<std::string> &valid_primitives()
    {
        static const std::unordered_set<std::string> names = {
            "vertices", "faces", "meshEdges", "surfaceCentroid",
            "lowestPoint", "highestPoint", "extentsCenter"};
        return names;
    }

    static json build_element_response(const GlbFile &glb, const std::unordered_map<int, Mat4> &world_transforms,
                                        RevitIdResolver &resolver, const std::string &element_id,
                                        const std::vector<std::string> &primitives)
    {
        json out;
        out["elementId"] = element_id;

        std::string resolved_id;
        ElementGeometry geom = resolve_and_extract_element(glb, world_transforms, resolver, element_id, &resolved_id);

        if (!geom.found)
        {
            out["error"] = "element not found in geometry file";
            return out;
        }

        out["meshNodeCount"] = static_cast<int>(find_mesh_nodes_for_element(glb.gltf, resolved_id).size());

        for (const auto &prim : primitives)
        {
            if (prim == "vertices")
            {
                out["vertices"] = compute_vertices(geom);
            }
            else if (prim == "faces")
            {
                out["faces"] = compute_faces(geom);
            }
            else if (prim == "meshEdges")
            {
                out["meshEdges"] = compute_mesh_edges(geom);
            }
            else if (prim == "surfaceCentroid")
            {
                std::array<double, 3> c;
                out["surfaceCentroid"] = compute_surface_centroid(geom, c) ? point_to_json(c) : json(nullptr);
            }
            else if (prim == "extentsCenter")
            {
                std::array<double, 3> mn, mx;
                if (compute_extents(geom, mn, mx))
                    out["extentsCenter"] = point_to_json(
                        {(mn[0] + mx[0]) / 2.0, (mn[1] + mx[1]) / 2.0, (mn[2] + mx[2]) / 2.0});
                else
                    out["extentsCenter"] = nullptr;
            }
            else if (prim == "lowestPoint")
            {
                std::array<double, 3> p;
                out["lowestPoint"] = compute_lowest_point(geom, p) ? point_to_json(p) : json(nullptr);
            }
            else if (prim == "highestPoint")
            {
                std::array<double, 3> p;
                out["highestPoint"] = compute_highest_point(geom, p) ? point_to_json(p) : json(nullptr);
            }
        }

        return out;
    }

    const char *plugin_name()
    {
        return "IfcGeomServer Plugin";
    }

    const char *handle_ifc_geom(const std::string &input_json)
    {
        static std::string response;

        try
        {
            json req = json::parse(input_json);
            std::string action = req.value("action", "");

            // Layer 2: point-to-point distance. Pure arithmetic on two
            // explicitly supplied points -- no model, no elementIds, no
            // bundle access at all.
            if (action == "calculate_distance")
            {
                if (!req.contains("pointA") || !req.contains("pointB") ||
                    !req["pointA"].is_object() || !req["pointB"].is_object())
                {
                    response = json({{"error", "missing pointA or pointB"}}).dump();
                    return response.c_str();
                }

                auto read_point = [](const json &p, std::array<double, 3> &out) -> bool
                {
                    if (!p.contains("x") || !p.contains("y") || !p.contains("z"))
                        return false;
                    if (!p["x"].is_number() || !p["y"].is_number() || !p["z"].is_number())
                        return false;
                    out = {p["x"].get<double>(), p["y"].get<double>(), p["z"].get<double>()};
                    return true;
                };

                std::array<double, 3> point_a, point_b;
                if (!read_point(req["pointA"], point_a) || !read_point(req["pointB"], point_b))
                {
                    response = json({{"error", "pointA and pointB must each have numeric x, y, z"}}).dump();
                    return response.c_str();
                }

                double dx = point_b[0] - point_a[0];
                double dy = point_b[1] - point_a[1];
                double dz = point_b[2] - point_a[2];

                response = json({{"distance", std::sqrt(dx * dx + dy * dy + dz * dz)}}).dump();
                return response.c_str();
            }

            // Layer 2: signed difference between two explicitly supplied
            // points along one caller-specified global axis. A point-to-point
            // operation, not geometry-to-geometry -- fundamentally different
            // from calculate_distance above (unconstrained 3D distance), the
            // same way axis-constrained separation was proven irreducible
            // from minimum distance during Layer 3 scoping (see project
            // memory). No geometry is read and no points are searched for --
            // both points and the axis must be supplied explicitly.
            if (action == "calculate_axis_difference")
            {
                if (!req.contains("pointA") || !req.contains("pointB") ||
                    !req["pointA"].is_object() || !req["pointB"].is_object())
                {
                    response = json({{"error", "missing pointA or pointB"}}).dump();
                    return response.c_str();
                }

                auto read_point = [](const json &p, std::array<double, 3> &out) -> bool
                {
                    if (!p.contains("x") || !p.contains("y") || !p.contains("z"))
                        return false;
                    if (!p["x"].is_number() || !p["y"].is_number() || !p["z"].is_number())
                        return false;
                    out = {p["x"].get<double>(), p["y"].get<double>(), p["z"].get<double>()};
                    return true;
                };

                std::array<double, 3> point_a, point_b;
                if (!read_point(req["pointA"], point_a) || !read_point(req["pointB"], point_b))
                {
                    response = json({{"error", "pointA and pointB must each have numeric x, y, z"}}).dump();
                    return response.c_str();
                }

                std::string axis = req.value("axis", "");
                std::transform(axis.begin(), axis.end(), axis.begin(),
                                [](unsigned char c)
                                { return std::tolower(c); });

                int axis_index;
                if (axis == "x")
                    axis_index = 0;
                else if (axis == "y")
                    axis_index = 1;
                else if (axis == "z")
                    axis_index = 2;
                else
                {
                    response = json({{"error", "axis must be one of \"X\", \"Y\", \"Z\""}}).dump();
                    return response.c_str();
                }

                response = json({{"difference", point_b[axis_index] - point_a[axis_index]}}).dump();
                return response.c_str();
            }

            // Layer 2: surface area of one or more explicitly supplied
            // elements. Shares elementIds/model input handling and the
            // exact same GLB loading and per-element geometry extraction
            // Layer 1's get_geometry uses below, unmodified. (Volume was
            // investigated and deliberately removed -- see block comment
            // above compute_surface_area.)
            if (action == "calculate_surface_area")
            {
                std::string calc_model = req.value("model", "");
                if (calc_model.empty())
                {
                    response = json({{"error", "missing model"}}).dump();
                    return response.c_str();
                }

                std::vector<std::string> calc_element_ids;
                if (req.contains("elementIds") && req["elementIds"].is_array())
                    for (const auto &id : req["elementIds"])
                        if (id.is_string())
                            calc_element_ids.push_back(id.get<std::string>());

                if (calc_element_ids.empty())
                {
                    response = json({{"error", "missing elementIds"}}).dump();
                    return response.c_str();
                }

                std::string calc_glb_path = resolve_glb_path(calc_model);
                GlbFile calc_glb;
                if (!load_glb(calc_glb_path, calc_glb))
                {
                    response = json({{"error", "geometry file not found or invalid"}, {"path", calc_glb_path}}).dump();
                    return response.c_str();
                }

                std::unordered_map<int, Mat4> calc_world_transforms = compute_world_transforms(calc_glb.gltf);
                RevitIdResolver calc_resolver{calc_model};

                json calc_results = json::array();
                for (const auto &element_id : calc_element_ids)
                {
                    json entry;
                    entry["elementId"] = element_id;

                    ElementGeometry geom = resolve_and_extract_element(calc_glb, calc_world_transforms, calc_resolver, element_id);
                    if (!geom.found)
                    {
                        entry["error"] = "element not found in geometry file";
                        calc_results.push_back(entry);
                        continue;
                    }

                    entry["surfaceArea"] = compute_surface_area(geom);
                    calc_results.push_back(entry);
                }

                response = json({{"results", calc_results}}).dump();
                return response.c_str();
            }

            // Layer 3: minimum distance between two explicitly supplied
            // full geometries (see project memory: the one capability that
            // survived scoping). Unlike Layer 2's calculate_surface_area,
            // this takes exactly two elements, not an array -- it is
            // inherently pairwise.
            if (action == "calculate_min_distance")
            {
                std::string mind_model = req.value("model", "");
                if (mind_model.empty())
                {
                    response = json({{"error", "missing model"}}).dump();
                    return response.c_str();
                }

                std::string element_id_a = req.value("elementIdA", "");
                std::string element_id_b = req.value("elementIdB", "");
                if (element_id_a.empty() || element_id_b.empty())
                {
                    response = json({{"error", "missing elementIdA or elementIdB"}}).dump();
                    return response.c_str();
                }

                std::string mind_glb_path = resolve_glb_path(mind_model);
                GlbFile mind_glb;
                if (!load_glb(mind_glb_path, mind_glb))
                {
                    response = json({{"error", "geometry file not found or invalid"}, {"path", mind_glb_path}}).dump();
                    return response.c_str();
                }

                std::unordered_map<int, Mat4> mind_world_transforms = compute_world_transforms(mind_glb.gltf);
                RevitIdResolver mind_resolver{mind_model};

                ElementGeometry geom_a = resolve_and_extract_element(mind_glb, mind_world_transforms, mind_resolver, element_id_a);
                if (!geom_a.found)
                {
                    response = json({{"error", "element not found in geometry file"}, {"elementId", element_id_a}}).dump();
                    return response.c_str();
                }

                ElementGeometry geom_b = resolve_and_extract_element(mind_glb, mind_world_transforms, mind_resolver, element_id_b);
                if (!geom_b.found)
                {
                    response = json({{"error", "element not found in geometry file"}, {"elementId", element_id_b}}).dump();
                    return response.c_str();
                }

                if (geom_a.faces.empty() || geom_b.faces.empty())
                {
                    response = json({{"error", "no surface geometry for element"},
                                     {"elementIdA", element_id_a},
                                     {"elementIdB", element_id_b}})
                                   .dump();
                    return response.c_str();
                }

                response = json({{"minDistance", compute_min_distance(geom_a, geom_b)}}).dump();
                return response.c_str();
            }

            if (action != "get_geometry")
            {
                response = json({{"plugin", "IfcGeomServer"}, {"status", "idle"}}).dump();
                return response.c_str();
            }

            std::string model = req.value("model", "");
            if (model.empty())
            {
                response = json({{"error", "missing model"}}).dump();
                return response.c_str();
            }

            std::vector<std::string> element_ids;
            if (req.contains("elementIds") && req["elementIds"].is_array())
                for (const auto &id : req["elementIds"])
                    if (id.is_string())
                        element_ids.push_back(id.get<std::string>());

            if (element_ids.empty())
            {
                response = json({{"error", "missing elementIds"}}).dump();
                return response.c_str();
            }

            std::vector<std::string> primitives;
            if (req.contains("primitives") && req["primitives"].is_array())
                for (const auto &p : req["primitives"])
                    if (p.is_string())
                        primitives.push_back(p.get<std::string>());

            if (primitives.empty())
            {
                response = json({{"error", "missing primitives"}}).dump();
                return response.c_str();
            }

            for (const auto &p : primitives)
            {
                if (!valid_primitives().count(p))
                {
                    response = json({{"error", "unknown primitive: \"" + p + "\""}}).dump();
                    return response.c_str();
                }
            }

            std::string glb_path = resolve_glb_path(model);

            GlbFile glb;
            if (!load_glb(glb_path, glb))
            {
                response = json({{"error", "geometry file not found or invalid"}, {"path", glb_path}}).dump();
                return response.c_str();
            }

            std::unordered_map<int, Mat4> world_transforms = compute_world_transforms(glb.gltf);

            // One resolver shared across every element in this request --
            // semantic.json is read at most once, lazily, only if some
            // element actually needs the intId fallback (see
            // resolve_and_extract_element).
            RevitIdResolver resolver{model};

            json results = json::array();
            for (const auto &element_id : element_ids)
                results.push_back(build_element_response(glb, world_transforms, resolver, element_id, primitives));

            response = json({{"results", results}}).dump();

            return response.c_str();
        }
        catch (...)
        {
            response = R"({"error":"invalid input or runtime error"})";
            return response.c_str();
        }
    }
}
