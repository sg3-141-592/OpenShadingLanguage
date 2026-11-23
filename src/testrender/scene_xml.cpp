// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// XML scene parsing is intentionally kept out of the reusable libtestrender
// library so that embedding applications do not inherit a pugixml dependency.

#include <pugixml.hpp>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "raytracer.h"
#include "shading.h"
#include "simpleraytracer.h"

#ifdef USING_OIIO_PUGI
namespace pugi = OIIO::pugi;
#endif

using namespace OSL;

OSL_NAMESPACE_BEGIN

inline Vec3
strtovec(string_view str)
{
    Vec3 v(0, 0, 0);
    OIIO::Strutil::parse_float(str, v[0]);
    OIIO::Strutil::parse_char(str, ',');
    OIIO::Strutil::parse_float(str, v[1]);
    OIIO::Strutil::parse_char(str, ',');
    OIIO::Strutil::parse_float(str, v[2]);
    return v;
}

inline bool
strtobool(const char* str)
{
    return strcmp(str, "1") == 0 || strcmp(str, "on") == 0
           || strcmp(str, "yes") == 0;
}

template<int N> struct ParamStorage {
    ParamStorage() : fparamindex(0), iparamindex(0), sparamindex(0) {}

    void* Int(int i)
    {
        OSL_DASSERT(iparamindex < N);
        iparamdata[iparamindex] = i;
        iparamindex++;
        return &iparamdata[iparamindex - 1];
    }

    void* Float(float f)
    {
        OSL_DASSERT(fparamindex < N);
        fparamdata[fparamindex] = f;
        fparamindex++;
        return &fparamdata[fparamindex - 1];
    }

    void* Vec(float x, float y, float z)
    {
        Float(x);
        Float(y);
        Float(z);
        return &fparamdata[fparamindex - 3];
    }

    void* Vec(const float* xyz)
    {
        Float(xyz[0]);
        Float(xyz[1]);
        Float(xyz[2]);
        return &fparamdata[fparamindex - 3];
    }

    void* Str(const char* str)
    {
        OSL_DASSERT(sparamindex < N);
        sparamdata[sparamindex] = ustring(str);
        sparamindex++;
        return &sparamdata[sparamindex - 1];
    }

private:
    // storage for shader parameters
    float fparamdata[N];
    int iparamdata[N];
    ustring sparamdata[N];

    int fparamindex;
    int iparamindex;
    int sparamindex;
};



inline bool
parse_prefix_and_floats(string_view str, string_view prefix, int nvals,
                        float* vals)
{
    bool ok = OIIO::Strutil::parse_prefix(str, prefix);
    for (int i = 0; i < nvals && ok; ++i)
        ok &= OIIO::Strutil::parse_float(str, vals[i]);
    return ok;
}


inline bool
parse_prefix_and_ints(string_view str, string_view prefix, int nvals, int* vals)
{
    bool ok = OIIO::Strutil::parse_prefix(str, prefix);
    for (int i = 0; i < nvals && ok; ++i)
        ok &= OIIO::Strutil::parse_int(str, vals[i]);
    return ok;
}



