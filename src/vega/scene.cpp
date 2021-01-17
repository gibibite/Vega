#include "scene.hpp"

#include "utils/cast.hpp"

#include <atomic>
#include <ranges>

namespace {

struct ValueToJson final {
    void operator()(int i) { j[key] = i; }
    void operator()(float f) { j[key] = f; }
    void operator()(const std::string& s) { j[key] = s; }

    json&              j;
    const std::string& key;
};

} // namespace

static void to_json(json& json, const Float3& vec)
{
    json = { vec.x, vec.y, vec.z };
}

static void to_json(json& json, const UniqueNode& node)
{
    json = node->ToJson();
}

static void to_json(json& json, const UniqueMaterialInstance& material_instance)
{
    json = material_instance->ToJson();
}

static void to_json(json& json, const AABB& aabb)
{
    json["aabb.min"] = aabb.min;
    json["aabb.max"] = aabb.max;
}

static void to_json(json& json, ID id)
{
    json = id.value;
}

static void to_json(json& json, const Material& material)
{
    json = material.ToJson();
}

static void to_json(json& json, MeshNodePtr mesh_node)
{
    json = mesh_node->GetID();
}

static void to_json(json& json, const Mesh& mesh)
{
    json = mesh.ToJson();
}

static void to_json(json& json, const MeshVertices& vertices)
{
    json["vertex.attributes"] = vertices.GetVertexAttributes();
    json["vertex.size"]       = vertices.GetVertexSize();
    json["vertex.count"]      = vertices.GetCount();
    json["vertices.size"]     = vertices.GetSize();
}

static void to_json(json& json, const MeshIndices& indices)
{
    json["index.class"]  = indices.GetIndexType();
    json["index.size"]   = indices.GetIndexSize();
    json["index.count"]  = indices.GetCount();
    json["indices.size"] = indices.GetSize();
}

struct DictionaryValues final {
    const Dictionary* dictionary = nullptr;
};

static void to_json(json& json, const DictionaryValues& values)
{
    for (const auto& [key, value] : *values.dictionary) {
        std::visit(ValueToJson{ json, key }, value);
    }
}

struct RotateValues final {
    const Float3 axis;
    const float  angle;
};

static void to_json(json& json, const RotateValues& values)
{
    json["rotate.axis"]  = values.axis;
    json["rotate.angle"] = values.angle;
}

struct TranslateValues final {
    const Float3 amount;
};

static void to_json(json& json, const TranslateValues& values)
{
    json["translate"] = values.amount;
}

struct ScaleValues final {
    const float factor;
};

static void to_json(json& json, const ScaleValues& values)
{
    json["scale"] = values.factor;
}

struct MeshNodeRefs final {
    const ID material_instance;
    const ID mesh;
};

static void to_json(json& json, const MeshNodeRefs& values)
{
    json["material.instance"] = values.material_instance;
    json["mesh"]              = values.mesh;
}

struct MeshValues final {
    const AABB*         aabb;
    const MeshVertices* vertices;
    const MeshIndices*  indices;
};

static void to_json(json& json, const MeshValues& values)
{
    json["aabb"]     = *values.aabb;
    json["vertices"] = *values.vertices;
    json["indices"]  = *values.indices;
}

static ID GetUniqueID() noexcept
{
    static std::atomic_int id = 0;
    id++;
    return ID(id);
}

struct ObjectAccess {
    template <typename T, typename... Args>
    static auto MakeUnique(Args&&... args)
    {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }

    template <typename T>
    static auto GetChildren(const T* parent)
    {
        NodePtrArray ret;
        ret.reserve(parent->m_children.size());
        for (const auto& node : parent->m_children) {
            ret.push_back(node.get());
        }
        return ret;
    }

    static void AddMeshInstancePtr(MeshNodePtr mesh_node, MaterialInstancePtr material_instance)
    {
        assert(mesh_node && material_instance);
        material_instance->AddMeshNodePtr(mesh_node);
    }

    static void ApplyTransform(NodePtr node, const glm::mat4& matrix) { node->ApplyTransform(matrix); }

    template <typename T>
    static void ThisToJson(const T* object, json& json)
    {
        json["object.class"] = object->metadata.object_class;
        json["object.id"]    = object->m_id;

        if (object->HasProperties()) {
            json["object.properties"] = DictionaryValues{ object->m_dictionary.get() };
        }
    }

    template <typename T>
    static void ChildrenToJson(const std::vector<T>& children, json& json)
    {
        json["owns"] = children;
    }
};

class MaterialManager {
  public:
    MaterialPtr CreateMaterial()
    {
        auto  id       = GetUniqueID();
        auto& material = m_materials[id] = ObjectAccess::MakeUnique<Material>(id);
        return material.get();
    }

