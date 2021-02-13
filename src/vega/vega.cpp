#include "etna/etna.hpp"

#include "camera.hpp"
#include "descriptor_manager.hpp"
#include "frame_manager.hpp"
#include "gui.hpp"
#include "mesh_store.hpp"
#include "render_context.hpp"
#include "scene.hpp"
#include "swapchain_manager.hpp"
#include "utils/misc.hpp"
#include "utils/resource.hpp"

BEGIN_DISABLE_WARNINGS

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtx/transform.hpp>
#include <glm/matrix.hpp>

#include <spdlog/spdlog.h>

END_DISABLE_WARNINGS

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

enum class KhronosValidation { Disable, Enable };

struct VertexPN final {
    constexpr VertexPN(const glm::vec3& position, const glm::vec3 normal) noexcept : position(position), normal(normal)
    {}
    glm::vec3 position;
    glm::vec3 normal;
};

bool operator==(const VertexPN& lhs, const VertexPN& rhs) noexcept
{
    return (lhs.position == rhs.position) && (glm::dot(lhs.normal, rhs.normal) > 0.999847695f);
}

DECLARE_VERTEX_ATTRIBUTE_TYPE(glm::vec3, etna::Format::R32G32B32Sfloat)

DECLARE_VERTEX_TYPE(VertexPN, Position3f | Normal3f)

struct Index final {
    Index() noexcept = default;

    constexpr Index(tinyobj::index_t idx) noexcept
        : vertex(static_cast<uint32_t>(idx.vertex_index)), normal(static_cast<uint32_t>(idx.normal_index)),
          texcoord(static_cast<uint32_t>(idx.texcoord_index))
    {}

    uint32_t vertex;
    uint32_t normal;
    uint32_t texcoord;
};

constexpr bool operator==(const Index& lhs, const Index& rhs) noexcept
{
    return lhs.vertex == rhs.vertex && lhs.normal == rhs.normal && lhs.texcoord == rhs.texcoord;
}

namespace std {

template <>
struct hash<VertexPN> {
    size_t operator()(const VertexPN& vertex) const noexcept
    {
        size_t hash = 23;

        hash = hash * 31 + std::hash<float>{}(vertex.position.x);
        hash = hash * 31 + std::hash<float>{}(vertex.position.y);
        hash = hash * 31 + std::hash<float>{}(vertex.position.z);

        return hash;
    }
};

template <>
struct hash<Index> {
    size_t operator()(const Index& index) const noexcept
    {
        size_t hash = 23;

        hash = hash * 31 + index.vertex;
        hash = hash * 31 + index.normal;
        hash = hash * 31 + index.texcoord;

        return hash;
    }
};

} // namespace std

