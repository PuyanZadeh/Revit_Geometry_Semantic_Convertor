#include "ifc_geom.h"
#include <string>

extern "C" {

const char* plugin_name() {
    return "IfcGeomServer Plugin";
}

const char* handle_ifc_geom(const std::string& input_json) {
    static std::string response;
    response = R"({"plugin":"IfcGeomServer","status":"ok","message":"dummy geom plugin active"})";
    return response.c_str();
}

}
