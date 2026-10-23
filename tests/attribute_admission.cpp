#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAttributes.h>

// one row per AttributeAdmission arm, in each medium, allowed and not. the arm order of
// AST::attribute_admission is the content: echoc:: first, then a tool name, then the medium's
// own closed set, then the other medium's set, otherwise unknown. a bare compiler stem is
// always t_unknown, even when the module may write the echoc:: form

TEST_CASE("attribute_admission classifies each spelling by medium and gate", "[attributes]")
{
    using AST::AttributeAdmission;
    using AST::AttributeMedium;

    struct Row
    {
        const char *name;
        AttributeMedium medium;
        bool allowed;
        AttributeAdmission expected;
    };

    const Row rows[] = {
        { "export",          AttributeMedium::t_source,   false, AttributeAdmission::t_language },
        { "inline",          AttributeMedium::t_source,   false, AttributeAdmission::t_language },
        { "wasm",            AttributeMedium::t_source,   false, AttributeAdmission::t_language },
        { "echoc::builtin",  AttributeMedium::t_source,   false, AttributeAdmission::t_reserved_namespace },
        { "echoc::core",     AttributeMedium::t_source,   false, AttributeAdmission::t_reserved_namespace },
        { "echoc::bogus",    AttributeMedium::t_source,   false, AttributeAdmission::t_reserved_namespace },
        { "web::js_name",    AttributeMedium::t_source,   false, AttributeAdmission::t_tool },
        { "depends",         AttributeMedium::t_source,   false, AttributeAdmission::t_manifest_in_source },
        { "module",          AttributeMedium::t_source,   false, AttributeAdmission::t_manifest_in_source },
        { "builtin",         AttributeMedium::t_source,   false, AttributeAdmission::t_unknown },
        { "core",            AttributeMedium::t_source,   false, AttributeAdmission::t_unknown },
        { "nope",            AttributeMedium::t_source,   false, AttributeAdmission::t_unknown },

        { "echoc::builtin",  AttributeMedium::t_source,   true,  AttributeAdmission::t_compiler },
        { "echoc::core",     AttributeMedium::t_source,   true,  AttributeAdmission::t_compiler },
        { "echoc::intrinsic",AttributeMedium::t_source,   true,  AttributeAdmission::t_compiler },
        { "echoc::cold",     AttributeMedium::t_source,   true,  AttributeAdmission::t_compiler },
        { "echoc::cold",     AttributeMedium::t_source,   false, AttributeAdmission::t_reserved_namespace },
        { "echoc::bogus",    AttributeMedium::t_source,   true,  AttributeAdmission::t_unknown_compiler },
        { "export",          AttributeMedium::t_source,   true,  AttributeAdmission::t_language },
        { "web::js_name",    AttributeMedium::t_source,   true,  AttributeAdmission::t_tool },
        { "depends",         AttributeMedium::t_source,   true,  AttributeAdmission::t_manifest_in_source },
        { "builtin",         AttributeMedium::t_source,   true,  AttributeAdmission::t_unknown },

        { "depends",         AttributeMedium::t_manifest, false, AttributeAdmission::t_language },
        { "module",          AttributeMedium::t_manifest, false, AttributeAdmission::t_language },
        { "web::js_name",    AttributeMedium::t_manifest, false, AttributeAdmission::t_tool },
        { "echoc::builtin",  AttributeMedium::t_manifest, false, AttributeAdmission::t_reserved_namespace },
        { "echoc::bogus",    AttributeMedium::t_manifest, false, AttributeAdmission::t_reserved_namespace },
        { "inline",          AttributeMedium::t_manifest, false, AttributeAdmission::t_unknown },
        { "export",          AttributeMedium::t_manifest, false, AttributeAdmission::t_unknown },
        { "builtin",         AttributeMedium::t_manifest, false, AttributeAdmission::t_unknown },
        { "nope",            AttributeMedium::t_manifest, false, AttributeAdmission::t_unknown },

        { "echoc::core",     AttributeMedium::t_manifest, true,  AttributeAdmission::t_compiler },
        { "depends",         AttributeMedium::t_manifest, true,  AttributeAdmission::t_language },
        { "inline",          AttributeMedium::t_manifest, true,  AttributeAdmission::t_unknown },
    };

    for (const Row &row : rows) {
        INFO(row.name);
        REQUIRE(AST::attribute_admission(row.name, row.medium, row.allowed) == row.expected);
    }
}

TEST_CASE("known_attribute_list is the public declaration names only", "[attributes]")
{
    const std::string listed = AST::known_attribute_list();

    REQUIRE(listed.find("inline") != std::string::npos);
    REQUIRE(listed.find("export") != std::string::npos);
    REQUIRE(listed.find("core") == std::string::npos);
    REQUIRE(listed.find("builtin") == std::string::npos);
    REQUIRE(listed.find("intrinsic") == std::string::npos);
    REQUIRE(listed.find("depends") == std::string::npos);

    REQUIRE(AST::compiler_attribute_list() == "echoc::core, echoc::builtin, echoc::intrinsic, echoc::cold");
}
