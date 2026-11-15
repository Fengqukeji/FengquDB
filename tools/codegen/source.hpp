#pragma once
#include "model.hpp"
#include <filesystem>
namespace codegen {
struct SourceField {
    std::string member, native_type, getter, setter;
};
struct SourceEntity {
    std::string language, name, qualified, module, include_path;
    bool keyword_constructor = false;
    std::map<std::string, SourceField> fields;
};
struct SourceGroup {
    Model model;
    std::map<std::string, SourceEntity> entities;
};
struct SourceInput {
    std::string output_root;
    std::vector<SourceGroup> groups;
    Json registry;
    Json schema;
};
SourceInput read_sources(const std::filesystem::path &root, const std::filesystem::path &output,
                         const std::filesystem::path &registry, const std::string &language);
void emit_source(const SourceInput &, Files &);
void source_dynamic(const SourceGroup &, Files &, const std::string &);
void source_managed(const SourceGroup &, Files &, const std::string &);
void source_systems(const SourceGroup &, Files &, const std::string &);
void source_native(const SourceGroup &, Files &, const std::string &);
} // namespace codegen
