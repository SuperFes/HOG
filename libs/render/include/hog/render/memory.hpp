#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace hog::render {

class Device;

// Where a buffer's memory lives: on the GPU, or mapped for the CPU to write or read.
enum class Memory : std::uint8_t { Gpu, Upload, Readback };

class Buffer {
public:
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer();

    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
    // Empty for GPU memory. What the CPU writes to upload memory is made visible to the GPU with
    // flush(), and what the GPU writes to readback memory to the CPU with invalidate().
    [[nodiscard]] std::span<std::byte> mapped() const noexcept { return mapped_; }
    Result<> flush() const;
    Result<> invalidate() const;

private:
    friend Result<Buffer> make_buffer(const Device& device, VkDeviceSize size,
                                      VkBufferUsageFlags usage, Memory memory);
    void release() noexcept;

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    std::span<std::byte> mapped_;
};

[[nodiscard]] Result<Buffer> make_buffer(const Device& device, VkDeviceSize size,
                                         VkBufferUsageFlags usage, Memory memory);
// Upload memory holding `bytes`, made visible to the GPU.
[[nodiscard]] Result<Buffer> make_filled_buffer(const Device& device,
                                                std::span<const std::byte> bytes,
                                                VkBufferUsageFlags usage);

struct ImageDesc {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D extent{.width = 1, .height = 1};
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t layers = 1;
    std::uint32_t levels = 1; // of mips, each half the size of the one before
};

// A 2D image on the GPU with a view of the whole of it, viewed as an array when it has more than
// one layer.
class Image {
public:
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    ~Image();

    [[nodiscard]] VkImage handle() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] const ImageDesc& desc() const noexcept { return desc_; }

private:
    friend Result<Image> make_image(const Device& device, const ImageDesc& desc);
    void release() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    ImageDesc desc_;
};

[[nodiscard]] Result<Image> make_image(const Device& device, const ImageDesc& desc);

} // namespace hog::render
