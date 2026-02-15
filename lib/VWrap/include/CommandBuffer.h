#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "CommandPool.h"
#include "RenderPass.h"
#include "Framebuffer.h"
#include "Image.h"
#include "Buffer.h"
#include "stb_image.h"

namespace VWrap {

	/// <summary>
	/// Represents a command buffer. Contains static methods for common command buffer operations.
	/// </summary>
	class CommandBuffer
	{
	private:

		/// <summary>
		/// The underlying vulkan command buffer handle.
		/// </summary>
		VkCommandBuffer m_command_buffer{ VK_NULL_HANDLE };

		/// <summary>
		/// The command pool that this command buffer was allocated from.
		/// </summary>
		std::shared_ptr<CommandPool> m_command_pool;

	public:

		/// <summary>
		/// Allocates a new command buffer from the given pool.
		/// </summary>
		/// <param name="command_pool">The command pool to allocate the command buffer from.</param>
		/// <param name="level">The level of the command buffer.</param>
		/// <returns>A shared pointer to the command buffer.</returns>
		static std::shared_ptr<CommandBuffer> Create(std::shared_ptr<CommandPool> command_pool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		/// <summary>
		/// Begins recording the command buffer with the SINGLE_TIME_USE flag.
		/// </summary>
		void BeginSingle();

		/// <summary>
		/// Ends recording of the command buffer, and submits it.
		/// </summary>
		void EndAndSubmit();


		void Begin(VkCommandBufferUsageFlags usage = 0);

		/// <summary>
		/// Records a command to begin the given render pass and framebuffer
		/// </summary>
		void CmdBeginRenderPass(std::shared_ptr<RenderPass> render_pass, std::shared_ptr<Framebuffer> framebuffer);

		/// <summary>
		/// Creates an image at the dst_image handle, and uploads the given texture to it.
		/// </summary>
		static void UploadTextureToImage(std::shared_ptr<CommandPool> command_pool, std::shared_ptr<Allocator> allocator, std::shared_ptr<Image>& dst_image, const char* file_name);

		/// <summary>
		/// Copies the data from the source buffer to this buffer
		/// </summary>
		void CmdCopyBufferToImage(std::shared_ptr<Buffer> src_buffer, std::shared_ptr<Image> dst_image, uint32_t width, uint32_t height, uint32_t depth);

		/// <summary>
		/// Builds the mipmaps for the given image.
		/// </summary>
		void CmdGenerateMipmaps(std::shared_ptr<Image> image, int32_t tex_width, int32_t tex_height);

		/// <summary>
		/// Copies the data from the source buffer to the destination buffer
		/// </summary>
		void CmdCopyBuffer(std::shared_ptr<Buffer> src_buffer, std::shared_ptr<Buffer> dst_buffer, VkDeviceSize size);

		/// <summary>
		/// Submits a pipeline barrier to transition the image layout
		/// </summary>
		void CmdTransitionImageLayout(std::shared_ptr<Image> image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout);

		static void CreateAndFillBrickTexture(std::shared_ptr<CommandPool> command_pool, std::shared_ptr<Allocator> allocator, std::shared_ptr<Image>& dst_image, int brick_size);
		
		/// <summary>
		/// Gets the underlying vulkan command buffer handle.
		/// </summary>
		VkCommandBuffer Get() const { return m_command_buffer; }

		/// <summary>
		/// Gets the command pool that this command buffer was allocated from.
		/// </summary>
		std::shared_ptr<CommandPool> GetCommandPool() const { return m_command_pool; }
	};
}