    MaterialPtrArray GetMaterials() const
    {
        auto view = std::views::transform(m_materials, [](auto& material) { return material.second.get(); });
        return MaterialPtrArray(view.begin(), view.end());
    }

    json ToJson() const
    {
        auto view = std::views::transform(m_materials, [](auto& material) { return std::ref(*material.second); });
        return nlohmann::json(MaterialRefArray(view.begin(), view.end()));
    }

  private:
    std::unordered_map<ID, UniqueMaterial, ID::Hash> m_materials;
};

class MeshManager {
  public:
    MeshPtr CreateMesh(AABB aabb, MeshVertices vertices, MeshIndices indices)
    {
        auto  id   = GetUniqueID();
        auto& mesh = m_meshes[id] = ObjectAccess::MakeUnique<Mesh>(id, aabb, std::move(vertices), std::move(indices));
        return mesh.get();
    }

    json ToJson() const
    {
        auto view = std::views::transform(m_meshes, [](auto& mesh) { return std::ref(*mesh.second); });
        return nlohmann::json(MeshRefArray(view.begin(), view.end()));
    }

  private:
    std::unordered_map<ID, UniqueMesh, ID::Hash> m_meshes;
};

json Mesh::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);
    json["object.values"] = MeshValues{ &m_aabb, &m_vertices, &m_indices };

    return json;
}

ValueRef Mesh::GetField(std::string_view field_name)
{
    if (field_name == "aabb.min") {
        return ValueRef(&m_aabb.min);
    } else if (field_name == "aabb.max") {
        return ValueRef(&m_aabb.max);
    } else if (field_name == "vertex.attributes") {
        return ValueRef(&m_vertices.VertexAttributesRef());
    } else if (field_name == "vertex.size") {
        return ValueRef(&m_vertices.VertexSizeRef());
    } else if (field_name == "vertex.count") {
        return ValueRef(&m_vertices.CountRef());
    } else if (field_name == "index.type") {
        return ValueRef(&m_indices.IndexTypeRef());
    } else if (field_name == "index.size") {
        return ValueRef(&m_indices.IndexSizeRef());
    } else if (field_name == "index.count") {
        return ValueRef(&m_indices.CountRef());
    }
    return {};
}

Metadata Mesh::metadata = {
    "object.mesh",
    "Mesh",
    nullptr,
    { Field{ "aabb.min", "Min", nullptr, ValueType::Float3, Field::IsEditable{ false } },
      Field{ "aabb.max", "Max", nullptr, ValueType::Float3, Field::IsEditable{ false } },
      Field{ "vertex.attributes", "Vertex Attributes", nullptr, ValueType::String, Field::IsEditable{ false } },
      Field{ "vertex.size", "Vertex Size", nullptr, ValueType::Int, Field::IsEditable{ false } },
      Field{ "vertex.count", "Vertex Count", nullptr, ValueType::Int, Field::IsEditable{ false } },
      Field{ "index.type", "Index Type", nullptr, ValueType::String, Field::IsEditable{ false } },
      Field{ "index.size", "Index Size", nullptr, ValueType::Int, Field::IsEditable{ false } },
      Field{ "index.count", "Index Count", nullptr, ValueType::Int, Field::IsEditable{ false } } }
};

TranslateNodePtr InternalNode::AddTranslateNode(float x, float y, float z)
{
    m_children.push_back(ObjectAccess::MakeUnique<TranslateNode>(GetUniqueID(), x, y, z));
    return static_cast<TranslateNodePtr>(m_children.back().get());
}

RotateNodePtr InternalNode::AddRotateNode(float x, float y, float z, Radians angle)
{
    m_children.push_back(ObjectAccess::MakeUnique<RotateNode>(GetUniqueID(), x, y, z, angle));
    return static_cast<RotateNodePtr>(m_children.back().get());
}

ScaleNodePtr InternalNode::AddScaleNode(float factor)
{
    m_children.push_back(ObjectAccess::MakeUnique<ScaleNode>(GetUniqueID(), factor));
    return static_cast<ScaleNodePtr>(m_children.back().get());
}

MeshNodePtr InternalNode::AddMeshNode(MeshPtr mesh, MaterialInstancePtr material)
{
    assert(mesh && material);
    m_children.push_back(ObjectAccess::MakeUnique<MeshNode>(GetUniqueID(), mesh, material));
    return static_cast<MeshNodePtr>(m_children.back().get());
}

NodePtrArray InternalNode::GetChildren() const
{
    return ObjectAccess::GetChildren(this);
}

Scene::Scene()
{
    m_root_node        = ObjectAccess::MakeUnique<RootNode>(GetUniqueID());
    m_material_manager = std::make_unique<MaterialManager>();
    m_mesh_manager     = std::make_unique<MeshManager>();
}

