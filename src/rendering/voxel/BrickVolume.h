#pragma once
#include <vector>
#include <memory>

class BrickVolume
{
private:

public:

	static std::shared_ptr<BrickVolume> Create(int brick_size);
};