struct GLFW {
    GLFW()
    {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    ~GLFW() { glfwTerminate(); }
} glfw;

static MeshPtr GenerateMeshPN(ScenePtr scene, const tinyobj::attrib_t& attributes, const tinyobj::mesh_t& mesh)
{
    auto num_indices = mesh.indices.size();

    assert(num_indices % 3 == 0);

    auto index_map = std::unordered_map<Index, uint32_t>{};
    auto vertices  = std::vector<VertexPN>{};
    auto indices   = std::vector<uint32_t>{};

    auto aabb = AABB{ { FLT_MAX, FLT_MAX, FLT_MAX }, { FLT_MIN, FLT_MIN, FLT_MIN } };

    for (size_t i = 0; i < num_indices; i += 3) {
        for (std::size_t j = 0; j < 3; ++j) {
            auto index = Index(mesh.indices[i + j]);
            if (auto it = index_map.find(index); it != index_map.end()) {
                indices.push_back(it->second);
            } else {
                auto position_idx = static_cast<size_t>(3) * index.vertex;
                auto normal_idx   = static_cast<size_t>(3) * index.normal;

                auto position = glm::vec3(
                    attributes.vertices[position_idx + 0],
                    attributes.vertices[position_idx + 1],
                    attributes.vertices[position_idx + 2]);

                auto normal = glm::vec3(
                    attributes.normals[normal_idx + 0],
                    attributes.normals[normal_idx + 1],
                    attributes.normals[normal_idx + 2]);

                auto vertex = VertexPN(position, normal);

                vertices.push_back(vertex);

                aabb.min = { std::min(aabb.min.x, vertex.position.x),
                             std::min(aabb.min.y, vertex.position.y),
                             std::min(aabb.min.z, vertex.position.z) };

                aabb.max = { std::max(aabb.max.x, vertex.position.x),
                             std::max(aabb.max.y, vertex.position.y),
                             std::max(aabb.max.z, vertex.position.z) };

                auto idx         = static_cast<uint32_t>(vertices.size() - 1);
                index_map[index] = idx;

                indices.push_back(idx);
            }
        }
    }

    return scene->CreateMesh(aabb, std::move(vertices), std::move(indices));
}

static MeshPtr GenerateMeshP(ScenePtr scene, const tinyobj::attrib_t& attributes, const tinyobj::mesh_t& mesh)
{
    auto num_indices = mesh.indices.size();

    assert(num_indices % 3 == 0);

    auto vertices = std::vector<VertexPN>{};
    auto indices  = std::vector<uint32_t>{};

    auto index_map = std::unordered_map<VertexPN, size_t>();

    auto aabb = AABB{ { FLT_MAX, FLT_MAX, FLT_MAX }, { FLT_MIN, FLT_MIN, FLT_MIN } };

    for (size_t i = 0; i < num_indices; i += 3) {
        auto i0     = size_t(3) * mesh.indices[i + 0].vertex_index;
        auto i1     = size_t(3) * mesh.indices[i + 1].vertex_index;
        auto i2     = size_t(3) * mesh.indices[i + 2].vertex_index;
        auto pos0   = glm::vec3(attributes.vertices[i0 + 0], attributes.vertices[i0 + 1], attributes.vertices[i0 + 2]);
        auto pos1   = glm::vec3(attributes.vertices[i1 + 0], attributes.vertices[i1 + 1], attributes.vertices[i1 + 2]);
        auto pos2   = glm::vec3(attributes.vertices[i2 + 0], attributes.vertices[i2 + 1], attributes.vertices[i2 + 2]);
        auto e1     = pos1 - pos0;
        auto e2     = pos2 - pos0;
        auto normal = glm::normalize(glm::cross(e1, e2));

        auto vertex0 = VertexPN(pos0, normal);
        auto vertex1 = VertexPN(pos1, normal);
        auto vertex2 = VertexPN(pos2, normal);

        size_t index0 = vertices.size();
        if (auto [kv, is_inserted] = index_map.insert({ vertex0, index0 }); is_inserted) {
            vertices.push_back(vertex0);
        } else {
            index0 = kv->second;
        }

        size_t index1 = vertices.size();
        if (auto [kv, is_inserted] = index_map.insert({ vertex1, index1 }); is_inserted) {
            vertices.push_back(vertex1);
        } else {
            index1 = kv->second;
        }

        size_t index2 = vertices.size();
        if (auto [kv, is_inserted] = index_map.insert({ vertex2, index2 }); is_inserted) {
            vertices.push_back(vertex2);
        } else {
            index2 = kv->second;
        }

        indices.push_back(etna::narrow_cast<uint32_t>(index0));
        indices.push_back(etna::narrow_cast<uint32_t>(index1));
        indices.push_back(etna::narrow_cast<uint32_t>(index2));

        aabb.Expand({ pos0.x, pos0.y, pos0.z });
        aabb.Expand({ pos1.x, pos1.y, pos1.z });
        aabb.Expand({ pos2.x, pos2.y, pos2.z });
    }

    return scene->CreateMesh(aabb, std::move(vertices), std::move(indices));
}

void LoadObj(ScenePtr scene, std::filesystem::path filepath)
{
    namespace fs = std::filesystem;

    if (false == fs::exists(filepath)) {
        throw std::runtime_error("File does not exist");
    }

    auto parent_dir = fs::path(filepath).parent_path();

    auto attributes = tinyobj::attrib_t{};
    auto shapes     = std::vector<tinyobj::shape_t>{};
    auto materials  = std::vector<tinyobj::material_t>{};
    auto warning    = std::string{};
    auto err        = std::string{};

    bool success = tinyobj::LoadObj(
        &attributes,
        &shapes,
        &materials,
        &warning,
        &err,
        filepath.string().c_str(),
        parent_dir.string().c_str(),
        true,
        false);

    if (success == false) {
        throw std::runtime_error("Failed to load file");
    }

    auto material          = scene->CreateMaterial();
    auto material_instance = material->CreateMaterialInstance();

    auto root_node = scene->GetRootNodePtr();

    for (const auto& shape : shapes) {
        auto mesh = MeshPtr{};

        if (attributes.normals.empty()) {
            mesh = GenerateMeshP(scene, attributes, shape.mesh);
        } else {
            mesh = GenerateMeshPN(scene, attributes, shape.mesh);
        }

        mesh->SetProperty("Name", shape.name);

        auto t_node = root_node->AddTranslateNode(0, 0, 0);
        auto r_node = t_node->AddRotateNode(0, 0, 1, 0_rad);
        auto s_node = r_node->AddScaleNode(1);

        s_node->AddMeshNode(mesh, material_instance);
    }
}

struct QueueInfo final {
    uint32_t         family_index;
    etna::QueueFlags flags;
    uint32_t         count;
};

struct QueueFamilies final {
    QueueInfo graphics;
    QueueInfo compute;
    QueueInfo transfer;
    QueueInfo presentation;
};

struct Queues final {
    etna::Queue graphics;
    etna::Queue compute;
    etna::Queue transfer;
    etna::Queue presentation;
};

template <typename T>
std::vector<T> RemoveDuplicates(std::initializer_list<T> l)
{
    std::vector<T> v(l.begin(), l.end());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

constexpr float ComputeAspect(etna::Extent2D extent)
{
    return etna::narrow_cast<float>(extent.width) / etna::narrow_cast<float>(extent.height);
}

VkBool32 VulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT vk_message_severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void*)
{
    auto message_severity = static_cast<etna::DebugUtilsMessageSeverity>(vk_message_severity);

    switch (message_severity) {
    case etna::DebugUtilsMessageSeverity::Verbose: spdlog::debug(callback_data->pMessage); break;
    case etna::DebugUtilsMessageSeverity::Info: spdlog::info(callback_data->pMessage); break;
    case etna::DebugUtilsMessageSeverity::Warning: spdlog::warn(callback_data->pMessage); break;
    case etna::DebugUtilsMessageSeverity::Error: spdlog::error(callback_data->pMessage); break;
    default:
        spdlog::warn("Vulkan message callback message severity not recognized");
        spdlog::error(callback_data->pMessage);
        break;
    }

    return VK_FALSE;
}

QueueFamilies GetQueueFamilyInfo(etna::PhysicalDevice gpu, etna::SurfaceKHR surface)
{
    using namespace etna;

    auto properties = gpu.GetPhysicalDeviceQueueFamilyProperties();

    constexpr auto mask = QueueFlags::Graphics | QueueFlags::Compute | QueueFlags::Transfer;

    std::optional<QueueInfo> graphics;

    std::optional<QueueInfo> presentation;
    std::optional<QueueInfo> graphics_presentation;
    std::optional<QueueInfo> mixed_presentation;

    std::optional<QueueInfo> compute;
    std::optional<QueueInfo> dedicated_compute;
    std::optional<QueueInfo> graphics_compute;
    std::optional<QueueInfo> mixed_compute;

    std::optional<QueueInfo> transfer;
    std::optional<QueueInfo> dedicated_transfer;
    std::optional<QueueInfo> graphics_transfer;
    std::optional<QueueInfo> mixed_transfer;

    for (std::size_t i = 0; i != properties.size(); ++i) {
        const auto family_index = narrow_cast<uint32_t>(i);
        const auto queue_flags  = properties[i].queueFlags;
        const auto queue_count  = properties[i].queueCount;
        const auto masked_flags = queue_flags & mask;
        const auto queue_info   = QueueInfo{ family_index, queue_flags, queue_count };

        if (masked_flags & QueueFlags::Graphics) {
            if (!graphics.has_value() || queue_count > graphics->count) {
                graphics = queue_info;
            }
        }

        if (masked_flags == QueueFlags::Compute) {
            if (!dedicated_compute.has_value() || queue_count > dedicated_compute->count) {
                dedicated_compute = queue_info;
            }
        } else if (masked_flags & QueueFlags::Compute) {
            if (masked_flags & QueueFlags::Graphics) {
                if (!graphics_compute.has_value() || queue_count > graphics_compute->count) {
                    graphics_compute = queue_info;
                }
            } else {
                if (!mixed_compute.has_value() || queue_count > mixed_compute->count) {
                    mixed_compute = queue_info;
                }
            }
        }

        if (masked_flags == QueueFlags::Transfer) {
            if (!dedicated_transfer.has_value() || queue_count > dedicated_transfer->count) {
                dedicated_transfer = queue_info;
            }
        } else if (masked_flags & QueueFlags::Transfer) {
            if (masked_flags & QueueFlags::Graphics) {
                if (!graphics_transfer.has_value() || queue_count > graphics_transfer->count) {
                    graphics_transfer = queue_info;
                }
            } else {
                if (!mixed_transfer.has_value() || queue_count > mixed_transfer->count) {
                    mixed_transfer = queue_info;
                }
            }
        }

        if (gpu.GetPhysicalDeviceSurfaceSupportKHR(family_index, surface)) {
            if (masked_flags & QueueFlags::Graphics) {
                if (!graphics_presentation.has_value()) {
                    graphics_presentation = queue_info;
                }
            } else {
                mixed_presentation = queue_info;
            }
        }
    }

    if (false == graphics.has_value()) {
        throw std::runtime_error("Failed to detect GPU graphics queue!");
    }

    if (dedicated_compute.has_value()) {
        compute = dedicated_compute;
    } else if (mixed_compute.has_value()) {
        compute = mixed_compute;
    } else if (graphics_compute.has_value()) {
        compute = graphics_compute;
    }

    if (false == compute.has_value()) {
        throw std::runtime_error("Failed to detect GPU compute queue!");
    }

    if (dedicated_transfer.has_value()) {
        transfer = dedicated_transfer;
    } else if (mixed_transfer.has_value()) {
        transfer = mixed_transfer;
    } else if (graphics_transfer.has_value()) {
        transfer = graphics_transfer;
    }

    if (false == transfer.has_value()) {
        throw std::runtime_error("Failed to detect GPU transfer queue!");
    }

    if (graphics_presentation.has_value()) {
        presentation = graphics_presentation;
    } else if (mixed_presentation.has_value()) {
        presentation = mixed_presentation;
    }

    if (false == presentation.has_value()) {
        throw std::runtime_error("Failed to detect GPU presentation queue!");
    }

    return { graphics.value(), compute.value(), transfer.value(), presentation.value() };
}

etna::SurfaceFormatKHR FindOptimalSurfaceFormatKHR(
    etna::PhysicalDevice                          gpu,
    etna::SurfaceKHR                              surface,
    std::initializer_list<etna::SurfaceFormatKHR> preffered_formats)
{
    auto available_formats = gpu.GetPhysicalDeviceSurfaceFormatsKHR(surface);

    if (available_formats.empty()) {
        throw std::runtime_error("Failed to find supported surface format!");
    }

    auto it = std::ranges::find_first_of(available_formats, preffered_formats);

    return it == available_formats.end() ? available_formats.front() : *it;
}

etna::Format FindSupportedFormat(
    etna::PhysicalDevice                gpu,
    std::initializer_list<etna::Format> candidate_formats,
    etna::ImageTiling                   tiling,
    etna::FormatFeature                 required_features)
{
    using namespace etna;

    for (auto format : candidate_formats) {
        auto format_features = gpu.GetPhysicalDeviceFormatProperties(format);
        if (tiling == ImageTiling::Linear) {
            if (required_features == (format_features.linearTilingFeatures & required_features)) {
                return format;
            }
        } else if (tiling == ImageTiling::Optimal) {
            if (required_features == (format_features.optimalTilingFeatures & required_features)) {
                return format;
            }
        }
    }

    throw std::runtime_error("Failed to find supported depth format!");
}

struct GlfwWindowDeleter {
    void operator()(GLFWwindow* window) { glfwDestroyWindow(window); }
};

using GlfwWindow = std::unique_ptr<GLFWwindow, GlfwWindowDeleter>;

void GlfwErrorCallback(int, const char* description)
{
    spdlog::error("GLFW: {}", description);
}

GlfwWindow CreateGlfwWindow(const char* window_name)
{
    GLFWmonitor* primary_monitor = glfwGetPrimaryMonitor();

    int window_width  = 0;
    int window_height = 0;
    int window_pos_x  = 0;
    int window_pos_y  = 0;
    {
        int xpos{}, ypos{}, width{}, height{};
        glfwGetMonitorWorkarea(primary_monitor, &xpos, &ypos, &width, &height);
        window_width  = width * 3 / 4;
        window_height = height * 3 / 4;
        window_pos_x  = (width - window_width) / 2;
        window_pos_y  = (height - window_height) / 2;
    }

    auto window = glfwCreateWindow(window_width, window_height, window_name, nullptr, nullptr);
    if (!window) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowPos(window, window_pos_x, window_pos_y);

    return GlfwWindow(window);
}

auto CreateEtnaInstance(KhronosValidation khronos_validation) -> etna::UniqueInstance
{
    using namespace etna;

    utils::throw_runtime_error_if(false == glfwVulkanSupported(), "GLFW Vulkan not supported!");

    auto count           = 0U;
    auto glfw_extensions = glfwGetRequiredInstanceExtensions(&count);
    auto extensions      = std::vector<const char*>(glfw_extensions, glfw_extensions + count);
    auto layers          = std::vector<const char*>();

    if (khronos_validation == KhronosValidation::Enable) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    return CreateInstance(
        "Vega",
        Version{ 0, 1, 0 },
        extensions,
        layers,
        VulkanDebugCallback,
        DebugUtilsMessageSeverity::Warning | DebugUtilsMessageSeverity::Error,
        DebugUtilsMessageType::General | DebugUtilsMessageType::Performance | DebugUtilsMessageType::Validation);
}

auto GetEtnaGpu(etna::Instance instance)
{
    auto gpus                      = instance.EnumeratePhysicalDevices();
    auto gpu                       = gpus.front();
    auto queue_family_properties   = gpu.GetPhysicalDeviceQueueFamilyProperties();
    bool is_presentation_supported = false;

    for (uint32_t index = 0; index < queue_family_properties.size(); ++index) {
        is_presentation_supported |= (GLFW_TRUE == glfwGetPhysicalDevicePresentationSupport(instance, gpu, index));
    }

    utils::throw_runtime_error_if(!is_presentation_supported, "Failed to detect GPU queue that supports presentation");

    return gpu;
}

etna::UniqueSurfaceKHR CreateEtnaSurface(etna::Instance instance, GLFWwindow* glfw_window)
{
    auto vk_surface = VkSurfaceKHR{};

    auto success = (VK_SUCCESS == glfwCreateWindowSurface(instance, glfw_window, nullptr, &vk_surface));

    utils::throw_runtime_error_if(false == success, "Failed to create window surface");

    return etna::UniqueSurfaceKHR(etna::SurfaceKHR(instance, vk_surface));
}

etna::UniqueDevice GetEtnaDevice(etna::Instance instance, etna::PhysicalDevice gpu, const QueueFamilies& queue_families)
{
    auto queue_family_indices = RemoveDuplicates({

        queue_families.graphics.family_index,
        queue_families.compute.family_index,
        queue_families.transfer.family_index,
        queue_families.presentation.family_index });

    auto builder = etna::Device::Builder();

    for (auto queue_family_index : queue_family_indices) {
        builder.AddQueue(queue_family_index, 1);
    }

    builder.AddEnabledExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    return instance.CreateDevice(gpu, builder.state);
}

etna::Extent2D ComputeEtnaExtent(etna::PhysicalDevice gpu, GLFWwindow* glfw_window, etna::SurfaceKHR surface)
{
    int width{}, height{};
    glfwGetWindowSize(glfw_window, &width, &height);

    auto extent       = etna::Extent2D{ etna::narrow_cast<uint32_t>(width), etna::narrow_cast<uint32_t>(height) };
    auto capabilities = gpu.GetPhysicalDeviceSurfaceCapabilitiesKHR(surface);

    extent.width  = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    }