void
SimpleRaytracer::parse_scene_xml(const std::string& scenefile)
{
    ShaderMap shadermap;
    pugi::xml_document doc;
    pugi::xml_parse_result parse_result;
    if (OIIO::Strutil::ends_with(scenefile, ".xml")
        && OIIO::Filesystem::exists(scenefile)) {
        parse_result = doc.load_file(scenefile.c_str());
    } else {
        parse_result = doc.load_buffer(scenefile.c_str(), scenefile.size());
    }
    if (!parse_result)
        errhandler().severefmt("XML parsed with errors: {} at offset {}",
                               parse_result.description(), parse_result.offset);
    pugi::xml_node root = doc.child("World");
    if (!root)
        errhandler().severefmt(
            "Error reading scene: Root element <World> is missing");

    // loop over all children of world
    for (auto node = root.first_child(); node; node = node.next_sibling()) {
        if (strcmp(node.name(), "Option") == 0) {
            for (auto attr = node.first_attribute(); attr;
                 attr      = attr.next_attribute()) {
                int i = 0;
                if (parse_prefix_and_ints(attr.value(), "int ", 1, &i))
                    attribute(attr.name(), i);
                // TODO: pass any extra options to shading system (or texture system?)
            }
        } else if (strcmp(node.name(), "Camera") == 0) {
            // defaults
            Vec3 eye(0, 0, 0);
            Vec3 dir(0, 0, -1);
            Vec3 up(0, 1, 0);
            float fov = 90.f;

            // load camera (only first attribute counts if duplicates)
            pugi::xml_attribute eye_attr = node.attribute("eye");
            pugi::xml_attribute dir_attr = node.attribute("dir");
            pugi::xml_attribute at_attr  = node.attribute("look_at");
            pugi::xml_attribute up_attr  = node.attribute("up");
            pugi::xml_attribute fov_attr = node.attribute("fov");

            if (eye_attr)
                eye = strtovec(eye_attr.value());
            if (dir_attr)
                dir = strtovec(dir_attr.value());
            else if (at_attr)
                dir = strtovec(at_attr.value()) - eye;
            if (up_attr)
                up = strtovec(up_attr.value());
            if (fov_attr)
                fov = OIIO::Strutil::from_string<float>(fov_attr.value());

            camera.lookat(eye, dir, up, fov);
        } else if (strcmp(node.name(), "Sphere") == 0) {
            // load sphere
            pugi::xml_attribute center_attr = node.attribute("center");
            pugi::xml_attribute radius_attr = node.attribute("radius");

            if (center_attr && radius_attr) {
                Vec3 center  = strtovec(center_attr.value());
                float radius = OIIO::Strutil::from_string<float>(
                    radius_attr.value());
                if (radius > 0) {
                    pugi::xml_attribute resolution_attr = node.attribute(
                        "resolution");
                    int resolution = 64;
                    if (resolution_attr) {
                        OIIO::string_view str = resolution_attr.value();
                        OIIO::Strutil::parse_int(str, resolution);
                    }
                    scene.add_sphere(center, radius, int(shaders().size()) - 1,
                                     resolution);
                }
            }
        } else if (strcmp(node.name(), "Quad") == 0) {
            // load quad
            pugi::xml_attribute corner_attr = node.attribute("corner");
            pugi::xml_attribute edge_x_attr = node.attribute("edge_x");
            pugi::xml_attribute edge_y_attr = node.attribute("edge_y");
            if (corner_attr && edge_x_attr && edge_y_attr) {
                Vec3 co = strtovec(corner_attr.value());
                Vec3 ex = strtovec(edge_x_attr.value());
                Vec3 ey = strtovec(edge_y_attr.value());

                int resolution                      = 1;
                pugi::xml_attribute resolution_attr = node.attribute(
                    "resolution");
                if (resolution_attr) {
                    OIIO::string_view str = resolution_attr.value();
                    OIIO::Strutil::parse_int(str, resolution);
                }

                scene.add_quad(co, ex, ey, int(shaders().size()) - 1,
                               resolution);
            }
        } else if (strcmp(node.name(), "Model") == 0) {
            // load .obj model
            pugi::xml_attribute filename_attr = node.attribute("filename");
            if (filename_attr) {
                std::string filename = filename_attr.value();
                std::vector<std::string> searchpath;
                searchpath.emplace_back(
                    OIIO::Filesystem::parent_path(scenefile));
                std::string actual_filename
                    = OIIO::Filesystem::searchpath_find(filename, searchpath,
                                                        false);
                if (actual_filename.empty()) {
                    errhandler().errorfmt("Unable to find model file {}",
                                          filename);
                } else {
                    // we got a valid filename, try to load the model
                    scene.add_model(actual_filename, shadermap,
                                    int(shaders().size() - 1), errhandler());
                }
            }
        } else if (strcmp(node.name(), "Background") == 0) {
            pugi::xml_attribute res_attr = node.attribute("resolution");
            if (res_attr)
                backgroundResolution = OIIO::Strutil::from_string<int>(
                    res_attr.value());
            backgroundShaderID = int(shaders().size()) - 1;
        } else if (strcmp(node.name(), "ShaderGroup") == 0) {
            ShaderGroupRef group;
            pugi::xml_attribute is_light_attr = node.attribute("is_light");
            pugi::xml_attribute name_attr     = node.attribute("name");
            std::string name = name_attr ? name_attr.value() : "group";
            pugi::xml_attribute type_attr = node.attribute("type");
            std::string shadertype = type_attr ? type_attr.value() : "surface";
            pugi::xml_attribute commands_attr = node.attribute("commands");
            std::string commands = commands_attr ? commands_attr.value()
                                                 : node.text().get();
            if (commands.size())
                group = shadingsys->ShaderGroupBegin(name, shadertype,
                                                     commands);
            else
                group = shadingsys->ShaderGroupBegin(name);
            ParamStorage<1024>
                store;  // scratch space to hold parameters until they are read by Shader()
            for (pugi::xml_node gnode = node.first_child(); gnode;
                 gnode                = gnode.next_sibling()) {
                if (strcmp(gnode.name(), "Parameter") == 0) {
                    // handle parameters
                    for (auto attr = gnode.first_attribute(); attr;
                         attr      = attr.next_attribute()) {
                        int i = 0;
                        float f[3];
                        string_view val(attr.value());
                        if (parse_prefix_and_ints(val, "int ", 1, &i))
                            shadingsys->Parameter(*group, attr.name(), TypeInt,
                                                  store.Int(i));
                        else if (parse_prefix_and_floats(val, "float ", 1, f))
                            shadingsys->Parameter(*group, attr.name(),
                                                  TypeFloat, store.Float(f[0]));
                        else if (parse_prefix_and_floats(val, "vector ", 3, f))
                            shadingsys->Parameter(*group, attr.name(),
                                                  TypeVector, store.Vec(f));
                        else if (parse_prefix_and_floats(val, "point ", 3, f))
                            shadingsys->Parameter(*group, attr.name(),
                                                  TypePoint, store.Vec(f));
                        else if (parse_prefix_and_floats(val, "color ", 3, f))
                            shadingsys->Parameter(*group, attr.name(),
                                                  TypeColor, store.Vec(f));
                        else
                            shadingsys->Parameter(*group, attr.name(),
                                                  TypeString,
                                                  store.Str(attr.value()));
                    }
                } else if (strcmp(gnode.name(), "Shader") == 0) {
                    pugi::xml_attribute type_attr  = gnode.attribute("type");
                    pugi::xml_attribute name_attr  = gnode.attribute("name");
                    pugi::xml_attribute layer_attr = gnode.attribute("layer");
                    const char* type = type_attr ? type_attr.value()
                                                 : "surface";
                    if (name_attr && layer_attr)
                        shadingsys->Shader(*group, type, name_attr.value(),
                                           layer_attr.value());
                } else if (strcmp(gnode.name(), "ConnectShaders") == 0) {
                    // FIXME: find a more elegant way to encode this
                    pugi::xml_attribute sl = gnode.attribute("srclayer");
                    pugi::xml_attribute sp = gnode.attribute("srcparam");
                    pugi::xml_attribute dl = gnode.attribute("dstlayer");
                    pugi::xml_attribute dp = gnode.attribute("dstparam");
                    if (sl && sp && dl && dp)
                        shadingsys->ConnectShaders(*group, sl.value(),
                                                   sp.value(), dl.value(),
                                                   dp.value());
                } else {
                    // unknown element?
                }
            }
            shadingsys->ShaderGroupEnd(*group);
            if (name_attr) {
                if (auto it = shadermap.find(name); it != shadermap.end()) {
                    int shaderID = it->second;
                    if (shaderID >= 0 && shaderID < int(shaders().size())) {
                        fprintf(stderr, "Updating shader %d - %s\n", shaderID,
                                shadertype.c_str());
                        // we already have a material under this name,
                        Material& m = shaders()[shaderID];
                        // TODO: could we query the shadertype directly from the ShaderGroup?
                        if (shadertype == "displacement")
                            m.disp = group;
                        else if (shadertype == "surface")
                            m.surf = group;
                        // skip the rest which would add a new material
                        continue;
                    }
                } else {
                    shadermap.emplace(name, int(shaders().size()));
                }
            }
            shaders().emplace_back(Material { group, nullptr });
            m_shader_is_light.emplace_back(
                is_light_attr ? strtobool(is_light_attr.value()) : false);
        } else {
            // unknown element?
        }
    }
    if (root.next_sibling())
        errhandler().severefmt(
            "Error reading {}: Found multiple top-level elements", scenefile);
    if (shaders().empty())
        errhandler().severefmt("No shaders in scene");
    if (scene.num_prims() == 0)
        errhandler().severefmt("No primitives in scene");
    camera.finalize();
}

OSL_NAMESPACE_END