DrawList Scene::ComputeDrawList() const
{
    using namespace std::ranges;

    ObjectAccess::ApplyTransform(m_root_node.get(), glm::identity<glm::mat4>());

    auto draw_list = DrawList{};

    for (auto material : m_material_manager->GetMaterials()) {
        for (auto material_instance : material->GetMaterialInstances()) {
            for (auto mesh_node : material_instance->GetMeshNodes()) {
                draw_list.push_back({ mesh_node->GetMeshPtr(), mesh_node->GetTransformPtr() });
            }
        }
    }

    return draw_list;
}

AABB Scene::ComputeAxisAlignedBoundingBox() const
{
    using namespace std::ranges;

    if (m_root_node->GetChildren().empty()) {
        return AABB{ { -1, -1, -1 }, { 1, 1, 1 } };
    }

    ObjectAccess::ApplyTransform(m_root_node.get(), glm::identity<glm::mat4>());

    auto out = AABB{ { FLT_MAX, FLT_MAX, FLT_MAX }, { FLT_MIN, FLT_MIN, FLT_MIN } };

    for (auto material : m_material_manager->GetMaterials()) {
        for (auto material_instance : material->GetMaterialInstances()) {
            for (auto mesh_node : material_instance->GetMeshNodes()) {
                if (auto* mesh = mesh_node->GetMeshPtr()) {
                    auto& model = *mesh_node->GetTransformPtr();
                    auto& aabb  = mesh->GetBoundingBox();
                    auto  vec_a = model * glm::vec4(aabb.min.x, aabb.min.y, aabb.min.z, 1);
                    auto  vec_b = model * glm::vec4(aabb.max.x, aabb.max.y, aabb.max.z, 1);
                    out.min.x   = std::min({ out.min.x, vec_a.x, vec_b.x });
                    out.min.y   = std::min({ out.min.y, vec_a.y, vec_b.y });
                    out.min.z   = std::min({ out.min.z, vec_a.z, vec_b.z });
                    out.max.x   = std::max({ out.max.x, vec_a.x, vec_b.x });
                    out.max.y   = std::max({ out.max.y, vec_a.y, vec_b.y });
                    out.max.z   = std::max({ out.max.z, vec_a.z, vec_b.z });
                }
            }
        }
    }

    return out;
}

ValueRef RootNode::GetField(std::string_view)
{
    return {};
}

Metadata RootNode::metadata = { "root.node", "Root", nullptr, {} };

json RootNode::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);
    ObjectAccess::ChildrenToJson(m_children, json);

    return json;
}

void RootNode::ApplyTransform(const glm::mat4& matrix) noexcept
{
    for (auto& node : m_children) {
        ObjectAccess::ApplyTransform(static_cast<NodePtr>(node.get()), matrix);
    }
}

json MeshNode::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);

    json["object.refs"] = MeshNodeRefs{ m_material_instance->GetID(), m_mesh->GetID() };

    return json;
}

ValueRef MeshNode::GetField(std::string_view field_name)
{
    if (field_name == "mesh") {
        return ValueRef(m_mesh);
    } else if (field_name == "material.instance") {
        return ValueRef(m_material_instance);
    }
    return {};
}

Metadata MeshNode::metadata = {
    "mesh.node",
    "Mesh",
    nullptr,
    { Field{ "mesh", "Mesh", nullptr, ValueType::Reference, Field::IsEditable{ false } },
      Field{ "material.instance", "Material Instance", nullptr, ValueType::Reference, Field::IsEditable{ false } } }
};

void MeshNode::ApplyTransform(const glm::mat4& matrix) noexcept
{
    m_transform = matrix;
}

MeshNode::MeshNode(ID id, MeshPtr mesh, MaterialInstancePtr material_instance) noexcept
    : Node(id), m_mesh(mesh), m_material_instance(material_instance)
{
    ObjectAccess::AddMeshInstancePtr(this, material_instance);
}

ValueRef TranslateNode::GetField(std::string_view field_name)
{
    return field_name == "translate.amount" ? ValueRef(&m_amount) : ValueRef();
}

Metadata TranslateNode::metadata = {
    "translate.node",
    "Translate",
    nullptr,
    { Field{ "translate.amount", "Amount", nullptr, ValueType::Float3, Field::IsEditable{ true } } }
};

json TranslateNode::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);
    ObjectAccess::ChildrenToJson(m_children, json);

    json["object.values"] = TranslateValues{ m_amount };

    return json;
}

void TranslateNode::ApplyTransform(const glm::mat4& matrix) noexcept
{
    for (auto& node : m_children) {
        auto node_ptr = static_cast<NodePtr>(node.get());
        auto amount   = glm::vec3(m_amount.x, m_amount.y, m_amount.z);
        ObjectAccess::ApplyTransform(node_ptr, matrix * glm::translate(amount));
    }
}