    return extent;
}

class EventHandler {
  public:
    EventHandler(
        RenderContext* render_context,
        GLFWwindow*    glfw_window,
        Scene*         scene,
        Camera*        camera,
        MeshStore*     mesh_store)
        : m_render_context(render_context), m_glfw_window(glfw_window), m_scene(scene), m_camera(camera),
          m_mesh_store(mesh_store)
    {}

    void ScheduleCloseWindow() noexcept
    {
        m_event = Event::CloseWindow;
        m_render_context->StopRenderLoop();
    }

    void ScheduleLoadFile(std::string filepath) noexcept
    {
        m_event                         = Event::LoadFile;
        m_load_file_parameters.filepath = std::move(filepath);
        m_render_context->StopRenderLoop();
    }

    void HandleEvent()
    {
        switch (m_event) {
        case Event::None: break;
        case Event::CloseWindow: CloseWindow(); break;
        case Event::LoadFile: LoadFile(); break;
        default: break;
        }

        m_event = Event::None;
    }

  private:
    enum class Event { None, CloseWindow, LoadFile };

    struct LoadFileParameters final {
        std::string filepath;
    } m_load_file_parameters;

    void CloseWindow() { glfwSetWindowShouldClose(m_glfw_window, GLFW_TRUE); }

    void LoadFile()
    {
        LoadObj(m_scene, m_load_file_parameters.filepath);

        auto draw_list = m_scene->ComputeDrawList();
        for (DrawRecord draw_record : draw_list) {
            m_mesh_store->Add(draw_record.mesh);
        }
        m_mesh_store->Upload();

        auto aabb = m_scene->ComputeAxisAlignedBoundingBox();

        int width{}, height{};
        glfwGetWindowSize(m_glfw_window, &width, &height);

        auto aspect = static_cast<float>(width) / height;

        *m_camera = Camera::Create(
            Orientation::RightHanded,
            Forward{ Axis::PositiveY },
            Up{ Axis::PositiveZ },
            ObjectView::Front,
            aabb,
            45_deg,
            aspect);
    }

