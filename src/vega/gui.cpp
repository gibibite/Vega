#include "gui.hpp"
#include "camera.hpp"
#include "platform.hpp"
#include "utils/resource.hpp"

#include "etna/command.hpp"
#include "etna/synchronization.hpp"

BEGIN_DISABLE_WARNINGS

#include "examples/imgui_impl_glfw.h"
#include "examples/imgui_impl_vulkan.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_access.hpp>

#include <spdlog/spdlog.h>

END_DISABLE_WARNINGS

#include <array>
#include <charconv>

namespace {

static Gui& Self(GLFWwindow* window)
{
    return *static_cast<Gui*>(glfwGetWindowUserPointer(window));
}

} // namespace

Gui::Gui(
    etna::Instance       instance,
    etna::PhysicalDevice gpu,
    etna::Device         device,
    uint32_t             queue_family_index,
    etna::Queue          graphics_queue,
    etna::RenderPass     renderpass,
    GLFWwindow*          window,
    etna::Extent2D       extent,
    uint32_t             min_image_count,
    uint32_t             image_count)
    : m_graphics_queue(graphics_queue), m_extent(extent)
{
    using namespace etna;

    // Setup GLFW callbacks
    {
        glfwSetWindowUserPointer(window, this);

        glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            Self(window).OnKey(key, scancode, action, mods);
        });

        glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) {
            Self(window).OnCursorPosition(xpos, ypos);
        });

        glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mods) {
            Self(window).OnMouseButton(button, action, mods);
        });

        glfwSetScrollCallback(window, [](GLFWwindow* window, double xoffset, double yoffset) {
            Self(window).OnScroll(xoffset, yoffset);
        });

        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int width, int height) {
            Self(window).OnFramebufferSize(width, height);
        });

        glfwSetWindowContentScaleCallback(window, [](GLFWwindow* window, float xscale, float yscale) {
            Self(window).OnContentScale(xscale, yscale);
        });

        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
    }

    // ImGui Init
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        spdlog::info("ImGui {}", ImGui::GetVersion());
    }

    // Create Descriptor Pool
    {
        DescriptorPoolSize pool_sizes[] = {

            { DescriptorType::Sampler, 1000 },
            { DescriptorType::CombinedImageSampler, 1000 },
            { DescriptorType::SampledImage, 1000 },
            { DescriptorType::StorageImage, 1000 },
            { DescriptorType::UniformTexelBuffer, 1000 },
            { DescriptorType::StorageTexelBuffer, 1000 },
            { DescriptorType::UniformBuffer, 1000 },
            { DescriptorType::StorageBuffer, 1000 },
            { DescriptorType::UniformBufferDynamic, 1000 },
            { DescriptorType::StorageBufferDynamic, 1000 },
            { DescriptorType::InputAttachment, 1000 }
        };
        m_descriptor_pool = device.CreateDescriptorPool(pool_sizes);
    }

    // Init Vulkan
    {
        auto init_info = ImGui_ImplVulkan_InitInfo{

            .Instance        = instance,
            .PhysicalDevice  = gpu,
            .Device          = device,
            .QueueFamily     = queue_family_index,
            .Queue           = graphics_queue,
            .PipelineCache   = nullptr,
            .DescriptorPool  = *m_descriptor_pool,
            .MinImageCount   = min_image_count,
            .ImageCount      = image_count,
            .MSAASamples     = VK_SAMPLE_COUNT_1_BIT,
            .Allocator       = nullptr,
            .CheckVkResultFn = nullptr
        };
        ImGui_ImplGlfw_InitForVulkan(window, true);
        ImGui_ImplVulkan_Init(&init_info, renderpass);
    }

    // Set Style
    ImGui::StyleColorsDark();

    // Add fonts
    {
        auto font_config = ImFontConfig();

        font_config.FontDataOwnedByAtlas = false;

        auto regular = GetResource("fonts/Roboto-Regular.ttf");
        auto data    = const_cast<unsigned char*>(regular.data);
        auto size    = narrow_cast<int>(regular.size);

        m_fonts.regular = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, size, 24.0f, &font_config);

        auto monospace = GetResource("fonts/RobotoMono-Regular.ttf");
        data           = const_cast<unsigned char*>(monospace.data);
        size           = narrow_cast<int>(monospace.size);

        font_config.FontDataOwnedByAtlas = false;

        m_fonts.monospace = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, size, 24.0f, &font_config);
    }

    // Upload Fonts
    {
        auto command_pool   = device.CreateCommandPool(queue_family_index, CommandPoolCreate::Transient);
        auto command_buffer = command_pool->AllocateCommandBuffer();

        command_buffer->Begin(CommandBufferUsage::OneTimeSubmit);
        ImGui_ImplVulkan_CreateFontsTexture(*command_buffer);
        command_buffer->End();

        graphics_queue.Submit(*command_buffer);

        device.WaitIdle();
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    }
}

Gui::~Gui() noexcept
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Gui::OnKey(int /*key*/, int /*scancode*/, int /*action*/, int /*mods*/)
{}

void Gui::OnCursorPosition(double xpos, double ypos)
{
    auto xposf = etna::narrow_cast<float>(xpos);
    auto yposf = etna::narrow_cast<float>(ypos);

    if (m_mouse_state.cursor.position.x >= 0.0f) {
        m_mouse_state.cursor.delta.x = xposf - m_mouse_state.cursor.position.x;
        m_mouse_state.cursor.delta.y = yposf - m_mouse_state.cursor.position.y;
    }
    m_mouse_state.cursor.position.x = xposf;
    m_mouse_state.cursor.position.y = yposf;
}

