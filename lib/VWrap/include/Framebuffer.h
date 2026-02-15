#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "RenderPass.h"
#include "ImageView.h"
#include <array>

namespace VWrap {

	/// <summary>
	/// Represents a vulkan framebuffer
	/// </summary>
	class Framebuffer {
	private:

		/// <summary>
		/// The underlying vulkan framebuffer
		/// </summary>
		VkFramebuffer m_framebuffer;

		/// <summary>
		/// The device that created this framebuffer
		/// </summary>
		std::shared_ptr<Device> m_device;

		/// <summary>
		/// The render pass that this framebuffer is compatible with
		/// </summary>
		std::shared_ptr<RenderPass> m_render_pass;

		/// <summary>
		/// The attachments of this framebuffer
		/// </summary>
		std::vector<std::shared_ptr<ImageView>> m_attachments;

		/// <summary>
		/// The extent of this framebuffer
		/// </summary>
		VkExtent2D m_extent;

	public:

		/// <summary>
		/// Creates a new 2D framebuffer from the given device, that is compaitble with the given render pass, and has the given color and depth attachments
		/// </summary>
		static std::shared_ptr<Framebuffer> Create2D(std::shared_ptr<Device> device, std::shared_ptr<RenderPass> render_pass, std::vector<std::shared_ptr<ImageView>> attachments, VkExtent2D extent);

		/// <summary>
		/// Gets the underlying vulkan framebuffer
		/// </summary>
		VkFramebuffer Get() const { return m_framebuffer; }

		/// <summary>
		/// Gets the attachments of this framebuffer
		/// </summary>
		std::vector<std::shared_ptr<ImageView>> GetAttachments() const { return m_attachments; }

		/// <summary>
		/// Gets the extent of this framebuffer
		/// </summary>
		VkExtent2D GetExtent() const { return m_extent; }

		/// <summary>
		/// Destroys the underlying vulkan framebuffer
		/// </summary>
		~Framebuffer();
	};
}