    RenderContext* m_render_context;
    GLFWwindow*    m_glfw_window;
    Scene*         m_scene;
    Camera*        m_camera;
    MeshStore*     m_mesh_store;
    Event          m_event = Event::None;
};

int main()
{
#ifdef NDEBUG
    const KhronosValidation khronos_validation = KhronosValidation::Disable;
#else
    const KhronosValidation khronos_validation = KhronosValidation::Enable;
#endif

    using namespace etna;

    auto instance       = CreateEtnaInstance(khronos_validation);
    auto gpu            = GetEtnaGpu(instance.get());
    auto gpu_properties = gpu.GetPhysicalDeviceProperties();

    spdlog::info("GPU Info: {}, {}", gpu_properties.deviceName, to_string(gpu_properties.deviceType));
    spdlog::info("GLFW Version: {}", glfwGetVersionString());

    glfwSetErrorCallback(GlfwErrorCallback);

    auto glfw_window              = CreateGlfwWindow("Vega Viewer");
    auto surface                  = CreateEtnaSurface(instance.get(), glfw_window.get());
    auto extent                   = ComputeEtnaExtent(gpu, glfw_window.get(), surface.get());
    auto aspect                   = ComputeAspect(extent);
    auto preffered_surface_format = SurfaceFormatKHR{ Format::B8G8R8A8Srgb, ColorSpaceKHR::SrgbNonlinear };
    auto surface_format           = FindOptimalSurfaceFormatKHR(gpu, surface.get(), { preffered_surface_format });
    auto depth_format             = FindSupportedFormat(
        gpu,
        { Format::D24UnormS8Uint, Format::D32SfloatS8Uint, Format::D16Unorm },
        ImageTiling::Optimal,
        FormatFeature::DepthStencilAttachment);

    spdlog::info("Surface Format: {}, {}", to_string(surface_format.format), to_string(surface_format.colorSpace));

    auto queue_families = GetQueueFamilyInfo(gpu, surface.get());
    auto device         = GetEtnaDevice(instance.get(), gpu, queue_families);
    auto queues         = Queues{};
    {
        queues.graphics     = device->GetQueue(queue_families.graphics.family_index);
        queues.compute      = device->GetQueue(queue_families.compute.family_index);
        queues.transfer     = device->GetQueue(queue_families.transfer.family_index);
        queues.presentation = device->GetQueue(queue_families.presentation.family_index);
    }

    // Create pipeline renderpass
    UniqueRenderPass renderpass;
    {
        auto builder = RenderPass::Builder();

        auto color_attachment = builder.AddAttachmentDescription(
            surface_format.format,
            AttachmentLoadOp::Clear,
            AttachmentStoreOp::Store,
            ImageLayout::Undefined,
            ImageLayout::ColorAttachmentOptimal);

        auto depth_attachment = builder.AddAttachmentDescription(
            depth_format,
            AttachmentLoadOp::Clear,
            AttachmentStoreOp::DontCare,
            ImageLayout::Undefined,
            ImageLayout::DepthStencilAttachmentOptimal);

        auto color_ref = builder.AddAttachmentReference(color_attachment, ImageLayout::ColorAttachmentOptimal);
        auto depth_ref = builder.AddAttachmentReference(depth_attachment, ImageLayout::DepthStencilAttachmentOptimal);

        auto subpass_builder = builder.GetSubpassBuilder();

        subpass_builder.AddColorAttachment(color_ref);
        subpass_builder.SetDepthStencilAttachment(depth_ref);

        auto subpass_id = builder.AddSubpass(subpass_builder.state);

        builder.AddSubpassDependency(
            SubpassID::External,
            subpass_id,
            PipelineStage::ColorAttachmentOutput,
            PipelineStage::ColorAttachmentOutput,
            Access{},
            Access::ColorAttachmentWrite);

        renderpass = device->CreateRenderPass(builder.state);
    }

    // Create gui pipeline renderpass
    UniqueRenderPass gui_renderpass;
    {
        auto builder = RenderPass::Builder();

        auto color_attachment = builder.AddAttachmentDescription(
            surface_format.format,
            AttachmentLoadOp::Load,
            AttachmentStoreOp::Store,
            ImageLayout::ColorAttachmentOptimal,
            ImageLayout::PresentSrcKHR);

        auto color_ref = builder.AddAttachmentReference(color_attachment, ImageLayout::ColorAttachmentOptimal);

        auto subpass_builder = builder.GetSubpassBuilder();

        subpass_builder.AddColorAttachment(color_ref);

        auto subpass_id = builder.AddSubpass(subpass_builder.state);

        builder.AddSubpassDependency(
            SubpassID::External,
            subpass_id,
            PipelineStage::ColorAttachmentOutput,
            PipelineStage::ColorAttachmentOutput,
            Access::ColorAttachmentRead,
            Access::ColorAttachmentWrite);

        gui_renderpass = device->CreateRenderPass(builder.state);
    }

    // Create descriptor set layouts
    UniqueDescriptorSetLayout descriptor_set_layout;
    {
        auto builder = DescriptorSetLayout::Builder();

        builder
            .AddDescriptorSetLayoutBinding(Binding{ 0 }, DescriptorType::UniformBufferDynamic, 1, ShaderStage::Vertex);
        builder.AddDescriptorSetLayoutBinding(Binding{ 1 }, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex);

        builder.AddDescriptorSetLayoutBinding(Binding{ 10 }, DescriptorType::UniformBuffer, 1, ShaderStage::Fragment);

        descriptor_set_layout = device->CreateDescriptorSetLayout(builder.state);
    }

    // Create pipeline pipeline_layout
    UniquePipelineLayout pipeline_layout;
    {
        auto builder = PipelineLayout::Builder();
        builder.AddDescriptorSetLayout(*descriptor_set_layout);
        pipeline_layout = device->CreatePipelineLayout(builder.state);
    }

    // Create pipeline
    UniquePipeline pipeline;
    {
        auto builder            = Pipeline::Builder(*pipeline_layout, *renderpass);
        auto [vs_data, vs_size] = GetResource("shaders/shader.vert");
        auto [fs_data, fs_size] = GetResource("shaders/shader.frag");
        auto vertex_shader      = device->CreateShaderModule(vs_data, vs_size);
        auto fragment_shader    = device->CreateShaderModule(fs_data, fs_size);
        auto width              = narrow_cast<float>(extent.width);
        auto height             = narrow_cast<float>(extent.height);
        auto viewport           = Viewport{ 0, height, width, -height, 0, 1 };
        auto scissor            = Rect2D{ Offset2D{ 0, 0 }, Extent2D{ extent } };

        builder.AddShaderStage(*vertex_shader, ShaderStage::Vertex);
        builder.AddShaderStage(*fragment_shader, ShaderStage::Fragment);
        builder.AddVertexInputBindingDescription(Binding{ 0 }, sizeof(VertexPN));
        builder.AddVertexInputAttributeDescription(
            Location{ 0 },
            Binding{ 0 },
            formatof(VertexPN, position),
            offsetof(VertexPN, position));
        builder.AddVertexInputAttributeDescription(
            Location{ 1 },
            Binding{ 0 },
            formatof(VertexPN, normal),
            offsetof(VertexPN, normal));
        builder.AddViewport(viewport);
        builder.AddScissor(scissor);
        builder.AddDynamicStates({ DynamicState::Viewport, DynamicState::Scissor });
        builder.SetDepthState(DepthTest::Enable, DepthWrite::Enable, CompareOp::Less);
        builder.AddColorBlendAttachmentState();

        pipeline = device->CreateGraphicsPipeline(builder.state);
    }

    auto mesh_store = MeshStore(*device, queues.transfer);

    uint32_t image_count = 3;
    uint32_t frame_count = 2;

    auto descriptor_manager = DescriptorManager(*device, frame_count, *descriptor_set_layout, gpu_properties.limits);

    auto render_context = RenderContext();

    auto scene = Scene();

    auto aabb = AABB();

    auto camera = Camera::Create(
        Orientation::RightHanded,
        Forward{ Axis::PositiveY },
        Up{ Axis::PositiveZ },
        ObjectView::Front,
        aabb,
        45_deg,
        aspect);

    auto lights = Lights();
    {
        lights.KeyRef().MultiplierRef() = 0.7f;
        lights.KeyRef().ElevationRef()  = ToRadians(45_deg).value;
        lights.KeyRef().AzimuthRef()    = ToRadians(-45_deg).value;

        lights.FillRef().MultiplierRef() = 0.05f;
        lights.FillRef().ElevationRef()  = ToRadians(5_deg).value;
        lights.FillRef().AzimuthRef()    = ToRadians(25_deg).value;
    }

    auto event_handler = EventHandler(&render_context, glfw_window.get(), &scene, &camera, &mesh_store);

    auto parameters = Gui::Parameters{

        .instance       = *instance,
        .gpu            = gpu,
        .device         = *device,
        .graphics_queue = queues.graphics,
        .renderpass     = *gui_renderpass,
        .extent         = extent
    };

    auto callbacks = Gui::Callbacks{

        .OnWindowClose = [&event_handler]() { event_handler.ScheduleCloseWindow(); },
        .OnFileOpen = [&event_handler](std::string filepath) { event_handler.ScheduleLoadFile(std::move(filepath)); }
    };

    auto gui = Gui(parameters, callbacks, glfw_window.get(), image_count, image_count, &camera, &scene, &lights);

    bool running = true;

    while (running) {
        auto swapchain_manager = SwapchainManager(
            *device,
            *renderpass,
            *gui_renderpass,
            *surface,
            image_count,
            surface_format,
            depth_format,
            extent,
            queues.presentation,
            PresentModeKHR::Fifo);

        auto frame_manager = FrameManager(*device, queue_families.graphics.family_index, frame_count);

        render_context = RenderContext(
            *device,
            queues.graphics,
            *pipeline,
            *pipeline_layout,
            glfw_window.get(),
            &swapchain_manager,
            &frame_manager,
            &descriptor_manager,
            &gui,
            &camera,
            &lights,
            &mesh_store,
            &scene);

        auto status = render_context.StartRenderLoop();

        device->WaitIdle();

        switch (status) {
        case RenderContext::Status::WindowClosed: {
            running = false;
            break;
        }
        case RenderContext::Status::SwapchainOutOfDate: {
            int width  = 0;
            int height = 0;
            while (width == 0 && height == 0) {
                glfwGetWindowSize(glfw_window.get(), &width, &height);
                if (width == 0 && height == 0) {
                    glfwWaitEvents();
                }
            }
            extent.width  = narrow_cast<uint32_t>(width);
            extent.height = narrow_cast<uint32_t>(height);
            gui.UpdateViewport(extent, swapchain_manager.MinImageCount());
            camera.UpdateAspect(ComputeAspect(extent));
            break;
        }
        case RenderContext::Status::GuiEvent: {
            event_handler.HandleEvent();
            break;
        }
        default: break;
        }
    }

    return 0;
}
