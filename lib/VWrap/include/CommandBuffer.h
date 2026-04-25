#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include <vector>
#include "Device.h"
#include "CommandPool.h"
#include "RenderPass.h"
#include "Framebuffer.h"
#include "Image.h"
#include "Buffer.h"
#include "ComputePipeline.h"
#include "Pipeline.h"
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
		/// Records a command to begin the given render pass and framebuffer (2 clear values: color + depth)
		/// </summary>
		void CmdBeginRenderPass(std::shared_ptr<RenderPass> render_pass, std::shared_ptr<Framebuffer> framebuffer);

		/// <summary>
		/// Records a command to begin the given render pass with custom clear values
		/// </summary>
		void CmdBeginRenderPass(std::shared_ptr<RenderPass> render_pass, std::shared_ptr<Framebuffer> framebuffer, const std::vector<VkClearValue>& clearValues);

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
		/// Copies the contents of the source image to the destination buffer
		/// </summary>
		void CmdCopyImageToBuffer(std::shared_ptr<Image> src_image, std::shared_ptr<Buffer> dst_buffer, uint32_t width, uint32_t height);

		/// <summary>
		/// Records a command to end the current render pass
		/// </summary>
		void CmdEndRenderPass() { vkCmdEndRenderPass(m_command_buffer); }

		/// <summary>
		/// Ends recording of the command buffer
		/// </summary>
		void End();

		/// <summary> Dispatches compute workgroups. </summary>
		void CmdDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

		/// <summary> Binds a compute pipeline. </summary>
		void CmdBindComputePipeline(std::shared_ptr<ComputePipeline> pipeline);

		/// <summary> Binds descriptor sets for a compute pipeline. </summary>
		void CmdBindComputeDescriptorSets(VkPipelineLayout layout, const std::vector<VkDescriptorSet>& descriptor_sets, uint32_t first_set = 0);

		// ---- Graphics command wrappers (additive helpers) ----

		/// <summary> Binds a graphics pipeline. </summary>
		void CmdBindGraphicsPipeline(std::shared_ptr<Pipeline> pipeline);

		/// <summary> Binds descriptor sets for a graphics pipeline. </summary>
		void CmdBindGraphicsDescriptorSets(VkPipelineLayout layout, const std::vector<VkDescriptorSet>& descriptor_sets, uint32_t first_set = 0);

		/// <summary> Pushes a constant block to the given graphics pipeline's layout. </summary>
		void CmdPushConstants(std::shared_ptr<Pipeline> pipeline, VkShaderStageFlags stages, const void* data, size_t size, uint32_t offset = 0);

		/// <summary> Pushes a constant block to the given compute pipeline's layout. </summary>
		void CmdPushConstants(std::shared_ptr<ComputePipeline> pipeline, VkShaderStageFlags stages, const void* data, size_t size, uint32_t offset = 0);

		/// <summary> Pushes a constant block to a raw pipeline-layout handle. </summary>
		void CmdPushConstants(VkPipelineLayout layout, VkShaderStageFlags stages, const void* data, size_t size, uint32_t offset = 0);

		/// <summary> Issues a non-indexed draw. </summary>
		void CmdDraw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0);

		/// <summary> Issues an indexed draw. </summary>
		void CmdDrawIndexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0);

		/// <summary> Binds a single vertex buffer to binding 0 (the common case). </summary>
		void CmdBindVertexBuffer(std::shared_ptr<Buffer> buffer, VkDeviceSize offset = 0);

		/// <summary> Binds an index buffer. </summary>
		void CmdBindIndexBuffer(std::shared_ptr<Buffer> buffer, VkIndexType index_type, VkDeviceSize offset = 0);

		/// <summary> Sets a full-extent viewport (top-left at 0,0; depth 0..1). </summary>
		void CmdSetViewport(VkExtent2D extent);

		/// <summary> Sets a full-extent scissor rectangle. </summary>
		void CmdSetScissor(VkExtent2D extent);

		/// <summary> Records a general pipeline barrier with image and buffer memory barriers. </summary>
		void CmdPipelineBarrier(
			VkPipelineStageFlags src_stage,
			VkPipelineStageFlags dst_stage,
			const std::vector<VkImageMemoryBarrier>& image_barriers,
			const std::vector<VkBufferMemoryBarrier>& buffer_barriers = {});

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