#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>

#include <algorithm>
#include <utility>

namespace hog::render {

namespace {

VkImageViewType view_type(const ImageDesc& desc) noexcept {
    if (desc.cube) {
        return VK_IMAGE_VIEW_TYPE_CUBE;
    }
    if (desc.depth > 1) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    return desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

} // namespace

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)), mapped_(std::exchange(other.mapped_, {})) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release();
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, {});
    }
    return *this;
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
    }
}

Result<> Buffer::flush() const {
    if (const VkResult done = vmaFlushAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE);
        done != VK_SUCCESS) {
        return vulkan_error(done, "making a buffer visible to the GPU");
    }
    return {};
}

Result<> Buffer::invalidate() const {
    if (const VkResult done = vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE);
        done != VK_SUCCESS) {
        return vulkan_error(done, "making a buffer visible to the CPU");
    }
    return {};
}

Result<Buffer> make_buffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
                           Memory memory) {
    const VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .pNext = nullptr,
                                         .flags = 0,
                                         .size = size,
                                         .usage = usage,
                                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                         .queueFamilyIndexCount = 0,
                                         .pQueueFamilyIndices = nullptr};
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    switch (memory) {
    case Memory::Gpu:
        break;
    case Memory::Upload:
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case Memory::Readback:
        allocation_info.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }
    Buffer buffer;
    VmaAllocationInfo allocated{};
    if (const VkResult made = vmaCreateBuffer(device.allocator(), &buffer_info, &allocation_info,
                                              &buffer.buffer_, &buffer.allocation_, &allocated);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a buffer");
    }
    buffer.allocator_ = device.allocator();
    buffer.size_ = size;
    if (allocated.pMappedData != nullptr) {
        buffer.mapped_ = {static_cast<std::byte*>(allocated.pMappedData),
                          static_cast<std::size_t>(size)};
    }
    return buffer;
}

Result<Buffer> make_filled_buffer(const Device& device, std::span<const std::byte> bytes,
                                  VkBufferUsageFlags usage) {
    auto buffer = make_buffer(device, bytes.size(), usage, Memory::Upload);
    if (!buffer) {
        return buffer;
    }
    std::ranges::copy(bytes, buffer->mapped().begin());
    if (auto flushed = buffer->flush(); !flushed) {
        return std::unexpected(std::move(flushed).error());
    }
    return buffer;
}

Image::Image(Image&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), desc_(other.desc_) {}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        desc_ = other.desc_;
    }
    return *this;
}

Image::~Image() {
    release();
}

void Image::release() noexcept {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
        image_ = VK_NULL_HANDLE;
    }
}

bool operator==(const ImageDesc& left, const ImageDesc& right) noexcept {
    return left.format == right.format && left.extent.width == right.extent.width &&
           left.extent.height == right.extent.height && left.usage == right.usage &&
           left.aspect == right.aspect && left.layers == right.layers &&
           left.levels == right.levels && left.cube == right.cube && left.depth == right.depth;
}

Result<Image> make_image(const Device& device, const ImageDesc& desc) {
    if (desc.cube && (desc.layers != 6 || desc.extent.width != desc.extent.height)) {
        return make_error(ErrorCode::InvalidArgument, "a cube needs six square layers");
    }
    if (desc.depth == 0 || (desc.depth > 1 && (desc.layers != 1 || desc.cube))) {
        return make_error(ErrorCode::InvalidArgument, "a 3D image has depth and one layer");
    }
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = desc.cube ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT} : 0,
        .imageType = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = desc.format,
        .extent = {.width = desc.extent.width, .height = desc.extent.height, .depth = desc.depth},
        .mipLevels = desc.levels,
        .arrayLayers = desc.layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = desc.usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    Image image;
    if (const VkResult made = vmaCreateImage(device.allocator(), &image_info, &allocation_info,
                                             &image.image_, &image.allocation_, nullptr);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating an image");
    }
    image.device_ = device.handle();
    image.allocator_ = device.allocator();
    image.desc_ = desc;
    const VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0,
                                          .image = image.image_,
                                          .viewType = view_type(desc),
                                          .format = desc.format,
                                          .components = {},
                                          .subresourceRange = {.aspectMask = desc.aspect,
                                                               .baseMipLevel = 0,
                                                               .levelCount = desc.levels,
                                                               .baseArrayLayer = 0,
                                                               .layerCount = desc.layers}};
    if (const VkResult made = vkCreateImageView(device.handle(), &view_info, nullptr, &image.view_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating an image view");
    }
    return image;
}

} // namespace hog::render