void Gui::OnMouseButton(int button, int action, int /*mods*/)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        m_mouse_state.buttons.left.is_pressed = action == GLFW_PRESS;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        m_mouse_state.buttons.right.is_pressed = action == GLFW_PRESS;
    }
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        m_mouse_state.buttons.middle.is_pressed = action == GLFW_PRESS;
    }
}

void Gui::OnScroll(double xoffset, double yoffset)
{
    m_mouse_state.scroll.x = etna::narrow_cast<float>(xoffset);
    m_mouse_state.scroll.y = etna::narrow_cast<float>(yoffset);
}

void Gui::OnFramebufferSize(int /*width*/, int /*height*/)
{}

void Gui::OnContentScale(float /*xscale*/, float /*yscale*/)
{}

void Gui::UpdateViewport(etna::Extent2D extent, uint32_t min_image_count)
{
    ImGui_ImplVulkan_SetMinImageCount(min_image_count);
    m_extent = extent;
}

void Gui::Draw(
    etna::CommandBuffer cmd_buffer,
    etna::Framebuffer   framebuffer,
    etna::Semaphore     wait_semaphore,
    etna::Semaphore     signal_semaphore,
    etna::Fence         finished_fence)
{
    using namespace etna;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    for (auto& window : m_windows) {
        window->Draw();
    }

    ImGui::Render();

    auto  render_area = Rect2D{ Offset2D{ 0, 0 }, m_extent };
    auto  clear_color = ClearColor::Transparent;
    auto* draw_data   = ImGui::GetDrawData();

    cmd_buffer.ResetCommandBuffer();
    cmd_buffer.Begin();
    cmd_buffer.BeginRenderPass(framebuffer, render_area, { clear_color });
    ImGui_ImplVulkan_RenderDrawData(draw_data, cmd_buffer);
    cmd_buffer.EndRenderPass();
    cmd_buffer.End();

    m_graphics_queue.Submit(
        cmd_buffer,
        { wait_semaphore },
        { PipelineStage::ColorAttachmentOutput },
        { signal_semaphore },
        finished_fence);

    m_mouse_state.cursor.delta.x = 0;
    m_mouse_state.cursor.delta.y = 0;
    m_mouse_state.scroll.x       = 0;
    m_mouse_state.scroll.y       = 0;
}

bool Gui::IsAnyWindowHovered() const noexcept
{
    return ImGui::IsAnyWindowHovered();
}

void CameraWindow::Draw()
{
    using namespace ImGui;

    Begin("Camera");

    Text("View");

    auto coordinates = m_camera->GetSphericalCoordinates();
    auto offset      = m_camera->GetOffset();
    auto limits      = m_camera->GetLimits();
    auto labels      = std::array{ "Normal", "Inverted" };
    auto label_index = static_cast<int>(coordinates.camera_up);

    bool elevation_changed = SliderAngle(
        "Elevation",
        &coordinates.elevation.value,
        limits.elevation.min.value,
        limits.elevation.max.value,
        "%.1f deg",
        ImGuiSliderFlags_AlwaysClamp);

    bool azimuth_changed = SliderAngle(
        "Azimuth",
        &coordinates.azimuth.value,
        limits.azimuth.min.value,
        limits.azimuth.max.value,
        "%.1f deg",
        ImGuiSliderFlags_AlwaysClamp);

    bool distance_changed = SliderFloat(
        "Distance",
        &coordinates.distance,
        limits.distance.min,
        limits.distance.max,
        nullptr,
        ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);

    bool camera_up_changed = SliderInt("Camera Up", &label_index, 0, 1, labels[label_index]);

    if (elevation_changed || azimuth_changed || distance_changed || camera_up_changed) {
        coordinates.camera_up = static_cast<CameraUp>(label_index);
        m_camera->UpdateSphericalCoordinates(coordinates);
    }

    bool offset_h_changed = SliderFloat("Track H", &offset.horizontal, limits.offset_x.min, limits.offset_x.max);
    bool offset_v_changed = SliderFloat("Track V", &offset.vertical, limits.offset_y.min, limits.offset_y.max);

    if (offset_h_changed || offset_v_changed) {
        m_camera->UpdateOffset(offset);
    }

    auto perspective = m_camera->GetPerspective();

    Text("Perspective");

    bool fovy_changed = SliderAngle(
        "Fov Y",
        &perspective.fovy.value,
        limits.fov_y.min.value,
        limits.fov_y.max.value,
        "%.1f deg",
        ImGuiSliderFlags_AlwaysClamp);

    constexpr auto Success = std::errc();

    auto near_text = std::array<char, 16>();

    std::to_chars(near_text.data(), near_text.data() + near_text.size(), perspective.near);

    bool near_changed = ImGui::InputText(
        "Near",
        near_text.data(),
        sizeof(near_text),
        ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);

    auto far_text = std::array<char, 16>();

    assert(Success == std::to_chars(far_text.data(), far_text.data() + far_text.size(), perspective.far).ec);

    bool far_changed = ImGui::InputText(
        "Far",
        far_text.data(),
        sizeof(far_text),
        ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);

    if (fovy_changed || near_changed || far_changed) {
        std::from_chars(near_text.data(), near_text.data() + near_text.size(), perspective.near);
        std::from_chars(far_text.data(), far_text.data() + far_text.size(), perspective.far);
        m_camera->UpdatePerspective(perspective);
    }

    End();
}