json RotateNode::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);
    ObjectAccess::ChildrenToJson(m_children, json);

    json["object.values"] = RotateValues{ m_axis, m_angle.value };

    return json;
}

ValueRef RotateNode::GetField(std::string_view field_name)
{
    if (field_name == "rotate.axis") {
        return ValueRef(&m_axis);
    } else if (field_name == "rotate.angle") {
        return ValueRef(&m_angle.value);
    }
    return {};
}

Metadata RotateNode::metadata = {
    "rotate.node",
    "Rotate",
    nullptr,
    { Field{ "rotate.axis", "Axis", nullptr, ValueType::Float3, Field::IsEditable{ true } },
      Field{ "rotate.angle", "Angle", nullptr, ValueType::Float, Field::IsEditable{ true } } }
};

void RotateNode::ApplyTransform(const glm::mat4& matrix) noexcept
{
    for (auto& node : m_children) {
        auto node_ptr = static_cast<NodePtr>(node.get());
        auto axis     = glm::vec3(m_axis.x, m_axis.y, m_axis.z);
        ObjectAccess::ApplyTransform(node_ptr, matrix * glm::rotate(m_angle.value, axis));
    }
}

json ScaleNode::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);
    ObjectAccess::ChildrenToJson(m_children, json);

    json["object.values"] = ScaleValues{ m_factor };

    return json;
}

ValueRef ScaleNode::GetField(std::string_view field_name)
{
    return field_name == "scale.factor" ? ValueRef(&m_factor) : ValueRef();
}

void ScaleNode::ApplyTransform(const glm::mat4& matrix) noexcept
{
    for (auto& node : m_children) {
        auto node_ptr = static_cast<NodePtr>(node.get());
        ObjectAccess::ApplyTransform(node_ptr, matrix * glm::scale(glm::vec3{ m_factor, m_factor, m_factor }));
    }
}

Metadata ScaleNode::metadata = {
    "scale.node",
    "Scale",
    nullptr,
    { Field{ "scale.factor", "Factor", nullptr, ValueType::Float, Field::IsEditable{ true } } }
};

bool Object::HasProperties() const noexcept
{
    return m_dictionary && !m_dictionary->empty();
}

void Object::SetProperty(Key key, Value value)
{
    if (m_dictionary == nullptr) {
        m_dictionary.reset(new Dictionary);
    }
    m_dictionary->insert_or_assign(std::move(key), std::move(value));
}

void Object::RemoveProperty(Key key)
{
    if (m_dictionary) {
        m_dictionary->erase(key);
    }
}

RootNodePtr Scene::GetRootNodePtr() noexcept
{
    return static_cast<RootNodePtr>(m_root_node.get());
}

MaterialPtr Scene::CreateMaterial()
{
    return m_material_manager->CreateMaterial();
}

json Scene::ToJson() const
{
    json json;

    json["scene"]     = m_root_node->ToJson();
    json["materials"] = m_material_manager->ToJson();
    json["meshes"]    = m_mesh_manager->ToJson();

    return json;
}

MeshPtr Scene::CreateMesh(AABB aabb, MeshVertices mesh_vertices, MeshIndices mesh_indices)
{
    return m_mesh_manager->CreateMesh(aabb, std::move(mesh_vertices), std::move(mesh_indices));
}

Scene::~Scene()
{}

MaterialInstancePtr Material::CreateMaterialInstance()
{
    m_instances.push_back(ObjectAccess::MakeUnique<MaterialInstance>(GetUniqueID()));
    return static_cast<MaterialInstancePtr>(m_instances.back().get());
}

MaterialInstancePtrArray Material::GetMaterialInstances() const
{
    auto view = std::views::transform(m_instances, [](auto& instance) { return instance.get(); });
    return MaterialInstancePtrArray(view.begin(), view.end());
}

json Material::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);
    ObjectAccess::ChildrenToJson(m_instances, json);

    return json;
}

json MaterialInstance::ToJson() const
{
    json json;

    ObjectAccess::ThisToJson(this, json);

    json["object.refs"] = m_mesh_nodes;

    return json;
}

void MaterialInstance::AddMeshNodePtr(MeshNodePtr mesh_node)
{
    m_mesh_nodes.push_back(mesh_node);
}

std::string to_string(ValueType value_type)
{
    switch (value_type) {
    case ValueType::Null: return "Null";
    case ValueType::Float: return "Float";
    case ValueType::Int: return "Int";
    case ValueType::Reference: return "Reference";
    case ValueType::String: return "String";
    case ValueType::Float3: return "Float3";
    default: utils::throw_runtime_error("ValueType: Bad enum value");
    };
    return {};
}
