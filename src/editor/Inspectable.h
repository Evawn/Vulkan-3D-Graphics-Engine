#pragma once

#include <string>
#include <vector>
#include <functional>

// One editable parameter exposed to the inspector. Used by RenderTechnique and
// PostProcessEffect; in principle by anything else that derives from IInspectable.
struct TechniqueParameter {
	enum Type { Float, Int, Bool, Color3, Color4, Enum, File };
	std::string label;
	Type type;
	void* data = nullptr;
	float min = 0.0f;
	float max = 1.0f;
	std::vector<const char*> enumLabels;

	// File type fields
	std::string* filePath = nullptr;
	std::vector<std::string> fileFilters;                  // e.g. {"obj"}
	std::string fileFilterDesc;                            // e.g. "3D Models"
	std::function<void(const std::string&)> onFileChanged;

	// Generic on-edit hook fired by the inspector after any non-File edit.
	// Use this to reflect a parameter change into render state — e.g. a wireframe
	// toggle that needs to recreate pipelines.
	std::function<void()> onChanged;
};

struct FrameStats {
	uint32_t drawCalls = 0;
	uint32_t vertices = 0;
	uint32_t indices = 0;
};

// Anything inspectable in the editor — render techniques, post-process effects,
// and (eventually) scene graph nodes. The inspector walks one polymorphic surface.
class IInspectable {
public:
	virtual ~IInspectable() = default;

	virtual std::string GetDisplayName() const = 0;

	virtual std::vector<TechniqueParameter>& GetParameters() {
		static std::vector<TechniqueParameter> empty;
		return empty;
	}
};
